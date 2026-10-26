// Known answers against public eCash betanet data (slot 4): every block fetched from the
// public seed peer (bitassets-fetch-blocks), the enforcer's BMM/deposit/bundle events
// (GetTwoWayPegData), and the seed index (esplora facade).
#include "test.h"

#include <bitassets/fixture.h>

#include <cstdio>
#include <set>

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

TEST(betanet_blocks_codec_and_hashes)
{
    // bincode decode -> re-encode identical; header hash == the index's id; the index's merkle
    // root; header chain linked from genesis. The index's height-0 row (f761f9e6) is a stale
    // leftover of the pre-0.17 chain (study); compare heights 1..tip.
    auto& d = data();
    REQUIRE(d.blocks.size() >= 1080);
    std::optional<Hash32> prev;
    uint32_t exact = 0, index_ids = 0, index_roots = 0;
    for (auto& [h, raw] : d.blocks) {
        Block b = fixture::decode_block_response(raw);
        Writer w;
        w.u32(0);
        bincode(w, b.header);
        bincode(w, b.body);
        if (w.buf == raw) exact++;
        CHECK(b.header.prev_side_hash == prev);
        prev = b.header.hash();
        auto it = d.index.find(h);
        if (it != d.index.end() && h > 0) {
            if (it->second.id == *prev) index_ids++;
            if (it->second.merkle_root == b.header.merkle_root) index_roots++;
        }
    }
    std::fprintf(stderr, "  %zu blocks, %u byte-exact, %u ids and %u merkle roots equal the index (heights 1..%u)\n", d.blocks.size(), exact,
                 index_ids, index_roots, d.index.rbegin()->first);
    CHECK_EQ(exact, uint32_t(d.blocks.size()));
    CHECK_EQ(index_ids, d.index.rbegin()->first);
    CHECK_EQ(index_roots, d.index.rbegin()->first);
    CHECK_EQ(bt::hex_encode(*prev), std::string("195ef084e812d3aef67e9164f1fdb8b4cd2c0bbcd0ff60b102b65c4d25ddde5f")); // tip 1079
    // block 0 BMM'd at mainchain 968,864, before the enforcer's reported activation 968,998
    auto& g = d.main_by_height.begin()->second;
    CHECK_EQ(g.header.height, 968864u);
    CHECK(g.info.bmm_commitment == fixture::decode_block_response(d.blocks[0].second).header.hash());
}

TEST(betanet_index_tx_hex_borsh)
{
    // every tx the seed index serves: borsh decode -> re-encode identical, blake3 == txid,
    // bincode round trip
    size_t n = 0;
    for (auto& [txid, hex] : data().txhex) {
        auto raw = bt::hex_decode(hex);
        REQUIRE(raw.has_value());
        Reader r(*raw);
        Transaction t = borsh_de_transaction(r);
        CHECK(r.empty());
        CHECK(t.canonical_bytes() == *raw);
        CHECK_EQ(bt::hex_encode(t.txid()), txid);
        Writer w;
        bincode(w, t);
        Reader br(w.buf);
        CHECK(de_transaction(br).canonical_bytes() == *raw);
        n++;
    }
    CHECK_EQ(n, size_t(14));
}

static std::string kinds(const fixture::ReplayResult& r)
{
    static const char* names[] = {"AmmBurn", "AmmMint", "AmmSwap", "Reservation", "Registration", "Mint", "Update", "AuctionCreate", "AuctionBid", "AuctionCollect"};
    std::string s;
    for (auto& [k, n] : r.data_kinds) s += std::string(names[k]) + "=" + std::to_string(n) + " ";
    return s + "transfers=" + std::to_string(r.transfers);
}

TEST(betanet_replay_all_checks)
{
    // every block through prevalidate + connect with every check on, 2WPD windows from the
    // enforcer; then the state the chain implies.
    bt::kv::MemStore store;
    State st;
    auto r = fixture::replay(data(), st, store);
    std::fprintf(stderr, "  %u blocks connected, %u byte-exact, %u BMM verified, %u signatures, %u tx inclusions (%zu distinct txids, %u zero-input), %s, %u deposits; %s\n",
                 r.blocks, r.reencoded_exact, r.bmm_verified, r.signatures, r.tx_inclusions, r.txid_counts.size(), r.zero_input_txs, kinds(r).c_str(),
                 r.deposits, r.error.c_str());
    CHECK(r.error.empty());
    CHECK_EQ(r.blocks, uint32_t(data().blocks.size()));
    CHECK_EQ(r.bmm_verified, r.blocks);
    bt::kv::Txn t(store);
    size_t dep_events = 0;
    for (uint32_t h = 0; h <= *st.height(t); h++) dep_events += st.block_index_events(t, h).deposits.size();
    CHECK_EQ(dep_events, size_t(6)); // deposits seq 0..5 precede the halt
    auto pending = st.pending_bundle(t);
    std::fprintf(stderr, "  tip height %u, %zu utxos, %zu stxos, %zu bitassets, %zu open reservations, %zu pools, %zu auctions, wealth %llu sats, pending m6id %s\n",
                 *st.height(t), st.utxos(t).size(), st.stxos(t).size(), st.bitassets(t).size(), st.reservations(t).size(), st.pools(t).size(),
                 st.auctions(t).size(), (unsigned long long)st.sidechain_wealth(t),
                 pending ? bt::hex_encode_rev(pending->first.m6id().span()).c_str() : "none");
    // the enforcer reports bundle 0fe864bd... Submitted at mainchain 971,770 (after the halt):
    // it is the bundle our state has pending at the tip, so block 1080 will connect it as Known
    REQUIRE(pending.has_value());
    // (the enforcer's JSON carries the m6id in internal byte order; bitcoin displays it reversed)
    CHECK_EQ(bt::hex_encode(pending->first.m6id()), std::string("0fe864bdeab117cf4804d2f9429077fed9a3d90f9767c8058552f85e7b618d4d"));
    for (auto& [id, rec] : st.bitassets(t))
        std::fprintf(stderr, "  bitasset %s seq %u supply %llu\n", bt::hex_encode(id).c_str(), *st.bitasset_seq(t, id),
                     (unsigned long long)rec.total_supply.latest().data);
}

TEST(betanet_rollback_and_reconnect)
{
    // Disconnect blocks newest first (2WPD window, then the block, as net_task.rs
    // disconnect_tip_ does) until the first block their node cannot disconnect either (known
    // upstream consensus behaviour reproduced), then reconnect and compare the state.
    bt::kv::MemStore store;
    State st;
    std::vector<fixture::ReplayStep> steps;
    auto r = fixture::replay(data(), st, store, &steps);
    REQUIRE(r.error.empty());
    auto snapshot = [&] {
        bt::kv::Txn t(store);
        std::string s;
        for (auto& [op, o] : st.utxos(t)) s += op.to_string() + ";";
        for (auto& [op, o] : st.stxos(t)) s += "s" + op.to_string() + ";";
        for (auto& [id, rec] : st.bitassets(t)) s += bt::hex_encode(id) + std::to_string(rec.total_supply.latest().data) + ";";
        for (auto& [txid, c] : st.reservations(t)) s += bt::hex_encode(txid) + ";";
        return s + std::to_string(st.sidechain_wealth(t));
    };
    std::string tip_state = snapshot();
    size_t i = steps.size();
    std::string stop;
    while (i > 0) {
        bt::kv::Txn t(store);
        try {
            st.disconnect_2wpd(t, steps[i - 1].window);
            st.disconnect_tip(t, steps[i - 1].block.header, steps[i - 1].block.body);
        } catch (const StateError& e) {
            stop = e.what();
            break;
        }
        t.commit();
        i--;
    }
    std::fprintf(stderr, "  disconnected %zu blocks; stopped at height %zu: %s\n", steps.size() - i, i ? i - 1 : 0, stop.empty() ? "(genesis)" : stop.c_str());
    for (size_t k = i; k < steps.size(); k++) {
        bt::kv::Txn t(store);
        auto pre = st.prevalidate(t, steps[k].block.header, steps[k].block.body);
        st.connect_prevalidated(t, steps[k].block.header, steps[k].block.body, pre);
        st.connect_2wpd(t, steps[k].window);
        t.commit();
    }
    CHECK(snapshot() == tip_state);
}

TEST(betanet_replay_census)
{
    // public betanet data: txids confirmed at more than one height (197/199 and 253/259/261)
    std::map<Hash32, std::vector<uint32_t>> where;
    for (auto& [h, raw] : data().blocks)
        for (auto& t : fixture::decode_block_response(raw).body.transactions) where[t.txid()].push_back(h);
    for (auto& [id, hs] : where) {
        if (hs.size() < 2) continue;
        std::string s;
        for (auto x : hs) s += std::to_string(x) + " ";
        std::fprintf(stderr, "  txid %s confirmed at heights %s\n", bt::hex_encode(id).c_str(), s.c_str());
    }
    std::set<std::vector<uint32_t>> sets;
    for (auto& [id, hs] : where)
        if (hs.size() > 1) sets.insert(hs);
    CHECK(sets.count({197, 199}) == 1);
    CHECK(sets.count({253, 259, 261}) == 1);
}
