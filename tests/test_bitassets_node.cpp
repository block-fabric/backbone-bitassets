// Node hardening (core bt/sidechain/node.h, review 2026-10-22) and the duplicate-input rule:
//   - bans: ban_active (dial, inbound connect, per-request backstop)
//   - GetHeaders: walk bounded by height(end)+1, per-connection budget
//   - a transaction spending one outpoint twice: refused by our mempool (policy), and a block
//     carrying one is refused by consensus, as theirs (UtxoDoubleSpent).
#include "test.h"

#include <bitassets/fixture.h>
#include <bitassets/wallet.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <chrono>
#include <cstdio>
#include <thread>
#include <filesystem>
#include <unistd.h>

using namespace bitassets;

static const fixture::Data& data()
{
    static std::optional<fixture::Data> d;
    if (!d) {
        std::string err;
        d = fixture::load(bttest::data_dir() + "/betanet", err);
        if (!d) std::fprintf(stderr, "fixture load: %s\n", err.c_str());
    }
    return *d;
}

TEST(node_ban_active_and_getheaders_budget)
{
    std::map<std::string, int64_t> banned{{"1.2.3.4", 100}};
    CHECK(bt::sidechain::ban_active(banned, "1.2.3.4", 99));
    CHECK(!bt::sidechain::ban_active(banned, "1.2.3.4", 100)); // expired
    CHECK(!bt::sidechain::ban_active(banned, "5.6.7.8", 0));
    bt::sidechain::RequestBudget b;
    for (uint32_t i = 0; i < bt::sidechain::GETHEADERS_PER_MINUTE; i++) CHECK(b.take(1000));
    CHECK(!b.take(1000));
    CHECK(!b.take(1059));
    CHECK(b.take(1060)); // a new window
}

TEST(node_getheaders_budget_survives_reconnects)
{
    using bt::sidechain::HeadersBudgets;
    CHECK_EQ(bt::sidechain::rate_key("1.2.3.4"), std::string("1.2.3.4"));
    CHECK_EQ(bt::sidechain::rate_key("2001:db8:1:2:aaaa::1"), std::string("2001:db8:1:2::/64"));
    CHECK_EQ(bt::sidechain::rate_key("2001:db8:1:2:bbbb::9"), std::string("2001:db8:1:2::/64"));
    HeadersBudgets b;
    // the budget is per IP, not per connection: "reconnecting" (any number of connections from the
    // same IP, or anywhere in the same IPv6 /64) does not reset it
    for (uint32_t i = 0; i < bt::sidechain::GETHEADERS_PER_MINUTE; i++) CHECK(b.take("2001:db8:1:2::" + std::to_string(i + 1), 1000));
    CHECK(!b.take("2001:db8:1:2:ffff::7", 1001));
    CHECK(b.take("2001:db8:1:3::1", 1001)); // another /64
    CHECK(b.take("2001:db8:1:2::1", 1060)); // the next window
    // bounded table: idle entries expire, then the oldest goes
    HeadersBudgets c;
    for (size_t i = 0; i < HeadersBudgets::MAX; i++) c.take("10.0." + std::to_string(i / 256) + "." + std::to_string(i % 256), 0);
    CHECK_EQ(c.by_key.size(), HeadersBudgets::MAX);
    CHECK(c.take("192.168.0.1", 30));
    CHECK(c.by_key.size() <= HeadersBudgets::MAX);
    CHECK(c.take("192.168.0.2", 100)); // everything else idle a whole window: pruned
    CHECK(c.by_key.size() <= 2);
}

TEST(node_rate_key_ipv4_mapped_and_ban_normalised)
{
    using bt::sidechain::rate_key;
    // IPv4-mapped / -compatible IPv6: keyed by the embedded IPv4, not grouped under one /64
    CHECK_EQ(rate_key("::ffff:1.2.3.4"), std::string("1.2.3.4"));
    CHECK_EQ(rate_key("::ffff:5.6.7.8"), std::string("5.6.7.8"));
    CHECK_EQ(rate_key("[::ffff:1.2.3.4]"), std::string("1.2.3.4"));
    CHECK_EQ(rate_key("::1.2.3.4"), std::string("1.2.3.4"));
    CHECK_EQ(rate_key("::ffff:0102:0304"), std::string("1.2.3.4")); // another spelling
    CHECK(rate_key("::ffff:1.2.3.4") != rate_key("::ffff:5.6.7.8"));
    CHECK_EQ(rate_key("2001:db8::1"), std::string("2001:db8::/64"));
    CHECK_EQ(rate_key("fe80::1%eth0"), std::string("fe80::/64"));
    // one ban, whatever the spelling of the address it is checked against
    std::map<std::string, int64_t> banned{{rate_key("1.2.3.4"), 100}};
    CHECK(bt::sidechain::ban_active(banned, "::ffff:1.2.3.4", 50));
    CHECK(bt::sidechain::ban_active(banned, "1.2.3.4", 50));
    CHECK(!bt::sidechain::ban_active(banned, "::ffff:1.2.3.5", 50));
    std::map<std::string, int64_t> banned6{{rate_key("2001:db8:0:1::5"), 100}};
    CHECK(bt::sidechain::ban_active(banned6, "2001:db8:0:1:ffff::9", 50)); // same /64
    CHECK(!bt::sidechain::ban_active(banned6, "2001:db8:0:2::5", 50));
    // budgets: two IPv4 peers over a dual-stack socket do not share one budget
    bt::sidechain::HeadersBudgets b;
    for (uint32_t i = 0; i < bt::sidechain::GETHEADERS_PER_MINUTE; i++) CHECK(b.take("::ffff:1.2.3.4", 0));
    CHECK(!b.take("1.2.3.4", 0)); // same host, other spelling: same budget
    CHECK(b.take("::ffff:5.6.7.8", 0));
}

TEST(node_walk_headers_bounded_by_end_height)
{
    auto& d = data();
    REQUIRE(d.blocks.size() >= 1080);
    bt::kv::MemStore store;
    Archive a;
    std::vector<Hash32> ids;
    {
        bt::kv::Txn t(store);
        for (auto& [h, raw] : d.blocks) {
            Block b = fixture::decode_block_response(raw);
            a.put_header(t, b.header);
            ids.push_back(Chain::hash(b.header));
        }
        t.commit();
    }
    bt::kv::Txn t(store);
    auto all = bt::sidechain::walk_headers<Chain>(t, a, {}, ids.back());
    REQUIRE(all.has_value());
    CHECK_EQ(all->size(), ids.size()); // complete answer from genesis: height(end)+1 headers
    CHECK(Chain::hash(all->front()) == ids.front());
    auto part = bt::sidechain::walk_headers<Chain>(t, a, {ids[500]}, ids.back());
    REQUIRE(part.has_value());
    CHECK_EQ(part->size(), ids.size() - 501);
    CHECK(*part->front().prev_side_hash == ids[500]); // starts right after a `start` hash, as their client checks
    auto mid = bt::sidechain::walk_headers<Chain>(t, a, {}, ids[10]);
    CHECK_EQ(mid->size(), size_t(11));
    Hash32 unknown;
    CHECK(!bt::sidechain::walk_headers<Chain>(t, a, {}, unknown).has_value());
}

// the first betanet block with a transaction that has inputs
static size_t first_block_with_inputs(std::vector<Block>& blocks)
{
    for (auto& [h, raw] : data().blocks) blocks.push_back(fixture::decode_block_response(raw));
    for (size_t i = 0; i < blocks.size(); i++)
        for (auto& tx : blocks[i].body.transactions)
            if (!tx.inputs.empty()) return i;
    return blocks.size();
}

TEST(block_with_a_tx_spending_one_outpoint_twice_is_invalid)
{
    std::vector<Block> blocks;
    size_t k = first_block_with_inputs(blocks);
    REQUIRE(k > 0 && k < blocks.size());
    bt::kv::MemStore store;
    State st;
    auto r = fixture::replay(data(), st, store, nullptr, uint32_t(k - 1));
    REQUIRE(r.error.empty());
    Block b = blocks[k];
    // repeat the first input of the first spending transaction (and its authorization, so the
    // authorizations still line up with the inputs)
    size_t auth_index = 0;
    for (auto& tx : b.body.transactions) {
        if (!tx.inputs.empty()) {
            tx.inputs.push_back(tx.inputs.front());
            b.body.authorizations.insert(b.body.authorizations.begin() + long(auth_index), b.body.authorizations[auth_index]);
            break;
        }
        auth_index += tx.inputs.size();
    }
    bt::kv::Txn t(store);
    // header committing to the modified body (prevalidate checks the merkle root before the inputs)
    std::vector<FilledTransaction> filled;
    for (auto& tx : b.body.transactions) filled.push_back(st.fill(t, tx));
    b.header.merkle_root = *compute_merkle_root(b.body.coinbase, filled);
    std::string err;
    try {
        st.prevalidate(t, b.header, b.body);
    } catch (const StateError& e) {
        err = e.what();
    }
    CHECK_EQ(err, std::string("utxo double spent"));
    // the unmodified block still connects
    auto pre = st.prevalidate(t, blocks[k].header, blocks[k].body);
    CHECK(pre.filled.size() == blocks[k].body.transactions.size());
}

TEST(mempool_refuses_a_tx_spending_one_outpoint_twice)
{
    std::vector<Block> blocks;
    size_t k = first_block_with_inputs(blocks);
    REQUIRE(k < blocks.size());
    AuthorizedTransaction at;
    for (auto& t : blocks[k].body.authorized_transactions())
        if (!t.transaction.inputs.empty()) {
            at = t;
            break;
        }
    at.transaction.inputs.push_back(at.transaction.inputs.front());
    at.authorizations.push_back(at.authorizations.front());
    CHECK(bt::sidechain::spends_an_outpoint_twice(at.transaction));
    CHECK(!bt::sidechain::spends_an_outpoint_twice(blocks[k].body.transactions.front()));

    std::string dir = std::filesystem::temp_directory_path().string() + "/bt-ba-node-" + std::to_string(::getpid());
    std::filesystem::remove_all(dir);
    {
        bt::sidechain::NodeConfig cfg;
        cfg.datadir = dir;
        cfg.network = bt::sidechain::Network::Regtest;
        cfg.p2p = false;
        cfg.use_seeds = false;
        cfg.mainchain_grpc_url = "http://127.0.0.1:9"; // nothing there: no mainchain needed here
        Node n(cfg);
        std::string err;
        REQUIRE(n.start(err));
        std::string what;
        try {
            n.broadcast_transaction(at);
        } catch (const std::exception& e) {
            what = e.what();
        }
        CHECK(what.find("UtxoDoubleSpent") != std::string::npos);
        CHECK(n.mempool().empty());
        n.stop();
    }
    std::filesystem::remove_all(dir);
}

// Over real QUIC: a client asks GetHeaders past the budget, gets closed, reconnects, and is still
// limited (the budget is per IP and survives disconnects).
TEST(node_getheaders_limit_survives_reconnect_over_quic)
{
    std::string dir = std::filesystem::temp_directory_path().string() + "/bt-ba-p2p-" + std::to_string(::getpid());
    std::filesystem::remove_all(dir);
    bt::sidechain::NodeConfig cfg;
    cfg.datadir = dir;
    cfg.network = bt::sidechain::Network::Regtest;
    cfg.use_seeds = false;
    cfg.net_addr = {"127.0.0.1", 49083};
    cfg.mainchain_grpc_url = "http://127.0.0.1:9";
    auto magic = Chain::magic(bt::sidechain::Network::Regtest);
    {
        Node n(cfg);
        std::string err;
        REQUIRE(n.start(err));
        std::atomic<int> closed{0}, connected{0};
        bt::quic::Callbacks cb;
        cb.on_connected = [&](bt::quic::ConnId, const bt::quic::SockAddr&, bool) { ++connected; };
        cb.on_request = [](bt::quic::ConnId, int64_t, Bytes) {};
        cb.on_closed = [&](bt::quic::ConnId, const std::string&) { ++closed; };
        bt::quic::Options opt;
        opt.accept_inbound = false;
        bt::quic::Endpoint ep(cb, opt);
        REQUIRE(ep.start({"127.0.0.1", 0}, err));
        P2P::Request q;
        q.kind = P2P::Request::GetHeaders;
        Bytes payload(magic.begin(), magic.end());
        bt::append(payload, P2P::encode(q));
        auto wait = [](auto pred) {
            for (int i = 0; i < 100 && !pred(); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
            return pred();
        };
        // returns how many requests got an answer before the connection went
        auto session = [&](int requests) -> int {
            int before_conn = connected, before_closed = closed;
            auto c = ep.connect({"127.0.0.1", 49083}, "localhost");
            if (!wait([&] { return connected > before_conn || closed > before_closed; })) return -1;
            std::atomic<int> answered{0}, failed{0};
            for (int i = 0; i < requests; i++) {
                ep.send(c, payload, 1 << 20, std::chrono::milliseconds(3000), [&](std::optional<Bytes> r) { r ? ++answered : ++failed; });
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            wait([&] { return answered + failed == requests; });
            ep.close(c, "done");
            wait([&] { return closed > before_closed; });
            return int(answered);
        };
        int first = session(int(bt::sidechain::GETHEADERS_PER_MINUTE) + 5);
        CHECK_EQ(first, int(bt::sidechain::GETHEADERS_PER_MINUTE)); // then closed
        int second = session(3);
        CHECK_EQ(second, 0); // a fresh connection from the same IP is still limited
        ep.stop();
        n.stop();
    }
    std::filesystem::remove_all(dir);
}

// Kept wallet fix: the Dutch-auction collect our wallet builds (Wallet::build_dutch_auction_collect)
// is valid under the consensus rules as reproduced (prevalidate + connect_prevalidated), on a
// chain built from scratch: three BitAssets registered, an auction of beta for alpha, one bid,
// then the collect once the auction has ended.
namespace {
struct AuctionChain {
    bt::kv::MemStore store;
    State st;
    bt::frost::SigningKey sk;
    Address addr;
    Hash32 alpha, beta, gamma, auction_id;

    AuctionChain()
    {
        bt::ByteArray<64> seed{};
        seed.data()[0] = 7;
        sk = Wallet::tx_signing_key(seed, 0);
        addr = bt::sidechain::address_from_vk(sk.verifying_key());
    }

    void connect(std::vector<Transaction> txs)
    {
        bt::kv::Txn t(store);
        Body b;
        b.transactions = std::move(txs);
        for (auto& tx : b.transactions) {
            Bytes msg = tx_signing_message(tx);
            for (size_t i = 0; i < tx.inputs.size(); i++) b.authorizations.push_back(Authorization{sk.verifying_key(), sk.sign(msg)});
        }
        Header h;
        h.prev_side_hash = st.tip(t);
        std::vector<FilledTransaction> filled;
        for (auto& tx : b.transactions) filled.push_back(st.fill(t, tx));
        h.merkle_root = *compute_merkle_root(b.coinbase, filled);
        auto pre = st.prevalidate(t, h, b);
        st.connect_prevalidated(t, h, b, pre);
        t.commit();
    }

    Output out(Content::Kind k, uint64_t v = 0)
    {
        Output o;
        o.address = addr;
        o.content.kind = k;
        o.content.value = v;
        return o;
    }

    std::vector<std::pair<OutPoint, FilledOutput>> utxos()
    {
        bt::kv::Txn t(store);
        return st.utxos(t);
    }

    // returns the (outpoint, value) of the first BitAsset coin of `id`
    std::pair<OutPoint, uint64_t> coin(const Hash32& id)
    {
        for (auto& [op, o] : utxos())
            if (o.content.is_bitasset() && o.content.id == id) return {op, o.content.value};
        throw std::runtime_error("no coin");
    }

    void build()
    {
        // block 0: three reservations (no inputs); block 1: the registrations
        std::vector<Transaction> res, reg;
        Hash32* ids[] = {&alpha, &beta, &gamma};
        for (uint8_t i = 0; i < 3; i++) {
            Hash32 name{}, nonce{};
            name.data()[0] = uint8_t(0xa0 + i);
            nonce.data()[0] = uint8_t(0x50 + i);
            *ids[i] = name;
            Transaction r;
            r.outputs.push_back(out(Content::BitAssetReservation));
            TxData d;
            d.kind = TxData::BitAssetReservation;
            d.commitment = bt::sidechain::keyed_hash(nonce, name.span());
            r.data = d;
            res.push_back(r);
            Transaction g;
            g.inputs.push_back(OutPoint{OutPoint::Regular, r.txid(), 0});
            g.outputs.push_back(out(Content::BitAsset, 1'000'000));
            g.outputs.push_back(out(Content::BitAssetControl));
            TxData e;
            e.kind = TxData::BitAssetRegistration;
            e.name_hash = name;
            e.revealed_nonce = nonce;
            e.initial_supply = 1'000'000;
            g.data = e;
            reg.push_back(g);
        }
        connect(res);
        connect(reg);
        // block 2: auction of 10,000 beta for alpha, blocks 4..6
        auto [bop, bv] = coin(beta);
        Transaction c;
        c.inputs.push_back(bop);
        c.outputs.push_back(out(Content::BitAsset, bv - 10'000));
        c.outputs.push_back(out(Content::DutchAuctionReceipt));
        TxData cd;
        cd.kind = TxData::DutchAuctionCreate;
        cd.params = DutchAuctionParams{4, 3, AssetId::bitasset(beta), 10'000, AssetId::bitasset(alpha), 5'000, 1'000};
        c.data = cd;
        auction_id = c.txid();
        connect({c});
        connect({}); // 3
        // block 4: a bid of 2,000 alpha for 4,000 beta (quote change first, as our wallet)
        auto [aop, av] = coin(alpha);
        Transaction bid;
        bid.inputs.push_back(aop);
        bid.outputs.push_back(out(Content::BitAsset, av - 2'000));
        bid.outputs.push_back(out(Content::BitAsset, 4'000));
        TxData bd;
        bd.kind = TxData::DutchAuctionBid;
        bd.auction_id = auction_id;
        bd.receive_asset = AssetId::bitasset(beta);
        bd.quantity = 4'000;
        bd.bid_size = 2'000;
        bid.data = bd;
        connect({bid});
        connect({}); // 5
        connect({}); // 6
    }

    std::map<Hash32, uint64_t> supply()
    {
        std::map<Hash32, uint64_t> r;
        for (auto& [op, o] : utxos())
            if (o.content.is_bitasset()) r[o.content.id] += o.content.value;
        return r;
    }

    // the wallet's collect over `coins` (the wallet's BitAsset coins), connected in block 7
    Transaction collect(const std::vector<Hash32>& coin_ids)
    {
        std::optional<std::pair<OutPoint, FilledOutput>> receipt;
        std::vector<std::pair<OutPoint, FilledOutput>> coins;
        for (auto& e : utxos()) {
            if (e.second.content.is_receipt()) receipt = e;
            else if (e.second.content.is_bitasset() && std::find(coin_ids.begin(), coin_ids.end(), e.second.content.id) != coin_ids.end()) coins.push_back(e);
        }
        if (!receipt) throw std::runtime_error("no receipt");
        bt::kv::Txn t(store);
        auto a = st.auction(t, auction_id);
        return Wallet::build_dutch_auction_collect(*receipt, coins, AssetId::bitasset(beta), AssetId::bitasset(alpha),
                                                   a->base_amount_remaining.latest().data, a->quote_amount.latest().data, {addr, addr, addr});
    }
};
} // namespace

TEST(wallet_dutch_auction_collect_is_valid)
{
    for (int variant = 0; variant < 3; variant++) {
        AuctionChain c;
        c.build();
        {
            bt::kv::Txn t(c.store);
            auto a = c.st.auction(t, c.auction_id);
            REQUIRE(a.has_value());
            CHECK_EQ(a->base_amount_remaining.latest().data, uint64_t(6'000));
            CHECK_EQ(a->quote_amount.latest().data, uint64_t(2'000));
        }
        // 0: any coin (a coin of another asset is chosen); 1: quote coins only; 2: base coins only
        std::vector<Hash32> ids = variant == 0 ? std::vector<Hash32>{c.alpha, c.beta, c.gamma}
                                  : variant == 1 ? std::vector<Hash32>{c.alpha}
                                                 : std::vector<Hash32>{c.beta};
        if (variant == 2) {
            // with base coins only and both amounts non-zero the wallet refuses to build a collect
            bool threw = false;
            try {
                c.collect(ids);
            } catch (const WalletError&) {
                threw = true;
            }
            CHECK(threw);
            continue;
        }
        Transaction tx = c.collect(ids);
        REQUIRE(tx.inputs.size() == 2);
        CHECK(tx.inputs[1] == c.coin(variant == 0 ? c.gamma : c.alpha).first);
        auto before = c.supply();
        std::string err;
        try {
            c.connect({tx}); // block 7: the auction ended at 4 + 3
        } catch (const std::exception& e) {
            err = e.what();
        }
        CHECK_EQ(err, std::string());
        // the coin comes back whole; the auction pays out 6,000 beta and 2,000 alpha
        auto after = c.supply();
        CHECK_EQ(after[c.beta], before[c.beta] + 6'000);
        CHECK_EQ(after[c.alpha], before[c.alpha] + 2'000);
        CHECK_EQ(after[c.gamma], before[c.gamma]);
        CHECK_EQ(after[c.alpha], uint64_t(1'000'000));
        CHECK_EQ(after[c.beta], uint64_t(1'000'000));
        CHECK_EQ(after[c.gamma], uint64_t(1'000'000));
        bt::kv::Txn t(c.store);
        auto a = c.st.auction(t, c.auction_id);
        CHECK_EQ(a->base_amount_remaining.latest().data, uint64_t(0));
        CHECK_EQ(a->quote_amount.latest().data, uint64_t(0));
        bool receipt_left = false;
        for (auto& [op, o] : c.st.utxos(t))
            if (o.content.is_receipt()) receipt_left = true;
        CHECK(!receipt_left);
    }
}
