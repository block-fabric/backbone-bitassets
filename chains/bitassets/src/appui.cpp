// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bitassets/appui.h>
#include <bitassets/json.h>

#include <bt/appui/phrase.h>
#include <bt/appui/text.h>
#include <bt/fsperm.h>
#include <bt/hd.h>
#include <bt/log.h>

#include <sodium.h>

#include <algorithm>
#include <cctype>
#include <set>

namespace bitassets {

using bt::appui::Confirmed;
using bt::appui::HostEnv;
using bt::appui::HostError;
using bt::appui::Question;
using bt::appui::Row;
using bt::appui::RpcError;

std::string asset_name_problem(const std::string& name)
{
    // core's rule for free text a trusted dialog shows (valid UTF-8, no Cc/Cf/Zl/Zp/Co/Cs/Cn or default-ignorable code
    // point), a name of one line that reads as it is.
    if (name.empty() || name.size() > 255) return "A name of 1 to 255 bytes";
    if (std::string why{bt::appui::display_text_problem(name)}; !why.empty()) return why;
    if (name.front() == ' ' || name.back() == ' ') return "No spaces around a name";
    return {};
}

std::optional<uint64_t> auction_price_at(const AuctionState& a, uint32_t height)
{
    // dutch_auction.rs bid's price, read only: the same u32 and u64 steps (its own errors are "no price").
    if (height < a.start_block) return std::nullopt;
    const uint32_t last{a.most_recent_bid_block.latest().data};
    if (height < last) return std::nullopt;
    const uint32_t elapsed{height - last};
    const uint64_t e{uint64_t(a.start_block) + uint32_t(a.duration - 1)};
    const uint32_t end_block{e > UINT32_MAX ? UINT32_MAX : uint32_t(e)};
    const uint64_t rem{a.base_amount_remaining.latest().data};
    if (height > end_block || rem == 0) return std::nullopt;
    const uint32_t remaining_duration{end_block - last};
    const uint64_t P{a.price_after_most_recent_bid.latest().data}, E{a.end_price_after_most_recent_bid.latest().data};
    uint64_t price{P};
    if (remaining_duration != 0) {
        if (elapsed / remaining_duration >= 1) return std::nullopt;
        const u128 max_dec{u128(uint64_t(P - E))};
        const u128 dec{max_dec * u128(elapsed) / u128(remaining_duration)};
        if (dec > u128(P)) return std::nullopt;
        price = P - uint64_t(dec);
    }
    if (price == 0) return std::nullopt;
    return price;
}

namespace {
const char* WALLET_NAME = "main";
/** The LayerTwo read that stands for knowing the wallet's addresses: what tells a caller which outputs are the wallet's. */
const char* WALLET_READ = "get_wallet_addresses";
const char* VERSION_TEXT = "0.1.0 (plain_bitassets 0.17.1 compatible)";

const UniValue NULLV;
const UniValue& param(const UniValue& p, const std::string& key) { return p.isObject() ? p.find_value(key) : NULLV; }

std::string str_param(const UniValue& p, const std::string& key)
{
    const UniValue& v{param(p, key)};
    if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, key + ": a string is needed");
    return v.get_str();
}

uint64_t u64_of(const UniValue& v, const std::string& what)
{
    if (!v.isNum()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a whole number is needed");
    const std::string s{v.getValStr()};
    if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a whole number is needed");
    auto n{bt::parse_u64(s)};
    if (!n) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": too large");
    return *n;
}

uint32_t u32_of(const UniValue& v, const std::string& what)
{
    const uint64_t n{u64_of(v, what)};
    if (n > UINT32_MAX) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": too large");
    return static_cast<uint32_t>(n);
}

std::string coins(uint64_t sats) { return bt::appui::format_sats(sats) + " ECX"; }

/** A whole amount of a BitAsset in groups of three digits (BitAssets have no decimals). */
std::string units(uint64_t n)
{
    std::string d{std::to_string(n)}, out;
    for (size_t i{0}; i < d.size(); ++i) {
        if (i && (d.size() - i) % 3 == 0) out += ',';
        out += d[i];
    }
    return out;
}

/** a + b, or an error: no sum of amounts ever wraps. */
uint64_t add_checked(uint64_t a, uint64_t b)
{
    if (a + b < a) throw HostError(bt::appui::HOST_INVALID_REQUEST, "amount overflow");
    return a + b;
}

bool is_value(const FilledOutput& o) { return o.content.is_bitcoin(); }

std::string net_label(const std::string& network) { return network == "betanet" ? "" : network; }

/** Whether a mainchain address is of the mainchain network this node follows (betanet and forknet follow eCash's
 *  betanet, whose addresses are Bitcoin's main ones; regtest's are regtest ones, signet's test ones). */
bool main_address_of_network(const bt::btc::Address& a, const std::string& network)
{
    using A = bt::btc::Address;
    if (a.kind == A::Kind::Segwit) {
        if (network == "regtest") return a.hrp == A::Hrp::Regtest;
        if (network == "signet") return a.hrp == A::Hrp::Testnets;
        return a.hrp == A::Hrp::Mainnet;
    }
    if (network == "regtest" || network == "signet") return a.net == A::NetKind::Test;
    return a.net == A::NetKind::Main;
}

UniValue n64(uint64_t v) { return UniValue(v); }

/** An AssetId of the LayerTwo JSON (the hex of its borsh bytes: 00 ECX, 01<id> a BitAsset, 02<id> its control). */
AssetId asset_of(const UniValue& v, const std::string& what)
{
    if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": an asset id (hex)");
    auto b{bt::hex_decode(v.get_str())};
    if (!b) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": an asset id (hex)");
    auto a{AssetId::from_borsh(*b)};
    if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": not an asset id");
    return *a;
}

std::optional<Hash32> parse_hash(const std::string& s)
{
    auto b{bt::hex_to_array<32>(s)};
    if (!b) return std::nullopt;
    return Hash32{*b};
}

Hash32 hash_of(const UniValue& v, const std::string& what)
{
    if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": 64 hex digits");
    auto h{parse_hash(v.get_str())};
    if (!h) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": 64 hex digits");
    return *h;
}

/** The kind of an output in the pages' words. */
const char* content_kind(Content::Kind k)
{
    switch (k) {
    case Content::AmmLpToken: return "lp";
    case Content::Bitcoin: return "value";
    case Content::BitAsset: return "asset";
    case Content::BitAssetControl: return "control";
    case Content::BitAssetReservation: return "reservation";
    case Content::DutchAuctionReceipt: return "receipt";
    case Content::Withdrawal: return "withdrawal";
    }
    return "value";
}

const char* filled_kind(FilledContent::Kind k)
{
    switch (k) {
    case FilledContent::AmmLpToken: return "lp";
    case FilledContent::Bitcoin: return "value";
    case FilledContent::BitcoinWithdrawal: return "withdrawal";
    case FilledContent::BitAsset: return "asset";
    case FilledContent::BitAssetControl: return "control";
    case FilledContent::BitAssetReservation: return "reservation";
    case FilledContent::DutchAuctionReceipt: return "receipt";
    }
    return "value";
}
} // namespace

struct AppLayer::Impl {
    App& app;
    Node& node;
    Wallet& wallet;
    std::string network, datadir;
    bt::appui::AppOptions options;
    bt::jsonrpc::Server l2l;
    std::unique_ptr<bt::appui::AppStore> store;
    std::unique_ptr<bt::appui::LinkMonitor> link;
    std::unique_ptr<bt::appui::Poller> poller;
    bt::appui::PendingPhrases phrases;
    std::chrono::system_clock::time_point started{std::chrono::system_clock::now()};

    Impl(App& a, std::string net, std::string dir, bt::appui::AppOptions o)
        : app(a), node(a.node), wallet(a.wallet), network(std::move(net)), datadir(std::move(dir)), options(std::move(o))
    {
        // Every LayerTwo method, on a server that does not listen: the reads the pages make and the spends run after
        // a yes go through the very handlers (and JSON) of the LayerTwo RPC.
        register_rpc(l2l, app, true, true);
    }

    /** One LayerTwo method, through its own handler: its result, or its error as an app RPC error. */
    UniValue l2l_call(const std::string& method, const UniValue& params)
    {
        UniValue req{UniValue::VOBJ};
        req.pushKV("jsonrpc", "2.0");
        req.pushKV("id", 1);
        req.pushKV("method", method);
        req.pushKV("params", params.isNull() ? UniValue{UniValue::VARR} : params);
        UniValue resp;
        if (!resp.read(l2l.handle(req.write())) || !resp.isObject()) throw RpcError(bt::appui::RPC_INTERNAL_ERROR, "no answer from " + method);
        const UniValue& err{resp.find_value("error")};
        if (err.isObject()) {
            const UniValue& data{err.find_value("data")};
            throw RpcError(err.find_value("code").isNum() ? err.find_value("code").getInt<int>() : -1,
                           err.find_value("message").getValStr() + (data.isStr() ? ": " + data.get_str() : std::string{}));
        }
        return resp.find_value("result");
    }

    std::set<Address> addresses()
    {
        auto a{wallet.addresses()};
        return {a.begin(), a.end()};
    }

    std::optional<std::pair<uint32_t, Hash32>> block_of(const Hash32& block)
    {
        auto h{node.block_height(block)};
        auto hd{node.header(block)};
        if (!h || !hd) return std::nullopt;
        return std::make_pair(*h, hd->prev_main_hash);
    }

    std::optional<uint64_t> main_time(const Hash32& main) { return link ? link->block_time(main) : std::nullopt; }

    std::optional<std::pair<Hash32, std::optional<uint32_t>>> followed()
    {
        auto tip{node.tip()};
        if (!tip) return std::nullopt;
        auto hd{node.header(*tip)};
        if (!hd) return std::nullopt;
        return std::make_pair(hd->prev_main_hash, link ? link->block_height(hd->prev_main_hash) : std::nullopt);
    }

    /** The height the next block will have (what a bid, a collect or an auction's start is measured by). */
    uint32_t next_height()
    {
        auto h{node.height()};
        return h ? *h + 1 : 0;
    }

    // ---------------------------------------------------------- names of assets
    /** The name of a BitAsset in words, when this wallet knows it (it reserved or registered it, or the app looked it up);
     *  only ever for a reader of the wallet. */
    std::optional<std::string> known_name(const Hash32& id)
    {
        auto n{wallet.plain_name(id)};
        if (!n) n = store->get("assetname:" + bt::hex_encode(id));
        if (n && (!asset_name_problem(*n).empty() || name_hash(*n) != id)) return std::nullopt;
        return n;
    }

    /** An asset in a question or a row: ECX, its name in words (the wallet's own knowledge, checked against its id), or
     *  the start of its id. */
    std::string label(const AssetId& a)
    {
        if (a.kind == AssetId::Bitcoin) return "ECX";
        std::string base;
        if (auto n{known_name(a.id)}) base = *n;
        else base = "asset " + bt::hex_encode(a.id).substr(0, 10);
        return a.kind == AssetId::BitAssetControl ? "control coin of " + base : base;
    }

    UniValue asset_json(const AssetId& a, bool reader)
    {
        UniValue o{UniValue::VOBJ};
        o.pushKV("assetId", json::asset_id(a));
        o.pushKV("kind", a.kind == AssetId::Bitcoin ? "ecx" : a.kind == AssetId::BitAsset ? "asset" : "control");
        if (a.kind != AssetId::Bitcoin) o.pushKV("id", bt::hex_encode(a.id));
        if (reader && a.kind != AssetId::Bitcoin) {
            if (auto n{known_name(a.id)}) o.pushKV("name", *n);
        }
        return o;
    }

    // ---------------------------------------------------------- wallet
    struct Balances {
        uint64_t available{0}, withdrawing{0}, pending_in{0}, pending_out{0};
    };

    Balances balances(const std::set<Address>& mine)
    {
        Balances b;
        for (auto& [op, o] : node.utxos_by_addresses(mine)) {
            if (o.content.is_withdrawal()) b.withdrawing = add_checked(b.withdrawing, o.bitcoin_value());
            else if (is_value(o)) b.available = add_checked(b.available, o.bitcoin_value());
        }
        auto [txs, fees] = node.mempool_transactions(1000);
        (void)fees;
        for (auto& [atx, spent] : txs) {
            for (const FilledOutput& s : spent) {
                if (mine.contains(s.address) && is_value(s)) b.pending_out = add_checked(b.pending_out, s.bitcoin_value());
            }
            for (const Output& o : atx.transaction.outputs) {
                if (mine.contains(o.address) && o.content.kind == Content::Bitcoin) b.pending_in = add_checked(b.pending_in, o.bitcoin_value());
            }
        }
        return b;
    }

    std::string wallet_id()
    {
        // The fingerprint of the seed, or none: a token for one wallet is never taken for another.
        return seed_fingerprint();
    }

    /** bt::appui::seed_fingerprint of the wallet's seed (the same definition), "" without one. */
    std::string seed_fingerprint()
    {
        auto d{wallet.seed_digest("sidechain/dropin/fingerprint/v1")};
        return d ? bt::hex_encode(bt::ByteSpan{d->data(), 8}) : std::string{};
    }

    /** Set the wallet's seed from a phrase checked before; the phrase kept for the backup (owner-only store). */
    UniValue set_phrase(const std::string& phrase, const std::string& from)
    {
        auto p{bt::hd::parse_mnemonic(phrase)};
        if (!p) throw HostError(bt::hd::mnemonic_error(phrase));
        const bt::appui::WipeOnExit wipe_parsed{*p};
        auto seed{bt::hd::mnemonic_to_seed(*p)};
        const std::string fp{bt::appui::seed_fingerprint(seed.span())};
        try {
            wallet.set_seed(seed);
        } catch (const std::exception& e) {
            sodium_memzero(seed.data(), seed.size());
            throw HostError(bt::appui::RPC_WALLET_ERROR, std::string{"This wallet has a backup phrase already ("} + e.what() + ")");
        }
        sodium_memzero(seed.data(), seed.size());
        store->put("phrase", *p);
        store->put("phrase.from", from);
        if (poller) poller->poke();
        UniValue out{UniValue::VOBJ};
        out.pushKV("fingerprint", fp);
        return out;
    }

    /** The phrase the app layer kept, only when it is the one of the wallet's seed now (a wallet file replaced since: none). */
    std::optional<std::string> kept_phrase()
    {
        auto phrase{store->get("phrase")};
        if (!phrase || !wallet.has_seed()) return std::nullopt;
        auto p{bt::hd::parse_mnemonic(*phrase)};
        if (!p) {
            sodium_memzero(phrase->data(), phrase->size());
            return std::nullopt;
        }
        auto seed{bt::hd::mnemonic_to_seed(*p)};
        sodium_memzero(p->data(), p->size());
        const bool same{bt::appui::seed_fingerprint(seed.span()) == seed_fingerprint()};
        sodium_memzero(seed.data(), seed.size());
        if (!same) {
            sodium_memzero(phrase->data(), phrase->size());
            return std::nullopt;
        }
        return phrase;
    }

    UniValue seed_status()
    {
        UniValue s{UniValue::VOBJ};
        const bool has{wallet.has_seed()};
        std::optional<std::string> kept{has ? kept_phrase() : std::nullopt};
        s.pushKV("hasSeed", has);
        s.pushKV("phraseKept", kept.has_value());
        if (kept) sodium_memzero(kept->data(), kept->size());
        s.pushKV("from", has ? store->get("phrase.from").value_or("outside") : "");
        s.pushKV("fingerprint", has ? seed_fingerprint() : "");
        s.pushKV("derivation", "plain_bitassets");
        return s;
    }

    static std::string words_block(const std::string& phrase)
    {
        std::string out;
        size_t n{0}, at{0};
        while (at < phrase.size()) {
            size_t sp{phrase.find(' ', at)};
            if (sp == std::string::npos) sp = phrase.size();
            ++n;
            out += (n < 10 ? " " : "") + std::to_string(n) + ". " + phrase.substr(at, sp - at) + (n % 3 == 0 ? "\n" : "    ");
            at = sp + 1;
        }
        return out;
    }

    // ---------------------------------------------------------- history
    struct AssetMove {
        uint64_t in{0}, out{0};
    };
    struct Hist {
        std::string id, kind;
        int64_t delta{0};
        uint64_t received{0}, spent{0}, withdrawing{0};
        bool pending{false}, ours_in{false};
        std::optional<Hash32> txid, block, m6id;
        std::vector<std::pair<std::string, uint64_t>> to; // outputs to others
        std::optional<uint64_t> fee;
        std::string main_address;
        //! The wallet's address it paid (what a receipt was received at).
        std::string own_address;
        std::map<AssetId, AssetMove> assets; // BitAssets in and out of the wallet
        bool reservation{false};
        std::string reserved;          // the name a reservation of the wallet commits to, in words
        std::optional<TxData::Kind> data;
        std::optional<Hash32> issued;  // a registration: the asset's id
    };

    /** The block of the active chain a transaction is in. */
    std::optional<Hash32> block_of_tx(const Hash32& txid)
    {
        for (auto& [blk, idx] : node.tx_inclusions(txid)) {
            auto h{node.block_height(blk)};
            if (h && node.block_hash_at(*h) == blk) return blk;
        }
        return std::nullopt;
    }

    std::optional<std::pair<Hash32, BundleStatus>> bundle_status(const Hash32& m6id)
    {
        std::lock_guard<std::recursive_mutex> l(node.lock());
        bt::kv::Txn t(node.store());
        auto b{node.state().bundle(t, m6id)};
        if (!b) return std::nullopt;
        return std::make_pair(m6id, b->second.latest().value);
    }

    std::string seen_key(const std::string& id) { return "seen:" + id; }

    std::optional<std::pair<int64_t, int64_t>> seen(const std::string& id)
    {
        auto v{store->get(seen_key(id))};
        if (!v) return std::nullopt;
        const size_t c{v->find(':')};
        if (c == std::string::npos) return std::nullopt;
        return std::make_pair(std::atoll(v->substr(0, c).c_str()), std::atoll(v->substr(c + 1).c_str()));
    }

    void note_seen(const std::string& id)
    {
        if (store->get(seen_key(id))) return;
        auto h{node.height()};
        const int64_t now{std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count()};
        store->put(seen_key(id), std::to_string(h ? int64_t(*h) : int64_t{-1}) + ":" + std::to_string(now));
    }

    static void move_in(Hist& h, const AssetId& a, uint64_t v)
    {
        auto& m{h.assets[a]};
        m.in = m.in + v < m.in ? UINT64_MAX : m.in + v;
    }
    static void move_out(Hist& h, const AssetId& a, uint64_t v)
    {
        auto& m{h.assets[a]};
        m.out = m.out + v < m.out ? UINT64_MAX : m.out + v;
    }

    std::map<std::string, Hist> gather(const std::set<Address>& mine)
    {
        std::map<std::string, Hist> rows;
        auto row_of = [&](const OutPoint& op) -> Hist& {
            std::string id;
            std::string kind;
            if (op.kind == OutPoint::Deposit) {
                id = "deposit:" + bt::hex_encode_rev(op.txid.span());
                kind = "deposit";
            } else if (op.kind == OutPoint::Coinbase) {
                id = "coinbase:" + bt::hex_encode(op.txid);
                kind = "mined";
            } else {
                id = bt::hex_encode(op.txid);
            }
            Hist& h{rows[id]};
            if (h.id.empty()) {
                h.id = id;
                h.kind = kind;
                if (op.kind == OutPoint::Regular) h.txid = op.txid;
            }
            return h;
        };
        auto credit = [&](const OutPoint& op, const FilledOutput& o) {
            Hist& h{row_of(op)};
            if (o.content.is_withdrawal()) {
                h.withdrawing += o.bitcoin_value();
                h.main_address = o.content.main_address.to_string();
            } else if (o.content.is_bitasset()) {
                move_in(h, AssetId::bitasset(o.content.id), o.content.value);
            } else if (o.content.is_reservation()) {
                h.reservation = true;
                if (auto plain{wallet.reserved_name(o.content.commitment)}) h.reserved = *plain;
            } else if (is_value(o)) {
                h.received += o.bitcoin_value();
                h.delta += static_cast<int64_t>(o.bitcoin_value());
                if (h.own_address.empty()) h.own_address = o.address.to_base58();
            }
        };
        for (auto& [op, o] : node.utxos_by_addresses(mine)) credit(op, o);
        for (auto& [op, s] : node.stxos_by_addresses(mine)) {
            credit(op, s.output);
            std::string id;
            if (s.inpoint.kind == InPoint::Withdrawal) {
                id = "bundle:" + bt::hex_encode_rev(s.inpoint.id.span());
                Hist& h{rows[id]};
                if (h.id.empty()) {
                    h.id = id;
                    h.kind = "bundle";
                    h.m6id = s.inpoint.id;
                }
                // Our withdrawal paid out of the chain into a bundle: counted when it was asked for.
                if (s.output.content.is_withdrawal()) h.withdrawing += s.output.bitcoin_value();
                else if (is_value(s.output)) {
                    h.spent += s.output.bitcoin_value();
                    h.delta -= static_cast<int64_t>(s.output.bitcoin_value());
                }
                continue;
            }
            id = bt::hex_encode(s.inpoint.id);
            Hist& h{rows[id]};
            if (h.id.empty()) {
                h.id = id;
                h.txid = s.inpoint.id;
            }
            h.ours_in = true;
            if (is_value(s.output)) {
                h.spent += s.output.bitcoin_value();
                h.delta -= static_cast<int64_t>(s.output.bitcoin_value());
            } else if (s.output.content.is_bitasset()) {
                move_out(h, AssetId::bitasset(s.output.content.id), s.output.content.value);
            }
        }
        // What waits in the mempool: our spends and what comes in.
        auto [txs, fees] = node.mempool_transactions(1000);
        (void)fees;
        for (auto& [atx, spent] : txs) {
            const Transaction& tx{atx.transaction};
            const Hash32 txid{tx.txid()};
            bool touches{false};
            for (const FilledOutput& s : spent) touches = touches || mine.contains(s.address);
            for (const Output& o : tx.outputs) touches = touches || mine.contains(o.address);
            if (!touches) continue;
            Hist& h{rows[bt::hex_encode(txid)]};
            h.id = bt::hex_encode(txid);
            h.txid = txid;
            h.pending = true;
            h.received = h.spent = h.withdrawing = 0;
            h.delta = 0;
            h.assets.clear();
            uint64_t in_total{0}, out_total{0};
            bool all_inputs_ours{true};
            for (const FilledOutput& s : spent) {
                in_total = in_total + s.bitcoin_value() < in_total ? UINT64_MAX : in_total + s.bitcoin_value();
                if (mine.contains(s.address)) {
                    h.ours_in = true;
                    if (is_value(s)) {
                        h.spent += s.bitcoin_value();
                        h.delta -= static_cast<int64_t>(s.bitcoin_value());
                    } else if (s.content.is_bitasset()) {
                        move_out(h, AssetId::bitasset(s.content.id), s.content.value);
                    }
                } else {
                    all_inputs_ours = false;
                }
            }
            h.to.clear();
            // The outputs as they will be filled (a BitAsset output's asset only so).
            FilledTransaction ft{&tx, spent};
            auto filled{ft.filled_outputs()};
            for (size_t i{0}; i < tx.outputs.size(); ++i) {
                const Output& o{tx.outputs[i]};
                out_total = out_total + o.bitcoin_value() < out_total ? UINT64_MAX : out_total + o.bitcoin_value();
                if (mine.contains(o.address)) {
                    if (o.content.kind == Content::Withdrawal) {
                        h.withdrawing += o.bitcoin_value();
                        h.main_address = o.content.main_address.to_string();
                    } else if (o.content.kind == Content::BitAssetReservation) {
                        h.reservation = true;
                        if (tx.data && tx.data->kind == TxData::BitAssetReservation) {
                            if (auto plain{wallet.reserved_name(tx.data->commitment)}) h.reserved = *plain;
                        }
                    } else if (o.content.kind == Content::BitAsset) {
                        if (filled && i < filled->size()) move_in(h, AssetId::bitasset((*filled)[i].content.id), o.content.value);
                    } else if (o.content.kind == Content::Bitcoin) {
                        h.received += o.bitcoin_value();
                        h.delta += static_cast<int64_t>(o.bitcoin_value());
                        if (h.own_address.empty()) h.own_address = o.address.to_base58();
                    }
                } else if (o.content.kind == Content::Bitcoin) {
                    h.to.emplace_back(o.address.to_base58(), o.bitcoin_value());
                }
            }
            if (tx.data) h.data = tx.data->kind;
            if (tx.data && tx.data->kind == TxData::BitAssetRegistration) h.issued = tx.data->name_hash;
            if (all_inputs_ours && in_total >= out_total) h.fee = in_total - out_total;
        }
        return rows;
    }

    static std::string kind_of_data(TxData::Kind k)
    {
        switch (k) {
        case TxData::AmmBurn: return "pool-remove";
        case TxData::AmmMint: return "pool-add";
        case TxData::AmmSwap: return "swap";
        case TxData::BitAssetReservation: return "asset-reserve";
        case TxData::BitAssetRegistration: return "asset-issue";
        case TxData::BitAssetMint: return "asset-mint";
        case TxData::BitAssetUpdate: return "asset-update";
        case TxData::DutchAuctionCreate: return "auction-create";
        case TxData::DutchAuctionBid: return "auction-bid";
        case TxData::DutchAuctionCollect: return "auction-collect";
        }
        return "";
    }

    UniValue history(size_t limit)
    {
        const auto mine{addresses()};
        auto rows{gather(mine)};
        struct Out {
            UniValue v;
            int64_t order_height;
            int64_t order_time;
        };
        std::vector<Out> list;
        for (auto& [id, h] : rows) {
            note_seen(id);
            std::optional<uint32_t> height;
            std::optional<uint64_t> time;
            std::string block_hex;
            // A transaction of a block: where, when (the time of the mainchain block it was merged-mined in), what it is,
            // its other outputs and its fee.
            if (h.txid && !h.pending) {
                if (auto t{node.transaction(*h.txid)}) {
                    if (auto blk{block_of_tx(*h.txid)}) {
                        block_hex = bt::hex_encode(*blk);
                        if (auto b{block_of(*blk)}) {
                            height = b->first;
                            time = main_time(b->second);
                        }
                    }
                    if (t->data) h.data = t->data->kind;
                    if (t->data && t->data->kind == TxData::BitAssetRegistration) h.issued = t->data->name_hash;
                    h.to.clear();
                    uint64_t out_total{0};
                    for (const Output& o : t->outputs) {
                        out_total = out_total + o.bitcoin_value() < out_total ? UINT64_MAX : out_total + o.bitcoin_value();
                        if (!mine.contains(o.address) && o.content.kind == Content::Bitcoin) h.to.emplace_back(o.address.to_base58(), o.bitcoin_value());
                    }
                    // Every input ours (what we spent of value and withdrawals is the whole input): the fee.
                    if (h.ours_in && t->inputs.size() > 0 && h.spent >= out_total) h.fee = h.spent - out_total;
                }
            }
            bool approx{false};
            if (!height || !time) {
                if (auto s{seen(id)}) {
                    if (!height && s->first >= 0) height = static_cast<uint32_t>(s->first);
                    if (!time) time = static_cast<uint64_t>(s->second);
                    approx = true;
                }
            }
            std::erase_if(h.assets, [](const auto& e) { return e.second.in == e.second.out; });
            std::string kind{h.kind};
            if (kind.empty()) {
                if (h.data) kind = kind_of_data(*h.data);
                else if (!h.assets.empty()) kind = h.ours_in ? "asset-send" : "asset-receive";
                else if (h.withdrawing > 0) kind = "withdrawal";
                else if (!h.ours_in) kind = "receive";
                else if (h.to.empty()) kind = "self";
                else kind = "send";
            }
            UniValue o{UniValue::VOBJ};
            o.pushKV("id", id);
            o.pushKV("kind", kind);
            if (h.txid) o.pushKV("txid", bt::hex_encode(*h.txid));
            if (h.m6id) {
                o.pushKV("m6id", bt::hex_encode_rev(h.m6id->span()));
                if (auto b{bundle_status(*h.m6id)}) o.pushKV("bundleStatus", to_string(b->second));
            }
            if (!h.assets.empty()) {
                UniValue as{UniValue::VARR};
                for (auto& [a, mv] : h.assets) {
                    UniValue x{asset_json(a, true)};
                    x.pushKV("label", label(a));
                    x.pushKV("amount", mv.in >= mv.out ? "+" + std::to_string(mv.in - mv.out) : "-" + std::to_string(mv.out - mv.in));
                    as.push_back(x);
                }
                o.pushKV("assets", as);
            }
            if (h.issued) {
                UniValue names{UniValue::VARR};
                names.push_back(label(AssetId::bitasset(*h.issued)));
                o.pushKV("names", names);
            }
            o.pushKV("amount", static_cast<int64_t>(h.delta));
            o.pushKV("received", n64(h.received));
            o.pushKV("sent", n64(h.spent));
            if (h.withdrawing) o.pushKV("withdrawing", n64(h.withdrawing));
            if (!h.main_address.empty()) o.pushKV("mainAddress", h.main_address);
            if (!h.own_address.empty()) o.pushKV("ownAddress", h.own_address);
            if (!h.reserved.empty()) o.pushKV("reservedName", h.reserved);
            if (h.fee) o.pushKV("fee", n64(*h.fee));
            UniValue to{UniValue::VARR};
            for (auto& [a, v] : h.to) {
                UniValue t{UniValue::VOBJ};
                t.pushKV("address", a);
                t.pushKV("amount", n64(v));
                to.push_back(t);
                if (to.size() >= 50) break;
            }
            o.pushKV("to", to);
            o.pushKV("recipients", static_cast<uint64_t>(h.to.size()));
            o.pushKV("pending", h.pending);
            o.pushKV("height", height ? UniValue(uint64_t(*height)) : UniValue{});
            o.pushKV("time", time ? UniValue(*time) : UniValue{});
            o.pushKV("approxTime", approx);
            if (!block_hex.empty()) o.pushKV("block", block_hex);
            list.push_back(Out{o, h.pending ? INT64_MAX : height ? int64_t(*height) : -1, time ? int64_t(*time) : 0});
        }
        std::sort(list.begin(), list.end(), [](const Out& a, const Out& b) {
            if (a.order_height != b.order_height) return a.order_height > b.order_height;
            if (a.order_time != b.order_time) return a.order_time > b.order_time;
            return a.v.find_value("id").get_str() < b.v.find_value("id").get_str();
        });
        UniValue out{UniValue::VARR};
        for (auto& o : list) {
            if (out.size() >= limit) break;
            out.push_back(std::move(o.v));
        }
        return out;
    }

    // ---------------------------------------------------------- the node as events
    bt::appui::NodeLook look()
    {
        bt::appui::NodeLook l;
        // Its height, its peers and the best height they told of, the mainchain read: the sync status (sync_state).
        bt::appui::look_at_node(l, node, network);
        if (auto t{node.tip()}) {
            l.tip = bt::hex_encode(*t);
            if (auto hd{node.header(*t)}) {
                if (auto tm{main_time(hd->prev_main_hash)}) l.tip_time = *tm;
            }
        }
        // In step with eCash: as fresh as eCash's tip (blocks of this chain come only when merge-mined); behind it, as old
        // as what this chain last acted on.
        if (link && link->connected() && link->info().find_value("state").getValStr() == "ok") {
            if (auto tm{link->tip_time()}) l.tip_time = std::max(l.tip_time, *tm);
        }
        l.wallet = wallet.has_seed();
        if (l.wallet) {
            const auto mine{addresses()};
            const Balances b{balances(mine)};
            l.trusted = b.available;
            l.pending = b.pending_in;
            l.immature = b.withdrawing;
            for (auto& [id, h] : gather(mine)) {
                l.txs[id] = !h.pending;
                note_seen(id);
            }
        }
        if (link && !link->connected()) l.warnings.push_back("The mainchain interface does not answer: this chain cannot follow it.");
        return l;
    }

    UniValue info()
    {
        UniValue i{UniValue::VOBJ};
        i.pushKV("name", "BitAssets");
        i.pushKV("version", VERSION_TEXT);
        i.pushKV("bugreport", "https://github.com/block-fabric/backbone-bitassets/issues");
        i.pushKV("network", network);
        i.pushKV("networkLabel", net_label(network));
        i.pushKV("datadir", "");
        i.pushKV("blockSpacing", 600);
        i.pushKV("walletEnabled", true);
        i.pushKV("devtools", false);
        i.pushKV("language", "");
        i.pushKV("test", false);
        i.pushKV("platform", "other");
        i.pushKV("dropin", "bitassets");
        i.pushKV("slot", static_cast<int>(THIS_SIDECHAIN));
        return i;
    }

    // ---------------------------------------------------------- explorer
    UniValue block_summary(uint32_t height, const Hash32& hash, const Header& hd, const Body& b)
    {
        UniValue o{UniValue::VOBJ};
        o.pushKV("height", static_cast<uint64_t>(height));
        o.pushKV("hash", bt::hex_encode(hash));
        o.pushKV("mainHash", bt::hex_encode_rev(hd.prev_main_hash.span()));
        if (auto t{main_time(hd.prev_main_hash)}) o.pushKV("time", *t);
        uint64_t fees{0};
        for (const Output& c : b.coinbase.outputs) fees = fees + c.bitcoin_value() < fees ? UINT64_MAX : fees + c.bitcoin_value();
        o.pushKV("txs", static_cast<uint64_t>(b.transactions.size()));
        o.pushKV("inputs", static_cast<uint64_t>(b.inputs_len()));
        size_t outs{0};
        for (const auto& tx : b.transactions) outs += tx.outputs.size();
        o.pushKV("outputs", static_cast<uint64_t>(outs));
        o.pushKV("fees", n64(fees));
        o.pushKV("bytes", static_cast<uint64_t>(borsh_bytes(b).size()));
        o.pushKV("limit", n64(Body::MAX_SIZE));
        return o;
    }

    /** Which addresses are the wallet's, for a caller that may read the wallet's addresses; nothing for another (its chain
     *  reads must not tell it which outputs are the wallet's). */
    using Mine = std::optional<std::set<Address>>;
    Mine mine_for(HostEnv& env) { return env.may_call(WALLET_READ) ? Mine{addresses()} : std::nullopt; }

    UniValue output_json(const Output& o, const Mine& mine)
    {
        UniValue r{UniValue::VOBJ};
        r.pushKV("address", o.address.to_base58());
        r.pushKV("amount", n64(o.content.kind == Content::Bitcoin || o.content.kind == Content::Withdrawal ? o.bitcoin_value() : 0));
        r.pushKV("kind", content_kind(o.content.kind));
        if (o.content.kind == Content::Withdrawal) {
            r.pushKV("mainAddress", o.content.main_address.to_string());
            r.pushKV("mainFee", n64(o.content.main_fee));
        }
        if (o.content.kind == Content::BitAsset || o.content.kind == Content::AmmLpToken) r.pushKV("units", std::to_string(o.content.value));
        if (!o.memo.empty()) r.pushKV("memo", bt::hex_encode(o.memo));
        if (mine) r.pushKV("mine", mine->contains(o.address));
        return r;
    }

    UniValue output_json(const FilledOutput& o, const Mine& mine)
    {
        UniValue r{output_json(o.to_output(), mine)};
        const auto& c{o.content};
        r.pushKV("kind", filled_kind(c.kind));
        if (c.is_bitasset() || c.is_control()) {
            r.pushKV("asset", bt::hex_encode(c.id));
            // The name in words only to a reader of the wallet (the wallet knows the names it reserved or looked up).
            if (mine) {
                if (auto n{known_name(c.id)}) r.pushKV("assetName", *n);
            }
        } else if (c.is_lp_token()) {
            r.pushKV("asset0", json::asset_id(c.asset0));
            r.pushKV("asset1", json::asset_id(c.asset1));
        } else if (c.is_receipt()) {
            r.pushKV("auction", bt::hex_encode(c.id));
        }
        return r;
    }

    /** A transaction's outputs as filled (on the active chain or in the mempool), else as they are. */
    UniValue outputs_json(const Transaction& tx, const Mine& mine)
    {
        UniValue outs{UniValue::VARR};
        std::optional<std::vector<FilledOutput>> filled;
        if (auto f{node.filled_transaction(tx.txid())}) {
            FilledTransaction ft{&tx, f->first.second};
            filled = ft.filled_outputs();
        }
        for (size_t i{0}; i < tx.outputs.size(); ++i) {
            if (filled && i < filled->size()) outs.push_back(output_json((*filled)[i], mine));
            else outs.push_back(output_json(tx.outputs[i], mine));
        }
        return outs;
    }

    UniValue data_json(const Transaction& tx)
    {
        if (!tx.data) return UniValue{};
        UniValue d{UniValue::VOBJ};
        d.pushKV("kind", kind_of_data(tx.data->kind));
        d.pushKV("detail", json::txdata(*tx.data));
        return d;
    }

    UniValue block_detail(const Hash32& hash, const Mine& mine)
    {
        auto hd{node.header(hash)};
        auto b{node.body(hash)};
        auto at{block_of(hash)};
        if (!hd || !b || !at) throw HostError("No such block");
        UniValue o{block_summary(at->first, hash, *hd, *b)};
        o.pushKV("prevHash", hd->prev_side_hash ? bt::hex_encode(*hd->prev_side_hash) : "");
        o.pushKV("merkleRoot", bt::hex_encode(hd->merkle_root));
        const bool on_chain{node.block_hash_at(at->first) == hash};
        o.pushKV("onChain", on_chain);
        UniValue cb{UniValue::VARR};
        for (const Output& c : b->coinbase.outputs) cb.push_back(output_json(c, mine));
        o.pushKV("coinbase", cb);
        UniValue txs{UniValue::VARR};
        for (const auto& tx : b->transactions) {
            if (txs.size() >= 1000) break;
            UniValue t{UniValue::VOBJ};
            t.pushKV("txid", bt::hex_encode(tx.txid()));
            t.pushKV("inputs", static_cast<uint64_t>(tx.inputs.size()));
            if (on_chain && txs.size() < 200) {
                t.pushKV("outputs", outputs_json(tx, mine));
            } else {
                UniValue outs{UniValue::VARR};
                for (const Output& out : tx.outputs) outs.push_back(output_json(out, mine));
                t.pushKV("outputs", outs);
            }
            if (tx.data) t.pushKV("data", data_json(tx));
            txs.push_back(t);
        }
        o.pushKV("transactions", txs);
        return o;
    }

    UniValue tx_detail(const Hash32& txid, const Mine& mine)
    {
        auto t{node.transaction(txid)};
        UniValue o{UniValue::VOBJ};
        o.pushKV("txid", bt::hex_encode(txid));
        if (!t) throw HostError("No such transaction");
        auto f{node.filled_transaction(txid)};
        const bool pending{f && !f->second};
        o.pushKV("pending", pending);
        if (!pending) {
            if (auto blk{block_of_tx(txid)}) {
                o.pushKV("block", bt::hex_encode(*blk));
                if (auto b{block_of(*blk)}) {
                    o.pushKV("height", static_cast<uint64_t>(b->first));
                    if (auto tm{main_time(b->second)}) o.pushKV("time", *tm);
                    if (auto h{node.height()}) o.pushKV("confirmations", static_cast<uint64_t>(*h >= b->first ? *h - b->first + 1 : 0));
                }
            }
        }
        UniValue ins{UniValue::VARR};
        if (f) {
            for (const FilledOutput& s : f->first.second) ins.push_back(output_json(s, mine));
            FilledTransaction ft{&*t, f->first.second};
            if (auto fee{ft.bitcoin_fee()}) o.pushKV("fee", n64(*fee));
        } else {
            for (const OutPoint& op : t->inputs) {
                UniValue i{UniValue::VOBJ};
                i.pushKV("outpoint", json::outpoint(op));
                ins.push_back(i);
            }
        }
        if (!t->memo.empty()) o.pushKV("memo", bt::hex_encode(t->memo));
        if (t->data) o.pushKV("data", data_json(*t));
        o.pushKV("inputs", ins);
        o.pushKV("outputs", outputs_json(*t, mine));
        return o;
    }

    UniValue address_detail(const Address& a, const Mine& mine)
    {
        const std::set<Address> one{a};
        uint64_t balance{0};
        UniValue utxos{UniValue::VARR};
        for (auto& [op, o] : node.utxos_by_addresses(one)) {
            if (is_value(o)) balance = balance + o.bitcoin_value() < balance ? UINT64_MAX : balance + o.bitcoin_value();
            if (utxos.size() < 500) {
                UniValue u{output_json(o, mine)};
                u.pushKV("outpoint", json::outpoint(op));
                utxos.push_back(u);
            }
        }
        UniValue stxos{UniValue::VARR};
        for (auto& [op, s] : node.stxos_by_addresses(one)) {
            if (stxos.size() >= 500) break;
            UniValue u{output_json(s.output, mine)};
            u.pushKV("outpoint", json::outpoint(op));
            u.pushKV("spentBy", s.inpoint.kind == InPoint::Withdrawal ? "bundle:" + bt::hex_encode_rev(s.inpoint.id.span()) : bt::hex_encode(s.inpoint.id));
            stxos.push_back(u);
        }
        UniValue o{UniValue::VOBJ};
        o.pushKV("address", a.to_base58());
        if (mine) o.pushKV("mine", mine->contains(a));
        o.pushKV("balance", n64(balance));
        o.pushKV("utxos", utxos);
        o.pushKV("spent", stxos);
        o.pushKV("depositAddress", a.to_base58()); // the bare address: what every LayerTwo chain credits (doc/app.md)
        return o;
    }

    // ---------------------------------------------------------- peg
    UniValue peg()
    {
        const auto mine{addresses()};
        UniValue o{UniValue::VOBJ};
        auto pending{node.pending_withdrawal_bundle()};
        std::set<OutPoint> in_pending;
        if (pending) {
            UniValue p{UniValue::VOBJ};
            p.pushKV("m6id", bt::hex_encode_rev(pending->m6id().span()));
            uint64_t total{0};
            for (auto& [op, out] : pending->spend_utxos) {
                total = total + out.bitcoin_value() < total ? UINT64_MAX : total + out.bitcoin_value();
                in_pending.insert(op);
            }
            p.pushKV("withdrawals", static_cast<uint64_t>(pending->spend_utxos.size()));
            p.pushKV("total", n64(total));
            o.pushKV("pendingBundle", p);
        } else {
            o.pushKV("pendingBundle", UniValue{});
        }
        auto failed{node.latest_failed_withdrawal_bundle_height()};
        o.pushKV("lastFailedHeight", failed ? UniValue(uint64_t(*failed)) : UniValue{});
        UniValue w{UniValue::VARR};
        auto withdrawal = [&](const OutPoint& op, const FilledOutput& out, const std::string& state, const std::optional<Hash32>& m6id) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("outpoint", json::outpoint(op));
            r.pushKV("txid", bt::hex_encode(op.txid));
            r.pushKV("mainAddress", out.content.main_address.to_string());
            r.pushKV("amount", n64(out.content.value));
            r.pushKV("mainFee", n64(out.content.main_fee));
            r.pushKV("state", state);
            if (m6id) {
                r.pushKV("m6id", bt::hex_encode_rev(m6id->span()));
                if (auto b{bundle_status(*m6id)}) r.pushKV("bundleStatus", to_string(b->second));
            }
            w.push_back(r);
        };
        for (auto& [op, out] : node.utxos_by_addresses(mine)) {
            if (!out.content.is_withdrawal()) continue;
            withdrawal(op, out, in_pending.contains(op) ? "in-pending-bundle" : "waiting", pending && in_pending.contains(op) ? std::optional<Hash32>{pending->m6id()} : std::nullopt);
        }
        for (auto& [op, s] : node.stxos_by_addresses(mine)) {
            if (!s.output.content.is_withdrawal() || s.inpoint.kind != InPoint::Withdrawal) continue;
            auto b{bundle_status(s.inpoint.id)};
            std::string state{"submitted"};
            if (b && b->second == BundleStatus::Confirmed) state = "paid";
            else if (b && b->second == BundleStatus::Failed) state = "failed";
            withdrawal(op, s.output, state, s.inpoint.id);
        }
        o.pushKV("withdrawals", w);
        o.pushKV("wealth", n64(node.sidechain_wealth()));
        o.pushKV("mainchain", link ? link->info() : UniValue{});
        o.pushKV("slot", static_cast<int>(THIS_SIDECHAIN));
        return o;
    }

    // ---------------------------------------------------------- assets, pools, auctions
    std::optional<BitAssetRecord> asset_record(const Hash32& id)
    {
        std::lock_guard<std::recursive_mutex> l(node.lock());
        bt::kv::Txn t(node.store());
        return node.state().bitasset(t, id);
    }

    std::optional<uint32_t> asset_seq(const Hash32& id)
    {
        std::lock_guard<std::recursive_mutex> l(node.lock());
        bt::kv::Txn t(node.store());
        return node.state().bitasset_seq(t, id);
    }

    std::vector<std::pair<std::pair<AssetId, AssetId>, PoolState>> pools()
    {
        std::lock_guard<std::recursive_mutex> l(node.lock());
        bt::kv::Txn t(node.store());
        return node.state().pools(t);
    }

    /** What the wallet holds, coin by coin (without coins a mempool transaction spends): of each BitAsset, its controls,
     *  its LP tokens by pool, its auction receipts, its reservations. */
    struct Holdings {
        uint64_t ecx{0};
        std::map<Hash32, std::pair<uint64_t, size_t>> assets; // amount, coins
        std::set<Hash32> controls;
        std::map<std::pair<AssetId, AssetId>, uint64_t> lp;
        std::vector<Hash32> receipts;
        std::vector<std::pair<OutPoint, FilledOutput>> reservations;
    };

    Holdings holdings()
    {
        Holdings h;
        if (!wallet.has_seed()) return h;
        for (auto& [op, o] : wallet.utxos()) {
            const auto& c{o.content};
            if (c.is_bitcoin()) h.ecx = h.ecx + o.bitcoin_value() < h.ecx ? UINT64_MAX : h.ecx + o.bitcoin_value();
            else if (c.is_bitasset()) {
                auto& e{h.assets[c.id]};
                e.first = e.first + c.value < e.first ? UINT64_MAX : e.first + c.value;
                ++e.second;
            } else if (c.is_control()) h.controls.insert(c.id);
            else if (c.is_lp_token()) {
                auto& v{h.lp[{c.asset0, c.asset1}]};
                v = v + c.value < v ? UINT64_MAX : v + c.value;
            } else if (c.is_receipt()) h.receipts.push_back(c.id);
            else if (c.is_reservation()) h.reservations.emplace_back(op, o);
        }
        return h;
    }

    UniValue pool_json(const AssetId& a0, const AssetId& a1, const PoolState& p, bool reader)
    {
        UniValue o{UniValue::VOBJ};
        o.pushKV("asset0", asset_json(a0, reader));
        o.pushKV("asset1", asset_json(a1, reader));
        o.pushKV("reserve0", std::to_string(p.reserve0));
        o.pushKV("reserve1", std::to_string(p.reserve1));
        o.pushKV("lpTokens", std::to_string(p.outstanding_lp_tokens));
        o.pushKV("creationTxid", bt::hex_encode(p.creation_txid));
        // Both sides BitAssets: what our app trades (an ECX side can be read, not traded through it).
        o.pushKV("tradable", a0.kind == AssetId::BitAsset && a1.kind == AssetId::BitAsset);
        return o;
    }

    UniValue auction_json(const Hash32& id, const AuctionState& a, bool reader, const std::set<Hash32>& receipts)
    {
        const uint32_t next{next_height()};
        UniValue o{UniValue::VOBJ};
        o.pushKV("id", bt::hex_encode(id));
        o.pushKV("base", asset_json(a.base_asset, reader));
        o.pushKV("quote", asset_json(a.quote_asset, reader));
        o.pushKV("startBlock", static_cast<uint64_t>(a.start_block));
        o.pushKV("duration", static_cast<uint64_t>(a.duration));
        const uint64_t end{uint64_t(a.start_block) + a.duration};
        o.pushKV("endBlock", n64(end));
        o.pushKV("initialBase", std::to_string(a.initial_base_amount));
        o.pushKV("baseRemaining", std::to_string(a.base_amount_remaining.latest().data));
        o.pushKV("quoteRaised", std::to_string(a.quote_amount.latest().data));
        o.pushKV("initialPrice", std::to_string(a.initial_price));
        o.pushKV("finalPrice", std::to_string(a.initial_end_price));
        o.pushKV("bids", static_cast<uint64_t>(a.base_amount_remaining.v.size() > 0 ? a.base_amount_remaining.v.size() - 1 : 0));
        auto price{auction_price_at(a, next)};
        o.pushKV("priceNext", price ? UniValue(std::to_string(*price)) : UniValue{});
        std::string state;
        const bool collected{a.base_amount_remaining.latest().data == 0 && a.quote_amount.latest().data == 0 && a.base_amount_remaining.v.size() > 1 &&
                             a.base_amount_remaining.v.back().height >= end};
        // upcoming; open (a bid can be made in the next block); closing (no more bids, its collect from the block after
        // its last: the chain's rule); ended (to collect); collected.
        if (next < a.start_block) state = "upcoming";
        else if (uint64_t(next) <= end && a.base_amount_remaining.latest().data > 0 && price) state = "open";
        else if (collected) state = "collected";
        else if (uint64_t(next) <= end) state = "closing";
        else state = "ended";
        o.pushKV("state", state);
        o.pushKV("nextHeight", static_cast<uint64_t>(next));
        o.pushKV("tradable", a.base_asset.kind == AssetId::BitAsset && a.quote_asset.kind == AssetId::BitAsset);
        if (reader) o.pushKV("mine", receipts.contains(id));
        return o;
    }

    // ---------------------------------------------------------- questions of the spends
    Address side_address(const UniValue& v, const std::string& what)
    {
        if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": a BitAssets address");
        auto a{Address::from_base58(v.get_str())};
        if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": not a BitAssets address (" + v.get_str() + ")");
        return *a;
    }

    bt::btc::Address main_address(const UniValue& v)
    {
        if (!v.isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "mainchain_address: an eCash address");
        auto a{bt::btc::Address::parse(v.get_str())};
        if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Not an eCash address: " + v.get_str());
        if (!main_address_of_network(*a, network)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "This eCash address is of another network than the one this node follows (" + network + ")");
        return *a;
    }

    uint64_t available() { return balances(addresses()).available; }

    void enough(uint64_t need)
    {
        const uint64_t have{available()};
        if (need > have) throw HostError(bt::appui::RPC_WALLET_ERROR, "Not enough available: " + coins(have) + " for " + coins(need));
    }

    static UniValue same_numbers(const UniValue& p, size_t n, const std::vector<size_t>& numbers)
    {
        if (!p.isArray() || p.size() < n) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: " + std::to_string(n) + " parameters");
        UniValue out{UniValue::VARR};
        for (size_t i{0}; i < p.size() && i < n; ++i) {
            if (std::find(numbers.begin(), numbers.end(), i) != numbers.end()) out.push_back(n64(u64_of(p[i], "param " + std::to_string(i))));
            else out.push_back(p[i]);
        }
        return out;
    }

    /** An optional memo (hex) at p[i], kept as it is; null or absent: none. */
    static void memo_param(const UniValue& p, size_t i, UniValue& out)
    {
        if (p.size() > i && !p[i].isNull()) {
            if (!p[i].isStr() || !bt::hex_decode(p[i].get_str())) throw HostError(bt::appui::HOST_INVALID_REQUEST, "memo: hex");
            if (p[i].get_str().size() > 2048) throw HostError(bt::appui::HOST_INVALID_REQUEST, "memo: at most 1024 bytes");
            out.push_back(p[i]);
        }
    }

    /** A transaction the wallet would build, without taking addresses (Wallet::PreviewScope): whether its outputs fill and
     *  its fee as the chain counts it. Refuses (before any question) what the wallet cannot pay or the chain would refuse. */
    struct Preview {
        Transaction tx;
        uint64_t fee{0};
    };
    Preview preview(const std::function<Transaction()>& build)
    {
        Preview p;
        try {
            Wallet::PreviewScope scope;
            p.tx = build();
        } catch (const WalletError& e) {
            const std::string why{e.what()};
            if (why == "not enough funds") throw HostError(bt::appui::RPC_WALLET_ERROR, "Not enough in this wallet for this");
            throw HostError(bt::appui::RPC_WALLET_ERROR, why);
        } catch (const StateError& e) {
            throw HostError(bt::appui::HOST_INVALID_REQUEST, e.what());
        }
        auto mine{wallet.utxos()};
        std::vector<FilledOutput> spent;
        for (const OutPoint& op : p.tx.inputs) {
            auto it{std::find_if(mine.begin(), mine.end(), [&](auto& e) { return e.first == op; })};
            if (it == mine.end()) throw HostError(bt::appui::RPC_WALLET_ERROR, "A coin of this transaction is not the wallet's any more: try again");
            spent.push_back(it->second);
        }
        FilledTransaction ft{&p.tx, spent};
        auto fee{ft.bitcoin_fee()};
        if (!ft.filled_outputs() || !fee) throw HostError(bt::appui::RPC_WALLET_ERROR, "BitAssets would refuse this transaction under its current rules (its outputs cannot be paid out)");
        p.fee = *fee;
        // The chain's own checks of it in the next block, on a store transaction never committed (as the node's templates
        // check theirs): its counts of assets, the pool's or the auction's arithmetic, the auction's blocks.
        const uint32_t height{next_height()};
        std::lock_guard<std::recursive_mutex> l(node.lock());
        bt::kv::Txn t(node.store());
        try {
            node.state().validate_filled(t, node.state().fill(t, p.tx));
            node.state().template_check(t, AuthorizedTransaction{p.tx, {}}, height);
        } catch (const StateError& e) {
            throw HostError(bt::appui::RPC_WALLET_ERROR, std::string{"BitAssets would refuse this transaction in the next block: "} + e.what());
        }
        return p;
    }

    void require_bitasset(const AssetId& a, const std::string& what)
    {
        if (a.kind != AssetId::BitAsset) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": pools and auctions in this app are of BitAssets (not ECX, not a control coin)");
        if (!asset_record(a.id)) throw HostError(bt::appui::HOST_INVALID_REQUEST, what + ": no such BitAsset");
    }

    std::map<std::string, Confirmed> confirmed()
    {
        std::map<std::string, Confirmed> c;
        // ---- ECX
        c["transfer"] = Confirmed{
            [](const UniValue& p) {
                // [dest, value, fee] and an optional memo (hex) kept as it is.
                UniValue out{same_numbers(p, 3, {1, 2})};
                memo_param(p, 3, out);
                return out;
            },
            [this](const UniValue& p) {
                const Address to{side_address(p[0], "dest")};
                const uint64_t value{p[1].getInt<uint64_t>()}, fee{p[2].getInt<uint64_t>()};
                if (value == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to send");
                const uint64_t total{bt::appui::spend_total({value, fee})};
                enough(total);
                Question q;
                q.title = "Send " + coins(value);
                q.message = addresses().contains(to) ? "To an address of this wallet." : "To this address on BitAssets. A payment cannot be taken back.";
                q.accept = "Send";
                q.kind = "send";
                q.rows = {{"To", to.to_base58(), "address"}, {"Amount", coins(value), "amount"}, {"Fee", coins(fee), "amount"}, {"Total", coins(total), "total"}};
                if (p.size() > 3) q.rows.push_back({"Memo (hex, visible on the chain)", p[3].get_str(), "text"});
                return q;
            },
            [this](const UniValue& p) { return l2l_call("transfer", p); }};
        c["transfer_many"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 2 || !p[0].isObject()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: {address: satoshis}, fee");
                UniValue dests{UniValue::VOBJ};
                for (size_t i{0}; i < p[0].size(); ++i) dests.pushKV(p[0].getKeys()[i], n64(u64_of(p[0].getValues()[i], "amount")));
                UniValue out{UniValue::VARR};
                out.push_back(dests);
                out.push_back(n64(u64_of(p[1], "fee_sats")));
                return out;
            },
            [this](const UniValue& p) {
                const UniValue& dests{p[0]};
                if (dests.size() == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No recipient");
                if (dests.size() > 2000) throw HostError(bt::appui::HOST_INVALID_REQUEST, "At most 2000 recipients in one payment");
                const uint64_t fee{p[1].getInt<uint64_t>()};
                uint64_t total{bt::appui::spend_total({fee})};
                Question q;
                q.kind = "send-many";
                q.accept = "Send";
                std::set<Address> seen;
                for (size_t i{0}; i < dests.size(); ++i) {
                    const Address a{side_address(UniValue{dests.getKeys()[i]}, "dest")};
                    if (!seen.insert(a).second) throw HostError(bt::appui::HOST_INVALID_REQUEST, "invalid entry: found duplicate key");
                    const uint64_t v{dests.getValues()[i].getInt<uint64_t>()};
                    total = bt::appui::spend_total({total, v});
                    q.rows.push_back({"To", a.to_base58(), "address"});
                    q.rows.push_back({"Amount", coins(v), "amount"});
                }
                enough(total);
                q.title = "Send " + coins(total - fee) + " to " + std::to_string(dests.size()) + (dests.size() == 1 ? " recipient" : " recipients");
                q.message = "One transaction on BitAssets. A payment cannot be taken back.";
                q.rows.push_back({"Fee", coins(fee), "amount"});
                q.rows.push_back({"Total", coins(total), "total"});
                return q;
            },
            [this](const UniValue& p) { return l2l_call("transfer_many", p); }};
        c["withdraw"] = Confirmed{
            [](const UniValue& p) { return same_numbers(p, 4, {1, 2, 3}); },
            [this](const UniValue& p) {
                const bt::btc::Address to{main_address(p[0])};
                const uint64_t value{p[1].getInt<uint64_t>()}, fee{p[2].getInt<uint64_t>()}, main_fee{p[3].getInt<uint64_t>()};
                if (value == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to withdraw");
                const uint64_t total{bt::appui::spend_total({value, fee, main_fee})};
                enough(total);
                Question q;
                q.title = "Withdraw " + coins(value) + " to eCash";
                q.message = "Your coins leave BitAssets now. eCash pays them to the address below once a withdrawal bundle with them is "
                            "accepted by eCash's miners, which takes a long time (their votes). Until then they show as being withdrawn.";
                q.accept = "Withdraw";
                q.kind = "withdraw";
                q.rows = {{"To (on eCash)", to.to_string(), "address"}, {"Amount", coins(value), "amount"}, {"Fee on eCash", coins(main_fee), "amount"},
                          {"Fee on BitAssets", coins(fee), "amount"}, {"Total", coins(total), "total"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("withdraw", p); }};
        c["create_deposit"] = Confirmed{
            [](const UniValue& p) { return same_numbers(p, 3, {1, 2}); },
            [this](const UniValue& p) {
                const Address to{side_address(p[0], "address")};
                const uint64_t value{p[1].getInt<uint64_t>()}, fee{p[2].getInt<uint64_t>()};
                if (value == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to deposit");
                const uint64_t total{bt::appui::spend_total({value, fee})};
                const bool ours{addresses().contains(to)};
                Question q;
                q.title = "Deposit " + coins(value) + " into BitAssets";
                q.message = "Paid on eCash by the wallet of the mainchain interface this node uses (" + node.enforcer().url() +
                            "), into BitAssets' slot " + std::to_string(int(THIS_SIDECHAIN)) + ". It is credited on BitAssets in the first block after eCash mines it.";
                if (!ours) {
                    q.message += " The address is NOT one of this wallet: the coins go to whoever holds it.";
                    q.level = "danger";
                }
                q.accept = "Deposit";
                q.kind = "deposit";
                q.rows = {{"Into (BitAssets)", to.to_base58(), "address"}, {"Amount", coins(value), "amount"}, {"Fee on eCash", coins(fee), "amount"},
                          {"Total (paid on eCash)", coins(total), "total"}};
                return q;
            },
            [this](const UniValue& p) {
                UniValue r{l2l_call("create_deposit", p)};
                if (poller) poller->poke();
                return r;
            }};
        c["mine"] = Confirmed{
            [](const UniValue& p) {
                UniValue out{UniValue::VARR};
                if (p.isArray() && p.size() > 0 && !p[0].isNull()) out.push_back(n64(u64_of(p[0], "fee")));
                return out;
            },
            [this](const UniValue& p) {
                const uint64_t bid{bt::appui::spend_total({p.size() > 0 ? p[0].getInt<uint64_t>() : App::EMPTY_BLOCK_BMM_BRIBE})};
                Question q;
                q.title = "Merge-mine a block of BitAssets";
                q.message = "Asks eCash's miners to include the next block of BitAssets (blind merged mining), with a bid paid on eCash by the "
                            "wallet of the mainchain interface (" + node.enforcer().url() + "). The bid is paid whether or not the block is taken.";
                q.accept = "Bid";
                q.kind = "mine";
                q.rows = {{"Bid (at most)", p.size() > 0 ? coins(bid) : "the block's fees, or " + coins(App::EMPTY_BLOCK_BMM_BRIBE), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("mine", p); }};

        // ---- BitAssets: reserve, then register (= issue)
        c["reserve_bitasset"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 1 || !p[0].isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [plain_name]");
                UniValue out{UniValue::VARR};
                out.push_back(p[0]);
                return out;
            },
            [this](const UniValue& p) {
                const std::string name{p[0].get_str()};
                if (const std::string why{asset_name_problem(name)}; !why.empty()) throw HostError(bt::appui::HOST_INVALID_REQUEST, why);
                if (asset_record(name_hash(name))) throw HostError(bt::appui::HOST_INVALID_REQUEST, "This asset name is registered already");
                // Their wallet reserves without an input when it has no coin, a reservation anyone can replay: ours asks
                // for a coin first (spent back to the wallet, no fee).
                if (!wallet.has_spendable_coin()) throw HostError(bt::appui::RPC_WALLET_ERROR, "A reservation spends one of your ECX coins back to you: get some ECX on BitAssets first");
                Question q;
                q.title = "Reserve the asset name " + name;
                q.message = "A reservation hides the name until you register the asset: only its commitment goes on the chain now. Register it "
                            "(the asset, its supply and its records) once the reservation is in a block.";
                q.accept = "Reserve";
                q.kind = "asset.reserve";
                q.rows = {{"Name", name, "text"}, {"Its id (hash)", bt::hex_encode(name_hash(name)), "text"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("reserve_bitasset", p); }};
        c["register_bitasset"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 2 || !p[0].isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [plain_name, initial_supply, bitasset_data?]");
                UniValue out{UniValue::VARR};
                out.push_back(p[0]);
                out.push_back(n64(u64_of(p[1], "initial_supply")));
                if (p.size() > 2 && !p[2].isNull()) {
                    if (!p[2].isObject()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "bitasset_data: an object");
                    out.push_back(p[2]);
                }
                return out;
            },
            [this](const UniValue& p) {
                const std::string name{p[0].get_str()};
                if (const std::string why{asset_name_problem(name)}; !why.empty()) throw HostError(bt::appui::HOST_INVALID_REQUEST, why);
                const Hash32 id{name_hash(name)};
                if (asset_record(id)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "This asset name is registered already");
                const uint64_t supply{p[1].getInt<uint64_t>()};
                // The wallet's reservation of this name, in a block (its registration spends it).
                bool reserved{false};
                for (auto& [op, o] : holdings().reservations) {
                    auto plain{wallet.reserved_name(o.content.commitment)};
                    if (plain && *plain == name) reserved = true;
                }
                if (!reserved) throw HostError(bt::appui::RPC_WALLET_ERROR, "Reserve this name first, and register it once the reservation is in a block");
                Question q;
                q.title = "Register the asset " + name;
                q.message = "Reveals the name and creates the asset: its whole supply comes to this wallet, with its control coin (what lets "
                            "its holder change its records). Anyone can see it.";
                q.accept = "Register";
                q.kind = "asset.register";
                q.rows = {{"Name", name, "text"}, {"Asset id", bt::hex_encode(id), "text"}, {"Supply", units(supply) + " units", "amount"}};
                if (p.size() > 2) {
                    BitAssetData d;
                    try {
                        d = json::parse_bitasset_data(p[2]);
                    } catch (const std::exception& e) {
                        throw HostError(bt::appui::HOST_INVALID_REQUEST, std::string{"bitasset_data: "} + e.what());
                    }
                    // Each record as the node parsed it (what it will write), under its words.
                    static const std::map<std::string, std::string> labels{{"commitment", "Commitment"}, {"socket_addr_v4", "IPv4 address"}, {"socket_addr_v6", "IPv6 address"},
                                                                           {"encryption_pubkey", "Encryption key"}, {"signing_pubkey", "Signing key"}};
                    const UniValue j{json::bitasset_data(d)};
                    for (size_t i{0}; i < j.size(); ++i) {
                        const UniValue& v{j.getValues()[i]};
                        if (v.isNull()) continue;
                        std::string text{v.isStr() ? v.get_str() : v.write()};
                        if (j.getKeys()[i] == "commitment" && d.commitment) text = bt::hex_encode(*d.commitment);
                        const auto l{labels.find(j.getKeys()[i])};
                        q.rows.push_back({l != labels.end() ? l->second : j.getKeys()[i], text, "text"});
                    }
                }
                return q;
            },
            [this](const UniValue& p) {
                UniValue r{l2l_call("register_bitasset", p)};
                return r;
            }};
        c["transfer_bitasset"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 4) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [dest, asset_id, amount, fee_sats, memo?]");
                UniValue out{UniValue::VARR};
                out.push_back(p[0]);
                out.push_back(p[1]);
                out.push_back(n64(u64_of(p[2], "amount")));
                out.push_back(n64(u64_of(p[3], "fee_sats")));
                memo_param(p, 4, out);
                return out;
            },
            [this](const UniValue& p) {
                const Address to{side_address(p[0], "dest")};
                const Hash32 id{hash_of(p[1], "asset_id")};
                const uint64_t amount{p[2].getInt<uint64_t>()}, fee{p[3].getInt<uint64_t>()};
                if (amount == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to send");
                if (!asset_record(id)) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No such BitAsset");
                bt::appui::spend_total({fee});
                const Holdings h{holdings()};
                const auto held{h.assets.find(id)};
                if (held == h.assets.end() || held->second.first < amount)
                    throw HostError(bt::appui::RPC_WALLET_ERROR, "Not enough " + label(AssetId::bitasset(id)) + ": " + units(held == h.assets.end() ? 0 : held->second.first) + " for " + units(amount));
                const Bytes memo{p.size() > 4 ? *bt::hex_decode(p[4].get_str()) : Bytes{}};
                const Preview pv{preview([&] { return wallet.create_bitasset_transfer(to, id, amount, fee, memo); })};
                Question q;
                q.title = "Send " + units(amount) + " " + label(AssetId::bitasset(id));
                q.message = addresses().contains(to) ? "To an address of this wallet." : "To this address on BitAssets. A transfer cannot be taken back.";
                q.accept = "Send";
                q.kind = "asset.send";
                q.rows = {{"To", to.to_base58(), "address"},
                          {"Asset", label(AssetId::bitasset(id)), "text"},
                          {"Asset id", bt::hex_encode(id), "text"},
                          {"Amount", units(amount), "amount"},
                          {"Fee", coins(pv.fee), "amount"}};
                if (p.size() > 4) q.rows.push_back({"Memo (hex, visible on the chain)", p[4].get_str(), "text"});
                return q;
            },
            [this](const UniValue& p) { return l2l_call("transfer_bitasset", p); }};

        // ---- AMM pools (of two BitAssets)
        c["amm_mint"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 4) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [asset0, asset1, amount0, amount1]");
                UniValue out{UniValue::VARR};
                out.push_back(p[0]);
                out.push_back(p[1]);
                out.push_back(n64(u64_of(p[2], "amount0")));
                out.push_back(n64(u64_of(p[3], "amount1")));
                return out;
            },
            [this](const UniValue& p) {
                AssetId a0{asset_of(p[0], "asset0")}, a1{asset_of(p[1], "asset1")};
                uint64_t x0{p[2].getInt<uint64_t>()}, x1{p[3].getInt<uint64_t>()};
                if (a0 == a1) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A pool is of two different assets");
                require_bitasset(a0, "asset0");
                require_bitasset(a1, "asset1");
                if (x0 == 0 || x1 == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Put some of both assets into the pool");
                // As the LayerTwo handler: the amounts follow the assets into the pool's (sorted) order.
                if (a1 < a0) {
                    std::swap(a0, a1);
                    std::swap(x0, x1);
                }
                auto ps{app.pool(a0, a1)};
                const PoolState cur{ps ? *ps : PoolState::fresh(Hash32{})};
                std::string err;
                std::optional<PoolState> np;
                try {
                    np = cur.mint(x0, x1, err);
                } catch (const StateError&) {
                    throw HostError(bt::appui::HOST_INVALID_REQUEST, "BitAssets would refuse this deposit into the pool");
                }
                if (!np) throw HostError(bt::appui::HOST_INVALID_REQUEST, err);
                if (np->outstanding_lp_tokens < cur.outstanding_lp_tokens) throw HostError(bt::appui::HOST_INVALID_REQUEST, "AMM LP token overflow");
                const uint64_t lp{np->outstanding_lp_tokens - cur.outstanding_lp_tokens};
                if (lp == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "These amounts are too small for a share of the pool");
                const Preview pv{preview([&] { return wallet.amm_mint(a0, a1, x0, x1, lp); })};
                Question q;
                q.title = ps ? "Add liquidity to the pool " + label(a0) + " / " + label(a1) : "Open the pool " + label(a0) + " / " + label(a1);
                q.message = ps ? "Puts both assets into the pool for a share of it (LP tokens). Burning the tokens takes your share back, as the pool "
                                 "then holds it."
                               : "No pool of these two assets yet: this one opens it, at the price your two amounts make. Burning the LP tokens takes "
                                 "your share back.";
                q.accept = ps ? "Add" : "Open the pool";
                q.kind = "pool.add";
                q.rows = {{"Put in", units(x0) + " " + label(a0), "amount"},
                          {"And", units(x1) + " " + label(a1), "amount"},
                          {"LP tokens", units(lp), "amount"},
                          {"Pool after", units(np->reserve0) + " / " + units(np->reserve1), "text"},
                          {"Fee", coins(pv.fee), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("amm_mint", p); }};
        c["amm_burn"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 3) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [asset0, asset1, lp_token_amount]");
                UniValue out{UniValue::VARR};
                out.push_back(p[0]);
                out.push_back(p[1]);
                out.push_back(n64(u64_of(p[2], "lp_token_amount")));
                return out;
            },
            [this](const UniValue& p) {
                const AssetId a{asset_of(p[0], "asset0")}, b{asset_of(p[1], "asset1")};
                const uint64_t lp{p[2].getInt<uint64_t>()};
                if (a == b) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A pool is of two different assets");
                require_bitasset(a, "asset0");
                require_bitasset(b, "asset1");
                if (lp == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to take out");
                const AssetId a0{std::min(a, b)}, a1{std::max(a, b)};
                auto ps{app.pool(a0, a1)};
                if (!ps) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No pool of these two assets");
                std::string err;
                auto np{ps->burn(lp, err)};
                if (!np) throw HostError(bt::appui::HOST_INVALID_REQUEST, err);
                const uint64_t out0{ps->reserve0 - np->reserve0}, out1{ps->reserve1 - np->reserve1};
                const Preview pv{preview([&] { return wallet.amm_burn(a0, a1, out0, out1, lp); })};
                Question q;
                q.title = "Take liquidity out of the pool " + label(a0) + " / " + label(a1);
                q.message = "Burns these LP tokens for your share of what the pool holds now.";
                q.accept = "Take out";
                q.kind = "pool.remove";
                q.rows = {{"LP tokens burned", units(lp), "amount"}, {"You get", units(out0) + " " + label(a0), "amount"}, {"And", units(out1) + " " + label(a1), "amount"},
                          {"Fee", coins(pv.fee), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("amm_burn", p); }};
        c["amm_swap"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 3) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [asset_spend, asset_receive, amount_spend]");
                UniValue out{UniValue::VARR};
                out.push_back(p[0]);
                out.push_back(p[1]);
                out.push_back(n64(u64_of(p[2], "amount_spend")));
                return out;
            },
            [this](const UniValue& p) {
                const AssetId sp{asset_of(p[0], "asset_spend")}, rc{asset_of(p[1], "asset_receive")};
                const uint64_t amount{p[2].getInt<uint64_t>()};
                if (sp == rc) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Swap one asset for another");
                require_bitasset(sp, "asset_spend");
                require_bitasset(rc, "asset_receive");
                if (amount == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to swap");
                auto ps{app.pool(sp, rc)};
                if (!ps) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No pool of these two assets");
                std::string err;
                uint64_t receive;
                // As the LayerTwo handler: what the pool pays is old - new reserve.
                try {
                    if (sp < rc) {
                        auto np{ps->swap_asset0_for_asset1(amount, err)};
                        if (!np) throw HostError(bt::appui::HOST_INVALID_REQUEST, err);
                        receive = ps->reserve1 - np->reserve1;
                    } else {
                        auto np{ps->swap_asset1_for_asset0(amount, err)};
                        if (!np) throw HostError(bt::appui::HOST_INVALID_REQUEST, err);
                        receive = ps->reserve0 - np->reserve0;
                    }
                } catch (const StateError&) {
                    throw HostError(bt::appui::HOST_INVALID_REQUEST, "BitAssets would refuse this swap now (the pool cannot price it)");
                }
                if (receive == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "This amount is too small: the pool would pay nothing for it");
                const Preview pv{preview([&] { return wallet.amm_swap(sp, rc, amount, receive); })};
                Question q;
                q.title = "Swap " + units(amount) + " " + label(sp) + " for " + units(receive) + " " + label(rc);
                q.message = "At the pool's price now (0.3% of the swap goes to the pool, twice: on what you pay and on what you get). If the pool "
                            "changes before the next block, the swap is refused there and nothing is spent.";
                q.accept = "Swap";
                q.kind = "swap";
                q.rows = {{"You pay", units(amount) + " " + label(sp), "amount"}, {"You get", units(receive) + " " + label(rc), "amount"},
                          {"Pool", label(std::min(sp, rc)) + " / " + label(std::max(sp, rc)), "text"}, {"Fee", coins(pv.fee), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("amm_swap", p); }};

        // ---- Dutch auctions (of a BitAsset, priced in a BitAsset)
        c["dutch_auction_create"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 1 || !p[0].isObject()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [dutch_auction_params]");
                const UniValue& x{p[0]};
                UniValue o{UniValue::VOBJ};
                o.pushKV("start_block", n64(u32_of(x.find_value("start_block"), "start_block")));
                o.pushKV("duration", n64(u32_of(x.find_value("duration"), "duration")));
                if (!x.find_value("base_asset").isStr() || !x.find_value("quote_asset").isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "base_asset, quote_asset: asset ids");
                o.pushKV("base_asset", x.find_value("base_asset"));
                o.pushKV("base_amount", n64(u64_of(x.find_value("base_amount"), "base_amount")));
                o.pushKV("quote_asset", x.find_value("quote_asset"));
                o.pushKV("initial_price", n64(u64_of(x.find_value("initial_price"), "initial_price")));
                o.pushKV("final_price", n64(u64_of(x.find_value("final_price"), "final_price")));
                UniValue out{UniValue::VARR};
                out.push_back(o);
                return out;
            },
            [this](const UniValue& p) {
                const UniValue& x{p[0]};
                DutchAuctionParams d;
                d.start_block = static_cast<uint32_t>(x["start_block"].getInt<uint64_t>());
                d.duration = static_cast<uint32_t>(x["duration"].getInt<uint64_t>());
                d.base_asset = asset_of(x["base_asset"], "base_asset");
                d.base_amount = x["base_amount"].getInt<uint64_t>();
                d.quote_asset = asset_of(x["quote_asset"], "quote_asset");
                d.initial_price = x["initial_price"].getInt<uint64_t>();
                d.final_price = x["final_price"].getInt<uint64_t>();
                require_bitasset(d.base_asset, "base_asset");
                require_bitasset(d.quote_asset, "quote_asset");
                if (d.base_asset == d.quote_asset) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Sell an asset for another one");
                if (d.base_amount == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to sell");
                if (d.initial_price == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A starting price above zero");
                // The chain's own rules of a new auction (dutch_auction.rs), said before asking.
                const uint32_t next{next_height()};
                if (d.start_block <= next) throw HostError(bt::appui::HOST_INVALID_REQUEST, "The auction starts at block " + std::to_string(next + 1) + " at the earliest (the next block is " + std::to_string(next) + ")");
                if (d.duration == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "An auction lasts at least one block");
                if (d.final_price > d.initial_price) throw HostError(bt::appui::HOST_INVALID_REQUEST, "The final price cannot be above the starting price");
                if (d.duration == 1 && d.final_price != d.initial_price) throw HostError(bt::appui::HOST_INVALID_REQUEST, "An auction of one block has one price");
                if (uint64_t(d.start_block) + d.duration > UINT32_MAX) throw HostError(bt::appui::HOST_INVALID_REQUEST, "The auction ends past the last block height");
                const Preview pv{preview([&] { return wallet.dutch_auction_create(d); })};
                Question q;
                q.title = "Sell " + units(d.base_amount) + " " + label(d.base_asset) + " by Dutch auction";
                q.message = "The asset leaves your wallet now. From block " + std::to_string(d.start_block) + " for " + std::to_string(d.duration) +
                            " blocks its price falls block by block, from the starting price to the final one; anyone can buy part or all of it. "
                            "Once it ends, collect what was paid and what was not sold (this wallet keeps the auction's receipt).";
                q.accept = "Start the auction";
                q.kind = "auction.create";
                q.rows = {{"Sell", units(d.base_amount) + " " + label(d.base_asset), "amount"},
                          {"Priced in", label(d.quote_asset), "text"},
                          {"Starting price (all of it)", units(d.initial_price) + " " + label(d.quote_asset), "amount"},
                          {"Final price (all of it)", units(d.final_price) + " " + label(d.quote_asset), "amount"},
                          {"Blocks", std::to_string(d.start_block) + " to " + std::to_string(uint64_t(d.start_block) + d.duration - 1), "text"},
                          {"Fee", coins(pv.fee), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("dutch_auction_create", p); }};
        c["dutch_auction_bid"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 2 || !p[0].isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [dutch_auction_id, bid_size]");
                UniValue out{UniValue::VARR};
                out.push_back(p[0]);
                out.push_back(n64(u64_of(p[1], "bid_size")));
                return out;
            },
            [this](const UniValue& p) {
                const Hash32 id{hash_of(p[0], "dutch_auction_id")};
                const uint64_t bid{p[1].getInt<uint64_t>()};
                if (bid == 0) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Nothing to bid");
                auto st{app.auction(id)};
                if (!st) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No such auction");
                const uint32_t height{next_height()};
                std::string err;
                std::optional<AuctionState> ns;
                try {
                    ns = st->bid(Hash32{}, bid, height, err);
                } catch (const StateError& e) {
                    throw HostError(bt::appui::HOST_INVALID_REQUEST, "BitAssets would refuse this bid now");
                }
                if (!ns) throw HostError(bt::appui::HOST_INVALID_REQUEST, err);
                const uint64_t qty{st->base_amount_remaining.latest().data - ns->base_amount_remaining.latest().data};
                const Preview pv{preview([&] { return wallet.dutch_auction_bid(id, st->base_asset, st->quote_asset, bid, qty); })};
                const auto price{auction_price_at(*st, height)};
                Question q;
                q.title = "Bid " + units(bid) + " " + label(st->quote_asset) + " for " + units(qty) + " " + label(st->base_asset);
                q.message = "At the auction's price in the next block (block " + std::to_string(height) + "). If another bid comes first, the price "
                            "and what you get change: this bid is then refused by the chain and nothing is spent.";
                q.accept = "Bid";
                q.kind = "auction.bid";
                q.rows = {{"You pay", units(bid) + " " + label(st->quote_asset), "amount"},
                          {"You get", units(qty) + " " + label(st->base_asset), "amount"},
                          {"Price now (all that is left)", price ? units(*price) + " " + label(st->quote_asset) : "–", "text"},
                          {"Auction", bt::hex_encode(id), "text"},
                          {"Fee", coins(pv.fee), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("dutch_auction_bid", p); }};
        c["dutch_auction_collect"] = Confirmed{
            [](const UniValue& p) {
                if (!p.isArray() || p.size() < 1 || !p[0].isStr()) throw HostError(bt::appui::HOST_INVALID_REQUEST, "params: [dutch_auction_id]");
                UniValue out{UniValue::VARR};
                out.push_back(p[0]);
                return out;
            },
            [this](const UniValue& p) {
                const Hash32 id{hash_of(p[0], "dutch_auction_id")};
                auto st{app.auction(id)};
                if (!st) throw HostError(bt::appui::HOST_INVALID_REQUEST, "No such auction");
                const Holdings h{holdings()};
                if (std::find(h.receipts.begin(), h.receipts.end(), id) == h.receipts.end()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet does not hold this auction's receipt");
                const uint64_t height{next_height()};
                if (height <= uint64_t(st->start_block) + st->duration) throw HostError(bt::appui::HOST_INVALID_REQUEST, "The auction has not ended yet: collect from block " + std::to_string(uint64_t(st->start_block) + st->duration + 1));
                const uint64_t base{st->base_amount_remaining.latest().data}, quote{st->quote_amount.latest().data};
                const Preview pv{preview([&] { return wallet.dutch_auction_collect(id, st->base_asset, st->quote_asset, base, quote); })};
                Question q;
                q.title = "Collect the auction of " + label(st->base_asset);
                q.message = "What was paid for it, and what was not sold, come to your wallet; the auction's receipt is spent. The wallet also "
                            "spends one of its BitAsset coins and pays it back in full (the chain's rules need one in a collect).";
                q.accept = "Collect";
                q.kind = "auction.collect";
                q.rows = {{"Paid to you", units(quote) + " " + label(st->quote_asset), "amount"}, {"Not sold, back to you", units(base) + " " + label(st->base_asset), "amount"},
                          {"Auction", bt::hex_encode(id), "text"}, {"Fee", coins(pv.fee), "amount"}};
                return q;
            },
            [this](const UniValue& p) { return l2l_call("dutch_auction_collect", p); }};
        // Every spend's balance looked at again at once.
        for (auto& [name, conf] : c) {
            auto run{conf.run};
            conf.run = [this, run](const UniValue& p) {
                UniValue r{run(p)};
                if (poller) poller->poke();
                return r;
            };
        }
        return c;
    }
};

const std::vector<std::string>& AppLayer::l2l_reads()
{
    static const std::vector<std::string> reads{"bitasset_data", "bitassets", "bitcoin_balance", "dutch_auctions", "format_deposit_address", "get_amm_pool_state",
                                                "get_amm_price", "get_best_mainchain_block_hash", "get_best_sidechain_block_hash", "get_block", "get_block_hash",
                                                "get_block_index", "get_bmm_inclusions", "get_stxos", "get_transaction", "get_transaction_info", "get_utxos",
                                                "get_wallet_addresses", "get_wallet_utxos", "getblockcount", "latest_failed_withdrawal_bundle_height", "list_mempool",
                                                "list_peers", "list_utxos", "mainchain_sync_progress", "my_unconfirmed_utxos", "my_utxos", "openapi_schema",
                                                "pending_withdrawal_bundle", "sidechain_wealth"};
    return reads;
}

namespace {
const std::vector<std::string>& confirmed_methods()
{
    static const std::vector<std::string> m{"amm_burn", "amm_mint", "amm_swap", "create_deposit", "dutch_auction_bid", "dutch_auction_collect", "dutch_auction_create",
                                            "mine", "register_bitasset", "reserve_bitasset", "transfer", "transfer_bitasset", "transfer_many", "withdraw"};
    return m;
}

const std::vector<std::string>& host_methods()
{
    static const std::vector<std::string> m{"dropin.overview", "dropin.history", "dropin.newAddress", "dropin.addresses", "dropin.blocks", "dropin.block",
                                            "dropin.transaction", "dropin.address", "dropin.search", "dropin.checkAddress", "dropin.peg", "dropin.peers",
                                            "dropin.peers.connect", "dropin.peers.forget", "dropin.seed.status", "dropin.seed.prepare", "dropin.seed.create",
                                            "dropin.seed.restore", "dropin.seed.show", "dropin.seed.save", "bitassets.assets", "bitassets.asset",
                                            "bitassets.resolve", "bitassets.wallet", "bitassets.pools", "bitassets.quote", "bitassets.auctions",
                                            "bitassets.bidQuote"};
    return m;
}

const char* const SHELL_READS[] = {"getmainchaininfo", "getbalances", "getblockchaininfo", "getnetworkinfo", "uptime"};
} // namespace

AppLayer::AppLayer(App& app, std::string network, std::string datadir, bt::appui::AppOptions options)
    : m_(std::make_unique<Impl>(app, std::move(network), std::move(datadir), std::move(options)))
{
}

AppLayer::~AppLayer() { stop(); }

uint16_t AppLayer::port() const { return server_ ? server_->port() : 0; }

UniValue AppLayer::history(size_t limit) { return m_->history(limit); }

bool AppLayer::start(std::string& err)
{
    Impl& m{*m_};
    if (!bt::fsperm::private_dir(m.datadir)) {
        err = "cannot make " + m.datadir;
        return false;
    }
    m.store = std::make_unique<bt::appui::AppStore>(m.datadir + "/backbone-app.sqlite");
    std::string why;
    bt::appui::ServerOptions so{bt::appui::server_options(m.options, m.datadir, why)};
    if (!why.empty()) {
        err = why;
        return false;
    }
    server_ = std::make_unique<bt::appui::RpcServer>(std::move(so));
    m.link = std::make_unique<bt::appui::LinkMonitor>(m.node.enforcer(), THIS_SIDECHAIN, [&m] { return m.followed(); });

    // ---- the app RPC's own methods: the LayerTwo reads, as theirs; the reads of the kit's shell; the setup of the wallet.
    for (const std::string& name : l2l_reads()) {
        server_->add(name, {}, [&m, name](const bt::appui::Call& c) { return m.l2l_call(name, c.params); });
    }
    server_->add("getmainchaininfo", {}, [&m](const bt::appui::Call&) { return m.link->info(); });
    server_->add("getbalances", {}, [&m](const bt::appui::Call&) {
        UniValue mine{UniValue::VOBJ};
        Impl::Balances b;
        if (m.wallet.has_seed()) b = m.balances(m.addresses());
        mine.pushKV("trusted", bt::appui::coins_value(b.available));
        mine.pushKV("untrusted_pending", bt::appui::coins_value(b.pending_in));
        mine.pushKV("immature", bt::appui::coins_value(b.withdrawing));
        UniValue out{UniValue::VOBJ};
        out.pushKV("mine", mine);
        return out;
    });
    server_->add("getblockchaininfo", {}, [&m](const bt::appui::Call&) {
        UniValue o{UniValue::VOBJ};
        auto h{m.node.height()};
        auto t{m.node.tip()};
        o.pushKV("chain", m.network);
        // The sync status every page shows (bt::appui::sync_state): headers the best height known, in initial download
        // until up to date.
        bt::appui::NodeLook l;
        bt::appui::look_at_node(l, m.node, m.network);
        const bt::appui::SyncState sync{bt::appui::sync_state(l)};
        o.pushKV("blocks", h ? int64_t(*h) : int64_t{-1});
        o.pushKV("headers", sync.target ? int64_t(*sync.target) : h ? int64_t(*h) : int64_t{-1});
        o.pushKV("bestblockhash", t ? bt::hex_encode(*t) : "");
        o.pushKV("initialblockdownload", sync.kind != bt::appui::SyncState::Synced);
        o.pushKV("verificationprogress", sync.kind == bt::appui::SyncState::Synced ? 1.0 : sync.target && *sync.target ? double(sync.height) / *sync.target : 0.0);
        o.pushKV("warnings", UniValue{UniValue::VARR});
        return o;
    });
    server_->add("getnetworkinfo", {}, [&m](const bt::appui::Call&) {
        UniValue o{UniValue::VOBJ};
        size_t n{0};
        for (auto& p : m.node.peers()) n += p.connected ? 1 : 0;
        o.pushKV("version", 1701);
        o.pushKV("subversion", std::string{"/backbone-bitassets:"} + VERSION_TEXT + "/");
        o.pushKV("connections", static_cast<uint64_t>(n));
        o.pushKV("networkactive", true);
        o.pushKV("warnings", UniValue{UniValue::VARR});
        return o;
    });
    server_->add("uptime", {}, [&m](const bt::appui::Call&) {
        return UniValue(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now() - m.started).count()));
    });
    // The setup of the wallet by the unified wallet (doc/app.md "One backup phrase"): its own user (the cookie) sets
    // the phrase it derived; refused when the wallet has another one. Never offered to the pages.
    server_->add("getwalletphraseinfo", {}, [&m](const bt::appui::Call&) { return m.seed_status(); });
    server_->add("setwalletphrase", {"phrase"}, [&m](const bt::appui::Call& c) {
        if (!c[0].isStr()) throw RpcError(bt::appui::RPC_INVALID_PARAMETER, "phrase: the 12 to 24 words");
        std::string phrase{c[0].get_str()};
        const bt::appui::WipeOnExit wipe_phrase{phrase};
        try {
            UniValue r{m.set_phrase(phrase, "unified")};
            LOG_INFO("app layer: the wallet's phrase was set by the app (fingerprint %s)", r.find_value("fingerprint").get_str().c_str());
            return r;
        } catch (const HostError& e) {
            throw RpcError(e.code, e.what());
        }
    });
    server_->add("stop", {}, [&m](const bt::appui::Call&) {
        if (m.app.on_stop) m.app.on_stop();
        return UniValue("BitAssets stopping");
    });
    server_->add("getappmanifest", {}, [this](const bt::appui::Call&) { return manifest(); });

    // ---- the host of the pages
    bt::appui::ChainHost chain;
    chain.name = "BitAssets";
    chain.stem = "bitassets";
    chain.network = m.network;
    chain.info = [&m] { return m.info(); };
    chain.wallet_id = [&m] { return m.wallet_id(); };
    for (const char* r : SHELL_READS) chain.page_reads.insert(r);
    for (const std::string& r : l2l_reads()) chain.page_reads.insert(r);
    chain.confirmed = m.confirmed();
    auto& H{chain.host};
    auto& P{chain.privileges};
    H["dropin.overview"] = [&m](const UniValue&, HostEnv&) {
        UniValue o{UniValue::VOBJ};
        const bool has{m.wallet.has_seed()};
        Impl::Balances b;
        if (has) b = m.balances(m.addresses());
        UniValue w{UniValue::VOBJ};
        w.pushKV("hasSeed", has);
        w.pushKV("available", n64(b.available));
        w.pushKV("withdrawing", n64(b.withdrawing));
        w.pushKV("pendingIn", n64(b.pending_in));
        w.pushKV("pendingOut", n64(b.pending_out));
        o.pushKV("wallet", w);
        auto h{m.node.height()};
        auto t{m.node.tip()};
        UniValue c{UniValue::VOBJ};
        c.pushKV("height", h ? int64_t(*h) : int64_t{-1});
        c.pushKV("tip", t ? bt::hex_encode(*t) : "");
        size_t peers{0};
        for (auto& p : m.node.peers()) peers += p.connected ? 1 : 0;
        c.pushKV("peers", static_cast<uint64_t>(peers));
        c.pushKV("wealth", n64(m.node.sidechain_wealth()));
        c.pushKV("slot", static_cast<int>(THIS_SIDECHAIN));
        c.pushKV("network", m.network);
        o.pushKV("chain", c);
        o.pushKV("mainchain", m.link->info());
        return o;
    };
    P["dropin.overview"] = {"bitcoin_balance", "getblockcount"};
    H["dropin.history"] = [&m](const UniValue& p, HostEnv&) {
        const UniValue& lim{param(p, "limit")};
        const size_t limit{lim.isNum() ? static_cast<size_t>(std::clamp<int64_t>(lim.getInt<int64_t>(), 1, 5000)) : size_t{500}};
        if (!m.wallet.has_seed()) return UniValue{UniValue::VARR};
        return m.history(limit);
    };
    P["dropin.history"] = {"get_wallet_utxos"};
    H["dropin.newAddress"] = [&m](const UniValue&, HostEnv&) {
        if (!m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "The wallet has no backup phrase yet: set it up first");
        const Address a{m.wallet.get_new_address()};
        UniValue o{UniValue::VOBJ};
        o.pushKV("address", a.to_base58());
        o.pushKV("depositAddress", a.to_base58()); // the bare address: what every LayerTwo chain credits (doc/app.md)
        return o;
    };
    P["dropin.newAddress"] = {"get_new_address"};
    H["dropin.addresses"] = [&m](const UniValue&, HostEnv&) {
        UniValue out{UniValue::VARR};
        if (!m.wallet.has_seed()) return out;
        std::map<Address, std::pair<uint64_t, uint64_t>> got; // balance, received
        const auto mine{m.addresses()};
        auto add = [](uint64_t& x, uint64_t v) { x = x + v < x ? UINT64_MAX : x + v; };
        for (auto& [op, o] : m.node.utxos_by_addresses(mine)) {
            if (!is_value(o)) continue;
            add(got[o.address].first, o.bitcoin_value());
            add(got[o.address].second, o.bitcoin_value());
        }
        for (auto& [op, s] : m.node.stxos_by_addresses(mine)) {
            if (is_value(s.output)) add(got[s.output.address].second, s.output.bitcoin_value());
        }
        for (const Address& a : m.wallet.addresses()) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("address", a.to_base58());
            r.pushKV("depositAddress", a.to_base58());
            r.pushKV("balance", n64(got[a].first));
            r.pushKV("received", n64(got[a].second));
            out.push_back(r);
        }
        return out;
    };
    P["dropin.addresses"] = {"get_wallet_addresses"};
    H["dropin.blocks"] = [&m](const UniValue& p, HostEnv&) {
        const UniValue& cnt{param(p, "count")};
        const UniValue& from{param(p, "from")};
        const int64_t count{cnt.isNum() ? std::clamp<int64_t>(cnt.getInt<int64_t>(), 1, 200) : int64_t{20}};
        UniValue out{UniValue::VARR};
        auto h{m.node.height()};
        if (!h) return out;
        int64_t at{from.isNum() ? std::min<int64_t>(from.getInt<int64_t>(), *h) : int64_t(*h)};
        for (; at >= 0 && static_cast<int64_t>(out.size()) < count; --at) {
            auto hash{m.node.block_hash_at(static_cast<uint32_t>(at))};
            if (!hash) break;
            auto hd{m.node.header(*hash)};
            auto b{m.node.body(*hash)};
            if (!hd || !b) continue;
            out.push_back(m.block_summary(static_cast<uint32_t>(at), *hash, *hd, *b));
        }
        return out;
    };
    P["dropin.blocks"] = {"get_block"};
    H["dropin.block"] = [&m](const UniValue& p, HostEnv& env) {
        const UniValue& height{param(p, "height")};
        if (height.isNum()) {
            auto hash{m.node.block_hash_at(static_cast<uint32_t>(std::clamp<int64_t>(height.getInt<int64_t>(), 0, UINT32_MAX)))};
            if (!hash) throw HostError("No block at this height");
            return m.block_detail(*hash, m.mine_for(env));
        }
        auto hash{parse_hash(str_param(p, "hash"))};
        if (!hash) throw HostError(bt::appui::HOST_INVALID_REQUEST, "hash: 64 hex digits");
        return m.block_detail(*hash, m.mine_for(env));
    };
    P["dropin.block"] = {"get_block"};
    H["dropin.transaction"] = [&m](const UniValue& p, HostEnv& env) {
        auto txid{parse_hash(str_param(p, "txid"))};
        if (!txid) throw HostError(bt::appui::HOST_INVALID_REQUEST, "txid: 64 hex digits");
        return m.tx_detail(*txid, m.mine_for(env));
    };
    P["dropin.transaction"] = {"get_transaction"};
    H["dropin.address"] = [&m](const UniValue& p, HostEnv& env) {
        auto a{Address::from_base58(str_param(p, "address"))};
        if (!a) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Not a BitAssets address");
        return m.address_detail(*a, m.mine_for(env));
    };
    P["dropin.address"] = {"get_utxos", "get_stxos"};
    H["dropin.search"] = [&m](const UniValue& p, HostEnv&) {
        std::string q{str_param(p, "query")};
        q.erase(0, q.find_first_not_of(" \t"));
        q.erase(q.find_last_not_of(" \t") + 1);
        UniValue o{UniValue::VOBJ};
        if (q.size() > 120) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Too long");
        if (!q.empty() && q.size() <= 10 && q.find_first_not_of("0123456789") == std::string::npos) {
            auto hash{m.node.block_hash_at(static_cast<uint32_t>(std::min<uint64_t>(std::stoull(q), UINT32_MAX)))};
            if (hash) {
                o.pushKV("kind", "block");
                o.pushKV("hash", bt::hex_encode(*hash));
                return o;
            }
        } else if (auto h{parse_hash(q)}) {
            if (m.node.header(*h)) {
                o.pushKV("kind", "block");
                o.pushKV("hash", q);
                return o;
            }
            if (m.asset_record(*h)) {
                o.pushKV("kind", "asset");
                o.pushKV("id", q);
                return o;
            }
            o.pushKV("kind", "transaction");
            o.pushKV("txid", q);
            return o;
        } else if (Address::from_base58(q)) {
            o.pushKV("kind", "address");
            o.pushKV("address", q);
            return o;
        } else if (auto inner{Address::from_deposit_address(q, THIS_SIDECHAIN)}) {
            // A deposit address: the BitAssets address inside.
            o.pushKV("kind", "address");
            o.pushKV("address", inner->to_base58());
            return o;
        }
        o.pushKV("kind", "none");
        return o;
    };
    P["dropin.search"] = {"get_block", "get_transaction"};
    H["dropin.checkAddress"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string text{str_param(p, "address")};
        const std::string side{param(p, "side").isStr() ? param(p, "side").get_str() : "bitassets"};
        UniValue o{UniValue::VOBJ};
        if (side == "mainchain") {
            auto a{bt::btc::Address::parse(text)};
            o.pushKV("valid", a.has_value() && main_address_of_network(*a, m.network));
            if (a && !main_address_of_network(*a, m.network)) o.pushKV("error", "an eCash address of another network");
            else if (!a) o.pushKV("error", Address::from_base58(text) ? "a BitAssets address, not an eCash one" : "not an eCash address");
            return o;
        }
        auto a{Address::from_base58(text)};
        o.pushKV("valid", a.has_value());
        // Whether it is the wallet's: only to a caller that may read the wallet's addresses.
        if (a) {
            if (env.may_call(WALLET_READ)) o.pushKV("mine", m.addresses().contains(*a));
        } else {
            o.pushKV("error", bt::btc::Address::parse(text) ? "an eCash address, not a BitAssets one" : text.rfind("s4_", 0) == 0 ? "a deposit address: use it on eCash" : "not a BitAssets address");
        }
        return o;
    };
    P["dropin.checkAddress"] = {};
    H["dropin.peg"] = [&m](const UniValue&, HostEnv&) { return m.peg(); };
    P["dropin.peg"] = {"pending_withdrawal_bundle", "get_wallet_utxos"};
    H["dropin.peers"] = [&m](const UniValue&, HostEnv&) {
        UniValue out{UniValue::VARR};
        for (auto& pv : m.node.peers()) {
            UniValue o{UniValue::VOBJ};
            o.pushKV("address", pv.address.to_string());
            o.pushKV("connected", pv.connected);
            o.pushKV("inbound", pv.inbound);
            o.pushKV("name", pv.name);
            out.push_back(o);
        }
        return out;
    };
    P["dropin.peers"] = {"list_peers"};
    H["dropin.peers.connect"] = [&m](const UniValue& p, HostEnv&) {
        std::string a{str_param(p, "address")};
        if (a.rfind(':') == std::string::npos) a += ":" + std::to_string(DEFAULT_PORT);
        std::string err;
        if (!m.node.connect_peer(a, err)) throw HostError(err);
        return UniValue{true};
    };
    P["dropin.peers.connect"] = {"connect_peer"};
    H["dropin.peers.forget"] = [&m](const UniValue& p, HostEnv&) {
        std::string a{str_param(p, "address")};
        if (a.rfind(':') == std::string::npos) a += ":" + std::to_string(DEFAULT_PORT);
        return UniValue{m.node.forget_peer(a)};
    };
    P["dropin.peers.forget"] = {"forget_peer"};
    // ---- the backup phrase: its words shown only in the app's trusted dialog, never to the pages.
    H["dropin.seed.status"] = [&m](const UniValue&, HostEnv&) { return m.seed_status(); };
    P["dropin.seed.status"] = {"get_wallet_addresses"};
    H["dropin.seed.prepare"] = [&m](const UniValue&, HostEnv&) {
        if (m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet has a backup phrase already");
        UniValue o{UniValue::VOBJ};
        o.pushKV("id", m.phrases.put(bt::hd::generate_mnemonic(12)));
        o.pushKV("words", 12);
        return o;
    };
    P["dropin.seed.prepare"] = {"set_seed_from_mnemonic"};
    H["dropin.seed.create"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string id{str_param(p, "id")};
        auto phrase{m.phrases.get(id)};
        if (!phrase) throw HostError(bt::appui::HOST_INVALID_REQUEST, "This new phrase is not known any more (5 minutes): make another");
        const bt::appui::WipeOnExit wipe_phrase{*phrase};
        if (m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet has a backup phrase already");
        Question q;
        const bt::appui::WipeOnExit wipe_words{q.details};
        q.title = "Your backup phrase";
        q.message = "Write these 12 words down, in order, on paper, and keep them safe. They are the only way to get this wallet back. "
                    "Anyone who has them can take its coins and assets. They are not shown to the app's pages.";
        q.details = Impl::words_block(*phrase);
        q.accept = "I wrote them down";
        q.level = "warning";
        q.kind = "phrase.create";
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        UniValue r{m.set_phrase(*phrase, "app")};
        m.phrases.erase(id);
        return r;
    };
    P["dropin.seed.create"] = {"set_seed_from_mnemonic"};
    H["dropin.seed.restore"] = [&m](const UniValue& p, HostEnv& env) {
        std::string phrase{str_param(p, "phrase")};
        const bt::appui::WipeOnExit wipe_typed{phrase};
        auto parsed{bt::hd::parse_mnemonic(phrase)};
        if (!parsed) throw HostError(bt::appui::HOST_INVALID_REQUEST, bt::hd::mnemonic_error(phrase));
        const bt::appui::WipeOnExit wipe_parsed{*parsed};
        if (m.wallet.has_seed()) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet has a backup phrase already");
        auto seed{bt::hd::mnemonic_to_seed(*parsed)};
        const std::string fp{bt::appui::seed_fingerprint(seed.span())};
        sodium_memzero(seed.data(), seed.size());
        Question q;
        q.title = "Restore the wallet from this phrase";
        q.message = "The wallet takes the keys of this backup phrase. Its coins and assets show once the node has looked at the chain.";
        q.rows = {{"Words", std::to_string(std::count(parsed->begin(), parsed->end(), ' ') + 1), "text"}, {"Fingerprint", fp, "text"}};
        q.accept = "Restore";
        q.kind = "phrase.restore";
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        return m.set_phrase(*parsed, "restored");
    };
    P["dropin.seed.restore"] = {"set_seed_from_mnemonic"};
    // Shown or saved only when named in the whitelist (no privilege stands for it): the most sensitive read there is.
    H["dropin.seed.show"] = [&m](const UniValue&, HostEnv& env) {
        auto phrase{m.kept_phrase()};
        if (!phrase) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet's phrase was not set through the app: keep the backup you made then");
        const bt::appui::WipeOnExit wipe_phrase{*phrase};
        Question q;
        const bt::appui::WipeOnExit wipe_words{q.details};
        q.title = "Your backup phrase";
        q.message = "Make sure nobody can see your screen. Anyone who has these words can take this wallet's coins and assets.";
        q.details = Impl::words_block(*phrase);
        q.accept = "Done";
        q.cancel = "Close";
        q.level = "warning";
        q.kind = "phrase.show";
        env.ask(q);
        return UniValue{true};
    };
    H["dropin.seed.save"] = [&m](const UniValue& p, HostEnv& env) {
        const std::string path{str_param(p, "path")};
        const auto file{env.file(path, true)};
        auto phrase{m.kept_phrase()};
        if (!phrase) throw HostError(bt::appui::RPC_WALLET_ERROR, "This wallet's phrase was not set through the app: keep the backup you made then");
        const bt::appui::WipeOnExit wipe_phrase{*phrase};
        Question q;
        q.title = "Save your backup phrase to a file";
        q.message = "Anyone who can read this file can take this wallet's coins and assets. Keep it somewhere only you can reach, off this computer if you can.";
        q.rows = {{"File", path, "text"}};
        q.accept = "Save";
        q.kind = "phrase.save";
        if (!env.ask(q).accepted) throw HostError(bt::appui::HOST_CANCELLED, "Cancelled");
        std::string content{"BitAssets backup phrase (BIP-39, plain_bitassets keys)\n" + *phrase + "\n"};
        const bt::appui::WipeOnExit wipe_content{content};
        try {
            bt::appui::ChosenFiles::write(file, content);
        } catch (const std::runtime_error& e) {
            throw HostError(std::string{"The file chosen cannot be written: "} + e.what());
        }
        return UniValue{true};
    };

    // ---- BitAssets' own: the assets of the chain, the wallet's holdings, the pools, the auctions.
    H["bitassets.assets"] = [&m](const UniValue& p, HostEnv& env) {
        const UniValue& lim{param(p, "limit")};
        const size_t limit{lim.isNum() ? static_cast<size_t>(std::clamp<int64_t>(lim.getInt<int64_t>(), 1, 5000)) : size_t{1000}};
        // What the wallet holds of each, and names in words, only to a reader of the wallet.
        const bool reader{env.may_call(WALLET_READ)};
        const Impl::Holdings h{reader ? m.holdings() : Impl::Holdings{}};
        UniValue out{UniValue::VARR};
        for (auto& [seq, id, data] : m.app.bitassets()) {
            if (out.size() >= limit) break;
            UniValue a{UniValue::VOBJ};
            a.pushKV("id", bt::hex_encode(id));
            a.pushKV("assetId", json::asset_id(AssetId::bitasset(id)));
            a.pushKV("seq", static_cast<uint64_t>(seq));
            if (auto rec{m.asset_record(id)}) a.pushKV("supply", std::to_string(rec->total_supply.latest().data));
            a.pushKV("data", json::bitasset_data(data));
            if (reader) {
                if (auto n{m.known_name(id)}) a.pushKV("name", *n);
                const auto held{h.assets.find(id)};
                a.pushKV("held", std::to_string(held == h.assets.end() ? 0 : held->second.first));
                a.pushKV("control", h.controls.contains(id));
            }
            out.push_back(a);
        }
        return out;
    };
    P["bitassets.assets"] = {"bitassets"};
    H["bitassets.asset"] = [&m](const UniValue& p, HostEnv& env) {
        const Hash32 id{hash_of(param(p, "id"), "id")};
        auto rec{m.asset_record(id)};
        if (!rec) throw HostError("No such BitAsset");
        const bool reader{env.may_call(WALLET_READ)};
        UniValue a{UniValue::VOBJ};
        a.pushKV("id", bt::hex_encode(id));
        a.pushKV("assetId", json::asset_id(AssetId::bitasset(id)));
        if (auto seq{m.asset_seq(id)}) a.pushKV("seq", static_cast<uint64_t>(*seq));
        a.pushKV("supply", std::to_string(rec->total_supply.latest().data));
        a.pushKV("data", json::bitasset_data(rec->current()));
        a.pushKV("registeredAt", static_cast<uint64_t>(rec->total_supply.v.front().height));
        a.pushKV("registrationTxid", bt::hex_encode(rec->total_supply.v.front().txid));
        UniValue pools{UniValue::VARR};
        for (auto& [pair, ps] : m.pools()) {
            if ((pair.first.kind != AssetId::Bitcoin && pair.first.id == id) || (pair.second.kind != AssetId::Bitcoin && pair.second.id == id))
                pools.push_back(m.pool_json(pair.first, pair.second, ps, reader));
        }
        a.pushKV("pools", pools);
        const Impl::Holdings h{reader ? m.holdings() : Impl::Holdings{}};
        const std::set<Hash32> receipts(h.receipts.begin(), h.receipts.end());
        UniValue auctions{UniValue::VARR};
        for (auto& [aid, st] : m.app.auctions()) {
            const bool of{(st.base_asset.kind == AssetId::BitAsset && st.base_asset.id == id) || (st.quote_asset.kind == AssetId::BitAsset && st.quote_asset.id == id)};
            if (of) auctions.push_back(m.auction_json(aid, st, reader, receipts));
        }
        a.pushKV("auctions", auctions);
        if (reader) {
            if (auto n{m.known_name(id)}) a.pushKV("name", *n);
            const auto held{h.assets.find(id)};
            a.pushKV("held", std::to_string(held == h.assets.end() ? 0 : held->second.first));
            a.pushKV("control", h.controls.contains(id));
        }
        return a;
    };
    P["bitassets.asset"] = {"bitasset_data", "get_amm_pool_state", "dutch_auctions"};
    H["bitassets.resolve"] = [&m](const UniValue& p, HostEnv& env) {
        std::string name{str_param(p, "name")};
        if (name.empty() || name.size() > 255) throw HostError(bt::appui::HOST_INVALID_REQUEST, "name: 1 to 255 bytes");
        UniValue o{UniValue::VOBJ};
        const Hash32 h{name_hash(name)};
        o.pushKV("name", name);
        o.pushKV("id", bt::hex_encode(h));
        o.pushKV("assetId", json::asset_id(AssetId::bitasset(h)));
        auto rec{m.asset_record(h)};
        o.pushKV("registered", rec.has_value());
        if (rec) {
            o.pushKV("supply", std::to_string(rec->total_supply.latest().data));
            o.pushKV("data", json::bitasset_data(rec->current()));
            // Remembered, for the wallet's reader to see this asset by its name from now on (the app's own store).
            if (env.may_call(WALLET_READ) && asset_name_problem(name).empty()) m.store->put("assetname:" + bt::hex_encode(h), name);
        }
        return o;
    };
    P["bitassets.resolve"] = {"bitasset_data"};
    H["bitassets.wallet"] = [&m](const UniValue&, HostEnv&) {
        // The wallet's holdings: ECX, each BitAsset, control coins, pool shares, auction receipts, reservations (and of
        // those, the names in a block not registered yet: registering them is the wallet's next step).
        UniValue o{UniValue::VOBJ};
        const Impl::Holdings h{m.holdings()};
        o.pushKV("ecx", n64(h.ecx));
        UniValue assets{UniValue::VARR};
        std::set<Hash32> ids;
        for (auto& [id, v] : h.assets) ids.insert(id);
        for (auto& id : h.controls) ids.insert(id);
        for (const Hash32& id : ids) {
            UniValue a{m.asset_json(AssetId::bitasset(id), true)};
            const auto held{h.assets.find(id)};
            a.pushKV("amount", std::to_string(held == h.assets.end() ? 0 : held->second.first));
            a.pushKV("coins", static_cast<uint64_t>(held == h.assets.end() ? 0 : held->second.second));
            a.pushKV("control", h.controls.contains(id));
            if (auto rec{m.asset_record(id)}) a.pushKV("supply", std::to_string(rec->total_supply.latest().data));
            assets.push_back(a);
        }
        o.pushKV("assets", assets);
        UniValue lp{UniValue::VARR};
        for (auto& [pair, amount] : h.lp) {
            UniValue x{UniValue::VOBJ};
            x.pushKV("asset0", m.asset_json(pair.first, true));
            x.pushKV("asset1", m.asset_json(pair.second, true));
            x.pushKV("lpTokens", std::to_string(amount));
            if (auto ps{m.app.pool(pair.first, pair.second)}; ps && ps->outstanding_lp_tokens > 0) {
                x.pushKV("share0", std::to_string(uint64_t(u128(ps->reserve0) * amount / ps->outstanding_lp_tokens)));
                x.pushKV("share1", std::to_string(uint64_t(u128(ps->reserve1) * amount / ps->outstanding_lp_tokens)));
                x.pushKV("pool", m.pool_json(pair.first, pair.second, *ps, true));
            }
            lp.push_back(x);
        }
        o.pushKV("lp", lp);
        UniValue receipts{UniValue::VARR};
        const std::set<Hash32> rs(h.receipts.begin(), h.receipts.end());
        for (const Hash32& id : h.receipts) {
            if (auto st{m.app.auction(id)}) receipts.push_back(m.auction_json(id, *st, true, rs));
        }
        o.pushKV("auctions", receipts);
        UniValue reserved{UniValue::VARR}, pending{UniValue::VARR}, registering{UniValue::VARR};
        std::set<OutPoint> spending;
        for (const AuthorizedTransaction& atx : m.node.mempool()) spending.insert(atx.transaction.inputs.begin(), atx.transaction.inputs.end());
        for (auto& [op, out] : h.reservations) {
            UniValue r{UniValue::VOBJ};
            r.pushKV("txid", bt::hex_encode(out.content.id));
            r.pushKV("commitment", bt::hex_encode(out.content.commitment));
            auto plain{m.wallet.reserved_name(out.content.commitment)};
            if (plain) r.pushKV("name", *plain);
            reserved.push_back(r);
            if (!plain || m.asset_record(name_hash(*plain))) continue;
            UniValue n{UniValue::VOBJ};
            n.pushKV("name", *plain);
            n.pushKV("id", bt::hex_encode(name_hash(*plain)));
            pending.push_back(n);
        }
        // A registration waiting for its block spends its reservation: wallet.utxos() leaves those out, so the mempool tells.
        for (const AuthorizedTransaction& atx : m.node.mempool()) {
            const Transaction& tx{atx.transaction};
            if (!tx.data || tx.data->kind != TxData::BitAssetRegistration) continue;
            auto plain{m.wallet.plain_name(tx.data->name_hash)};
            if (!plain) continue;
            bool ours{false};
            for (const Output& out : tx.outputs) ours = ours || m.wallet.is_mine(out.address);
            if (!ours) continue;
            UniValue n{UniValue::VOBJ};
            n.pushKV("name", *plain);
            n.pushKV("id", bt::hex_encode(tx.data->name_hash));
            registering.push_back(n);
        }
        o.pushKV("reservations", reserved);
        o.pushKV("pending", pending);
        o.pushKV("registering", registering);
        return o;
    };
    P["bitassets.wallet"] = {"get_wallet_utxos", "bitassets"};
    H["bitassets.pools"] = [&m](const UniValue&, HostEnv& env) {
        const bool reader{env.may_call(WALLET_READ)};
        UniValue out{UniValue::VARR};
        for (auto& [pair, ps] : m.pools()) {
            if (out.size() >= 2000) break;
            out.push_back(m.pool_json(pair.first, pair.second, ps, reader));
        }
        return out;
    };
    P["bitassets.pools"] = {"get_amm_pool_state"};
    H["bitassets.quote"] = [&m](const UniValue& p, HostEnv&) {
        // What a pool would give now, by the chain's own arithmetic (amm.rs): a swap, a deposit of both assets, a burn.
        const std::string kind{str_param(p, "kind")};
        UniValue o{UniValue::VOBJ};
        std::string err;
        if (kind == "swap") {
            const AssetId sp{asset_of(param(p, "spend"), "spend")}, rc{asset_of(param(p, "receive"), "receive")};
            const uint64_t amount{u64_of(param(p, "amount"), "amount")};
            if (sp == rc) throw HostError(bt::appui::HOST_INVALID_REQUEST, "Swap one asset for another");
            auto ps{m.app.pool(sp, rc)};
            o.pushKV("pool", ps.has_value());
            if (!ps) return o;
            std::optional<PoolState> np;
            uint64_t receive{0};
            try {
                if (sp < rc) {
                    np = ps->swap_asset0_for_asset1(amount, err);
                    if (np) receive = ps->reserve1 - np->reserve1;
                } else {
                    np = ps->swap_asset1_for_asset0(amount, err);
                    if (np) receive = ps->reserve0 - np->reserve0;
                }
            } catch (const StateError&) {
                np.reset();
                err = "The pool cannot price this swap";
            }
            if (!np) {
                o.pushKV("error", err);
                return o;
            }
            o.pushKV("receive", std::to_string(receive));
            const bool zero_first{sp < rc};
            o.pushKV("reserveSpend", std::to_string(zero_first ? ps->reserve0 : ps->reserve1));
            o.pushKV("reserveReceive", std::to_string(zero_first ? ps->reserve1 : ps->reserve0));
            return o;
        }
        AssetId a{asset_of(param(p, "asset0"), "asset0")}, b{asset_of(param(p, "asset1"), "asset1")};
        if (a == b) throw HostError(bt::appui::HOST_INVALID_REQUEST, "A pool is of two different assets");
        if (kind == "mint") {
            uint64_t x0{u64_of(param(p, "amount0"), "amount0")}, x1{u64_of(param(p, "amount1"), "amount1")};
            if (b < a) {
                std::swap(a, b);
                std::swap(x0, x1);
            }
            auto ps{m.app.pool(a, b)};
            o.pushKV("pool", ps.has_value());
            const PoolState cur{ps ? *ps : PoolState::fresh(Hash32{})};
            std::optional<PoolState> np;
            try {
                np = cur.mint(x0, x1, err);
            } catch (const StateError&) {
                err = "The pool cannot take these amounts";
            }
            if (!np || np->outstanding_lp_tokens < cur.outstanding_lp_tokens) {
                o.pushKV("error", np ? "AMM LP token overflow" : err);
                return o;
            }
            o.pushKV("lpTokens", std::to_string(np->outstanding_lp_tokens - cur.outstanding_lp_tokens));
            o.pushKV("reserve0", std::to_string(np->reserve0));
            o.pushKV("reserve1", std::to_string(np->reserve1));
            return o;
        }
        if (kind == "burn") {
            const uint64_t lp{u64_of(param(p, "lpTokens"), "lpTokens")};
            const AssetId a0{std::min(a, b)}, a1{std::max(a, b)};
            auto ps{m.app.pool(a0, a1)};
            o.pushKV("pool", ps.has_value());
            if (!ps) return o;
            auto np{ps->burn(lp, err)};
            if (!np) {
                o.pushKV("error", err);
                return o;
            }
            o.pushKV("out0", std::to_string(ps->reserve0 - np->reserve0));
            o.pushKV("out1", std::to_string(ps->reserve1 - np->reserve1));
            return o;
        }
        throw HostError(bt::appui::HOST_INVALID_REQUEST, "kind: swap, mint or burn");
    };
    P["bitassets.quote"] = {"get_amm_pool_state"};
    H["bitassets.auctions"] = [&m](const UniValue&, HostEnv& env) {
        const bool reader{env.may_call(WALLET_READ)};
        const Impl::Holdings h{reader ? m.holdings() : Impl::Holdings{}};
        const std::set<Hash32> receipts(h.receipts.begin(), h.receipts.end());
        UniValue out{UniValue::VARR};
        for (auto& [id, st] : m.app.auctions()) {
            if (out.size() >= 2000) break;
            out.push_back(m.auction_json(id, st, reader, receipts));
        }
        return out;
    };
    P["bitassets.auctions"] = {"dutch_auctions"};
    H["bitassets.bidQuote"] = [&m](const UniValue& p, HostEnv&) {
        const Hash32 id{hash_of(param(p, "id"), "id")};
        const uint64_t bid{u64_of(param(p, "bid"), "bid")};
        auto st{m.app.auction(id)};
        if (!st) throw HostError("No such auction");
        const uint32_t height{m.next_height()};
        UniValue o{UniValue::VOBJ};
        o.pushKV("height", static_cast<uint64_t>(height));
        auto price{auction_price_at(*st, height)};
        o.pushKV("price", price ? UniValue(std::to_string(*price)) : UniValue{});
        std::string err;
        std::optional<AuctionState> ns;
        try {
            ns = st->bid(Hash32{}, bid, height, err);
        } catch (const StateError&) {
            err = "BitAssets would refuse this bid now";
        }
        if (!ns) {
            o.pushKV("error", err);
            return o;
        }
        o.pushKV("quantity", std::to_string(st->base_amount_remaining.latest().data - ns->base_amount_remaining.latest().data));
        return o;
    };
    P["bitassets.bidQuote"] = {"dutch_auctions"};

    chain.snapshot = [&m] { return m.poller ? m.poller->snapshot() : UniValue{UniValue::VOBJ}; };
    host_ = std::make_unique<bt::appui::RemoteHost>(std::move(chain), bt::appui::HostOptions{m.options.chooser, m.options.file_dir}, *server_);
    m.poller = std::make_unique<bt::appui::Poller>(host_->events(), WALLET_NAME, [&m] { return m.look(); });
    if (!server_->listen(err)) return false;
    m.link->start();
    m.poller->start();
    LOG_INFO("app RPC on 127.0.0.1:%u (%zu methods; the LayerTwo RPC is not changed by it)", unsigned(server_->port()), server_->methods().size());
    return true;
}

void AppLayer::stop()
{
    if (!m_) return;
    if (m_->poller) m_->poller->stop();
    if (m_->link) m_->link->stop();
    if (host_) host_->stop();
    if (server_) server_->stop();
}

std::vector<std::string> AppLayer::app_methods() const
{
    std::vector<std::string> out{server_ ? server_->methods() : std::vector<std::string>{}};
    return out;
}

UniValue AppLayer::manifest() const
{
    // What a pack of BitAssets' pages uses of this node (the unified wallet names it in the app's whitelist): the
    // reads of the pages, the confirmed methods, the host methods, and the host's own methods.
    UniValue o{UniValue::VOBJ};
    o.pushKV("stem", "bitassets");
    o.pushKV("hostApi", bt::appui::HOST_API);
    UniValue reads{UniValue::VARR};
    for (const char* r : SHELL_READS) reads.push_back(r);
    for (const std::string& r : l2l_reads()) reads.push_back(r);
    o.pushKV("reads", reads);
    UniValue confirmed{UniValue::VARR};
    for (const std::string& c : confirmed_methods()) confirmed.push_back(c);
    o.pushKV("confirmed", confirmed);
    UniValue host{UniValue::VARR};
    for (const std::string& h : host_methods()) host.push_back(h);
    o.pushKV("host", host);
    UniValue protocol{UniValue::VARR};
    for (const char* p : {"webuihost", "describeconfirmation", "webuievents", "webuichosenfile", "appserverproof"}) protocol.push_back(p);
    o.pushKV("protocol", protocol);
    return o;
}

} // namespace bitassets
