// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bitassets/app.h>
#include <bitassets/json.h>

#include <set>

#include <bt/ecies.h>
#include <bt/hd.h>
#include <bt/log.h>

#include <limits>

namespace bitassets {

using bt::jsonrpc::app_error;
using bt::jsonrpc::Params;
using bt::kv::Txn;

// app.rs build_block_template: on a mainchain reorg below the tip's prev_main, mine an
// empty block on the last ancestor whose prev_main is still an ancestor.
BlockTemplate App::build_block_template(std::optional<uint64_t> fee)
{
    auto tipr = node.enforcer().get_chain_tip();
    if (!bt::enforcer::ok(tipr)) throw app_error(bt::enforcer::err(tipr).what());
    Hash32 prev_main = bt::enforcer::val(tipr).block_hash;
    auto tip_hash = node.tip();
    std::optional<Hash32> prev_side;
    if (tip_hash) {
        if (!node.request_mainchain_ancestor_infos(prev_main)) throw app_error("Failed to request mainchain ancestor info for " + bt::hex_encode_rev(prev_main.span()));
        std::lock_guard<std::recursive_mutex> l(node.lock());
        Txn t(node.store());
        Header th = node.archive().need_header(t, *tip_hash);
        Hash32 lcma = node.archive().last_common_main_ancestor(t, prev_main, th.prev_main_hash);
        if (lcma == th.prev_main_hash) prev_side = tip_hash;
        else {
            std::optional<Hash32> cur = tip_hash;
            while (cur) {
                Header h = node.archive().need_header(t, *cur);
                if (h.prev_main_hash != lcma && node.archive().is_main_descendant(t, h.prev_main_hash, lcma)) {
                    prev_side = cur;
                    break;
                }
                cur = h.prev_side_hash;
            }
        }
    }
    BlockTemplate bt_;
    if (prev_side == tip_hash) {
        auto r = node.mempool_transactions(1000); // NUM_TRANSACTIONS (app.rs:495)
        std::vector<AuthorizedTransaction> ats;
        for (auto& [tx, _] : r.first) ats.push_back(tx);
        Coinbase cb;
        if (r.second) {
            Output o;
            o.address = wallet.get_receive_address();
            o.content.kind = Content::Bitcoin;
            o.content.value = r.second;
            cb.outputs.push_back(o);
        }
        bt_.body = Body::from_authorized(ats, cb);
        std::vector<FilledTransaction> filled;
        for (size_t i = 0; i < r.first.size(); i++) filled.push_back(FilledTransaction{&bt_.body.transactions[i], r.first[i].second});
        auto mr = compute_merkle_root(bt_.body.coinbase, filled);
        if (!mr) throw app_error("ComputeMerkleRoot");
        bt_.header.merkle_root = *mr;
        bt_.fees = r.second;
        bt_.bribe = fee ? *fee : (r.second ? r.second : EMPTY_BLOCK_BMM_BRIBE);
    } else {
        bt_.header.merkle_root = *compute_merkle_root(Coinbase{}, std::vector<FilledTransaction>{});
        bt_.bribe = EMPTY_BLOCK_BMM_BRIBE;
    }
    bt_.header.prev_side_hash = prev_side;
    bt_.header.prev_main_hash = prev_main;
    bt_.height = prev_side ? *node.block_height(*prev_side) + 1 : 0;
    return bt_;
}

// app.rs mine + miner.rs attempt_bmm/confirm_bmm
void App::mine(std::optional<uint64_t> fee)
{
    auto tpl = build_block_template(fee);
    Hash32 h = tpl.header.hash();
    auto r = node.enforcer().create_bmm_critical_data_tx(tpl.bribe, 0, h, tpl.header.prev_main_hash);
    if (!bt::enforcer::ok(r)) throw app_error(bt::enforcer::err(r).what());
    LOG_INFO("created BMM tx %s", bt::hex_encode_rev(bt::enforcer::val(r).span()).c_str());
    std::optional<Hash32> main_hash;
    std::atomic<bool> cancel{false};
    node.enforcer().subscribe_events(
        [&](const bt::enforcer::SubscribeEvent& ev) {
            if (ev.kind == bt::enforcer::SubscribeEvent::Connect && ev.info.bmm_commitment == h) main_hash = ev.header.block_hash;
            return false; // only the first event of the subscription is considered
        },
        &cancel);
    if (!main_hash) return;
    if (!node.submit_block(*main_hash, tpl.header, tpl.body)) LOG_WARN("mine: BMM not accepted as new tip");
}

bool App::connect_block(const Header& h, const Body& b, const Hash32& main_block_hash) { return node.submit_block(main_block_hash, h, b); }

Hash32 App::deposit(const Address& a, uint64_t value, uint64_t fee)
{
    auto r = node.enforcer().create_deposit_tx(a.to_base58(), value, fee);
    if (!bt::enforcer::ok(r)) throw app_error(bt::enforcer::err(r).what());
    return bt::enforcer::val(r);
}

// ------------------------------------------------------------------ state queries (node/mod.rs)
std::vector<std::tuple<uint32_t, Hash32, BitAssetData>> App::bitassets()
{
    // node/mod.rs:279-312: seq_to_bitasset in LMDB (memcmp of LE u32) key order
    std::lock_guard<std::recursive_mutex> l(node.lock());
    bt::kv::Txn t(node.store());
    std::vector<std::tuple<uint32_t, Hash32, BitAssetData>> r;
    for (auto& [seq, id] : node.state().seq_to_bitasset(t)) {
        auto rec = node.state().bitasset(t, id);
        if (!rec) throw app_error("missing BitAsset " + bt::hex_encode(id));
        r.emplace_back(seq, id, rec->current());
    }
    return r;
}

std::optional<BitAssetData> App::bitasset_data(const Hash32& id)
{
    std::lock_guard<std::recursive_mutex> l(node.lock());
    bt::kv::Txn t(node.store());
    auto rec = node.state().bitasset(t, id);
    if (!rec) return std::nullopt;
    return rec->current();
}

std::optional<PoolState> App::pool(const AssetId& a0, const AssetId& a1)
{
    std::lock_guard<std::recursive_mutex> l(node.lock());
    bt::kv::Txn t(node.store());
    return node.state().pool(t, a0, a1);
}

std::vector<std::pair<Hash32, AuctionState>> App::auctions()
{
    std::lock_guard<std::recursive_mutex> l(node.lock());
    bt::kv::Txn t(node.store());
    return node.state().auctions(t);
}

std::optional<AuctionState> App::auction(const Hash32& id)
{
    std::lock_guard<std::recursive_mutex> l(node.lock());
    bt::kv::Txn t(node.store());
    return node.state().auction(t, id);
}

// ------------------------------------------------------------------ RPC
namespace {

// jsonrpsee answers -32602 when a parameter does not deserialize: json::Error is that error (with serde's cause).
template <class F>
auto P(F f)
{
    return [f](const Params& p) -> UniValue { return f(p); };
}

std::set<Address> parse_addresses(const UniValue& v)
{
    std::set<Address> s;
    if (!v.isArray()) throw json::Error("expected an array");
    for (auto& a : v.getValues()) s.insert(json::parse_address(a));
    return s;
}

UniValue arr() { return UniValue(UniValue::VARR); }

// derived Debug of BitAssetId([u8; 32]): "BitAssetId([1, 2, ...])"
std::string bitasset_id_debug(const Hash32& h)
{
    std::string r = "BitAssetId([";
    for (size_t i = 0; i < 32; i++) r += (i ? ", " : "") + std::to_string(int(h[i]));
    return r + "])";
}

UniValue openapi_schema(const bt::jsonrpc::Server& s)
{
    // utoipa's full schema is not reproduced; the methods are listed for introspection.
    UniValue r(UniValue::VOBJ);
    r.pushKV("openapi", "3.1.0");
    UniValue info(UniValue::VOBJ);
    info.pushKV("title", "plain_bitassets");
    info.pushKV("version", VERSION);
    r.pushKV("info", info);
    UniValue paths(UniValue::VOBJ);
    for (auto& m : s.methods()) paths.pushKV("/" + m, UniValue(UniValue::VOBJ));
    r.pushKV("paths", paths);
    return r;
}

UniValue broadcast_result(const Hash32& txid, size_t peers)
{
    UniValue o(UniValue::VOBJ);
    o.pushKV("txid", json::hash(txid));
    o.pushKV("peer_count", UniValue(uint64_t(peers)));
    return o;
}

std::string str_param(const UniValue& v)
{
    if (!v.isStr()) throw json::Error("expected a string");
    return v.get_str();
}

} // namespace

void register_rpc(bt::jsonrpc::Server& s, App& app, bool public_api, bool private_api)
{
    Node& n = app.node;
    Wallet& w = app.wallet;
    s.add("openapi_schema", {}, 0, [&s](const Params&) { return openapi_schema(s); });
    if (public_api) {
        s.add("bitasset_data", {"bitasset_id"}, 1, P([&app](const Params& p) -> UniValue {
                  Hash32 id = json::parse_hash(p[0]);
                  auto d = app.bitasset_data(id);
                  if (!d) throw app_error("state error: missing BitAsset " + bitasset_id_debug(id));
                  return json::bitasset_data(*d);
              }));
        s.add("bitassets", {}, 0, [&app](const Params&) {
            UniValue a = arr();
            for (auto& [seq, id, d] : app.bitassets()) {
                UniValue t = arr();
                t.push_back(UniValue(uint64_t(seq)));
                t.push_back(json::hash(id));
                t.push_back(json::bitasset_data(d));
                a.push_back(t);
            }
            return a;
        });
        s.add("connect_block", {"block", "main_block_hash"}, 2, P([&app](const Params& p) {
                  json::require_struct(p[0], "Block", {"header", "body", "height"});
                  Header h = json::parse_header(p[0]["header"]);
                  Body b = json::parse_body(p[0]["body"]);
                  return UniValue(app.connect_block(h, b, json::parse_btc_hash(p[1])));
              }));
        s.add("dutch_auctions", {}, 0, [&app](const Params&) {
            UniValue a = arr();
            for (auto& [id, st] : app.auctions()) {
                UniValue t = arr();
                t.push_back(json::hash(id));
                t.push_back(json::auction_state(st));
                a.push_back(t);
            }
            return a;
        });
        s.add("get_amm_pool_state", {"asset0", "asset1"}, 2, P([&app](const Params& p) -> UniValue {
                  AssetId a0 = json::parse_asset_id(p[0]), a1 = json::parse_asset_id(p[1]);
                  auto ps = app.pool(a0, a1);
                  if (!ps) throw app_error("state error: missing AMM pool state for " + std::min(a0, a1).to_hex() + "-" + std::max(a0, a1).to_hex());
                  return json::pool_state(*ps);
              }));
        s.add("get_amm_price", {"base", "quote"}, 2, P([&app](const Params& p) -> UniValue {
                  AssetId base = json::parse_asset_id(p[0]), quote = json::parse_asset_id(p[1]);
                  auto ps = app.pool(base, quote);
                  if (!ps || ps->reserve0 == 0 || ps->reserve1 == 0) return UniValue();
                  return base < quote ? json::fraction(ps->reserve1, ps->reserve0) : json::fraction(ps->reserve0, ps->reserve1);
              }));
        s.add("get_block", {"block_hash"}, 1, P([&n](const Params& p) -> UniValue {
                  Hash32 h = json::parse_hash(p[0]);
                  auto hd = n.header(h);
                  auto b = n.body(h);
                  auto ht = n.block_height(h);
                  if (!hd || !b || !ht) throw app_error("archive error: no block with hash " + bt::hex_encode(h)); // theirs panics (expect)
                  return json::block(*hd, *b, *ht);
              }));
        s.add("get_block_hash", {"height"}, 1, P([&n](const Params& p) {
                  auto r = n.block_hash_at(json::parse_u32(p[0]));
                  return r ? json::hash(*r) : UniValue();
              }));
        s.add("get_block_index", {"block_hash"}, 1, P([&n](const Params& p) {
                  Hash32 h = json::parse_hash(p[0]);
                  auto b = n.body(h);
                  if (!b) throw app_error("archive error: no block body with hash " + bt::hex_encode(h));
                  auto ev = n.block_index_events(h);
                  if (!ev) throw app_error("Block " + bt::hex_encode(h) + " is not in the current chain");
                  UniValue r(UniValue::VOBJ), txs = arr(), deps = arr(), spends = arr();
                  for (auto& t : b->transactions) {
                      UniValue x(UniValue::VOBJ);
                      Bytes raw = t.canonical_bytes();
                      x.pushKV("txid", json::hash(bt::blake3(raw)));
                      x.pushKV("size", UniValue(uint64_t(raw.size())));
                      x.pushKV("raw", bt::hex_encode(raw));
                      txs.push_back(x);
                  }
                  for (auto& [op, o] : ev->deposits) deps.push_back(json::pointed(op, json::filled_output(o)));
                  for (auto& [op, m] : ev->bundle_spends) {
                      UniValue x(UniValue::VOBJ);
                      x.pushKV("outpoint", json::outpoint(op));
                      x.pushKV("m6id", json::btc_hash(m));
                      spends.push_back(x);
                  }
                  r.pushKV("txs", txs);
                  r.pushKV("deposits", deps);
                  r.pushKV("bundle_spends", spends);
                  return r;
              }));
        s.add("get_bmm_inclusions", {"block_hash"}, 1, P([&n](const Params& p) {
                  UniValue a = arr();
                  for (auto& m : n.bmm_inclusions(json::parse_hash(p[0]))) a.push_back(json::btc_hash(m));
                  return a;
              }));
        s.add("get_best_mainchain_block_hash", {}, 0, [&n](const Params&) -> UniValue {
            auto t = n.tip();
            if (!t) return UniValue();
            auto m = n.best_main_verification(*t);
            if (!m) throw app_error("archive error: no BMM result with block " + bt::hex_encode(*t));
            return json::btc_hash(*m);
        });
        s.add("get_best_sidechain_block_hash", {}, 0, [&n](const Params&) {
            auto t = n.tip();
            return t ? json::hash(*t) : UniValue();
        });
        s.add("get_stxos", {"addresses"}, 1, P([&n](const Params& p) {
                  UniValue a = arr();
                  for (auto& [op, so] : n.stxos_by_addresses(parse_addresses(p[0]))) {
                      UniValue x(UniValue::VOBJ);
                      x.pushKV("outpoint", json::outpoint(op));
                      x.pushKV("output", json::spent_output(so));
                      a.push_back(x);
                  }
                  return a;
              }));
        s.add("get_transaction", {"txid"}, 1, P([&n](const Params& p) -> UniValue {
                  auto t = n.transaction(json::parse_hash(p[0]));
                  return t ? json::transaction(*t) : UniValue();
              }));
        s.add("get_transaction_info", {"txid"}, 1, P([&n](const Params& p) -> UniValue {
                  auto r = n.filled_transaction(json::parse_hash(p[0]));
                  if (!r) return UniValue();
                  UniValue o(UniValue::VOBJ);
                  UniValue conf, txin;
                  if (r->second) {
                      uint32_t tip_height = *n.height();
                      uint32_t h = *n.block_height(r->second->first);
                      conf = UniValue(uint64_t(tip_height - h));
                      txin = UniValue(UniValue::VOBJ);
                      txin.pushKV("block_hash", json::hash(r->second->first));
                      txin.pushKV("idx", UniValue(uint64_t(r->second->second)));
                  }
                  FilledTransaction ft{&r->first.first.transaction, r->first.second};
                  auto fee = ft.bitcoin_fee();
                  if (!fee) throw app_error("bitcoin fee: value in overflow, value out overflow, or underfunded");
                  o.pushKV("confirmations", conf);
                  o.pushKV("fee_sats", UniValue(*fee));
                  o.pushKV("txin", txin);
                  return o;
              }));
        s.add("get_utxos", {"addresses"}, 1, P([&n](const Params& p) {
                  UniValue a = arr();
                  for (auto& [op, o] : n.utxos_by_addresses(parse_addresses(p[0]))) a.push_back(json::pointed(op, json::filled_output(o)));
                  return a;
              }));
        s.add("getblockcount", {}, 0, [&n](const Params&) {
            auto h = n.height();
            return UniValue(uint64_t(h ? *h + 1 : 0));
        });
        s.add("latest_failed_withdrawal_bundle_height", {}, 0, [&n](const Params&) {
            auto h = n.latest_failed_withdrawal_bundle_height();
            return h ? UniValue(uint64_t(*h)) : UniValue();
        });
        s.add("list_mempool", {}, 0, [&n](const Params&) {
            UniValue a = arr();
            for (auto& at : n.mempool()) {
                UniValue x(UniValue::VOBJ);
                Bytes raw = at.transaction.canonical_bytes();
                x.pushKV("txid", json::hash(bt::blake3(raw)));
                x.pushKV("size", UniValue(uint64_t(raw.size())));
                x.pushKV("raw", bt::hex_encode(raw));
                x.pushKV("tx", json::transaction(at.transaction));
                a.push_back(x);
            }
            return a;
        });
        s.add("list_peers", {}, 0, [&n](const Params&) {
            UniValue a = arr();
            for (auto& pv : n.peers()) {
                UniValue o(UniValue::VOBJ);
                o.pushKV("address", pv.address.to_string());
                o.pushKV("status", pv.connected ? "Connected" : "Connecting");
                a.push_back(o);
            }
            return a;
        });
        s.add("list_utxos", {}, 0, [&n](const Params&) {
            UniValue a = arr();
            for (auto& [op, o] : n.utxos()) a.push_back(json::pointed(op, json::filled_output(o)));
            return a;
        });
        s.add("mainchain_sync_progress", {}, 0, [&n](const Params&) {
            auto sp = n.sync_progress();
            static const char* PH[] = {"idle", "headers", "writing", "state"};
            UniValue o(UniValue::VOBJ);
            o.pushKV("phase", PH[sp.phase]);
            o.pushKV("done", UniValue(uint64_t(sp.done)));
            o.pushKV("total", UniValue(uint64_t(sp.total)));
            o.pushKV("tip_height", UniValue(uint64_t(sp.tip_height)));
            return o;
        });
        s.add("pending_withdrawal_bundle", {}, 0, [&n](const Params&) {
            auto b = n.pending_withdrawal_bundle();
            return b ? json::withdrawal_bundle(*b) : UniValue();
        });
        s.add("sidechain_wealth", {}, 0, [&n](const Params&) { return UniValue(n.sidechain_wealth()); });
        s.add("get_authorized_transaction", {"txid"}, 1, P([&n](const Params& p) -> UniValue {
                  // node/mod.rs:468-478: the mempool only (a confirmed transaction is null), as theirs
                  Hash32 id = json::parse_hash(p[0]);
                  for (auto& at : n.mempool())
                      if (at.transaction.txid() == id) return json::authorized_transaction(at);
                  return UniValue();
              }));
        s.add("broadcast_transaction", {"transaction"}, 1, P([&n](const Params& p) {
                  auto tx = json::parse_authorized_transaction(p[0]);
                  size_t peers = n.broadcast_transaction(tx);
                  return broadcast_result(tx.transaction.txid(), peers);
              }));
        s.add("rebroadcast_transaction", {"txid"}, 1, P([&n](const Params& p) {
                  Hash32 id = json::parse_hash(p[0]);
                  return broadcast_result(id, n.rebroadcast_transaction(id));
              }));
        s.add("submit_transaction", {"transaction"}, 1, P([&n](const Params& p) {
                  auto tx = json::parse_authorized_transaction(p[0]);
                  n.broadcast_transaction(tx);
                  return json::hash(tx.transaction.txid());
              }));
    }
    if (private_api) {
        s.add("connect_peer", {"addr"}, 1, P([&n](const Params& p) {
                  std::string a = str_param(p[0]), err;
                  if (!n.connect_peer(a, err)) throw app_error(err);
                  return UniValue();
              }));
        s.add("forget_peer", {"addr"}, 1, P([&n](const Params& p) {
                  std::string a = str_param(p[0]);
                  if (a.rfind(':') == std::string::npos) a += ":" + std::to_string(DEFAULT_PORT);
                  n.forget_peer(a);
                  return UniValue();
              }));
        s.add("invalidate_block", {"block_hash"}, 1, P([&n](const Params& p) {
                  n.invalidate_block(json::parse_hash(p[0]));
                  return UniValue();
              }));
        s.add("remove_from_mempool", {"txid"}, 1, P([&n](const Params& p) {
                  n.remove_from_mempool(json::parse_hash(p[0]));
                  return UniValue();
              }));
        s.add("stop", {}, 0, [&app](const Params&) {
            if (app.on_stop) app.on_stop();
            return UniValue();
        });
        // ---- wallet
        auto sign_and_send = [&n, &w](const Transaction& tx) {
            w.check_fillable(tx);
            auto at = w.authorize(tx);
            n.broadcast_transaction(at);
            return json::hash(at.transaction.txid());
        };
        auto hex_memo = [](const UniValue& v) {
            Bytes memo;
            if (!v.isNull()) {
                auto m = bt::hex_decode(str_param(v));
                if (!m) throw app_error("Invalid character in hex memo");
                memo = *m;
            }
            return memo;
        };
        s.add("amm_burn", {"asset0", "asset1", "lp_token_amount"}, 3, P([&app, &w, sign_and_send](const Params& p) {
                  AssetId a = json::parse_asset_id(p[0]), b = json::parse_asset_id(p[1]);
                  uint64_t lp = json::parse_u64(p[2]);
                  AssetId a0 = std::min(a, b), a1 = std::max(a, b);
                  auto ps = app.pool(a0, a1);
                  if (!ps) throw app_error("state error: missing AMM pool state for " + a0.to_hex() + "-" + a1.to_hex());
                  std::string err;
                  auto np = ps->burn(lp, err);
                  if (!np) throw app_error(err);
                  return sign_and_send(w.amm_burn(a0, a1, ps->reserve0 - np->reserve0, ps->reserve1 - np->reserve1, lp));
              }));
        s.add("amm_mint", {"asset0", "asset1", "amount0", "amount1"}, 4, P([&app, &w, sign_and_send](const Params& p) {
                  AssetId a0 = json::parse_asset_id(p[0]), a1 = json::parse_asset_id(p[1]);
                  uint64_t x0 = json::parse_u64(p[2]), x1 = json::parse_u64(p[3]);
                  // kept fix: amounts follow the assets into the pool's (sorted) order
                  if (a1 < a0) {
                      std::swap(a0, a1);
                      std::swap(x0, x1);
                  }
                  auto ps = app.pool(a0, a1);
                  PoolState cur = ps ? *ps : PoolState::fresh(Hash32{});
                  std::string err;
                  auto np = cur.mint(x0, x1, err);
                  if (!np) throw app_error(err);
                  return sign_and_send(w.amm_mint(a0, a1, x0, x1, np->outstanding_lp_tokens - cur.outstanding_lp_tokens));
              }));
        s.add("amm_swap", {"asset_spend", "asset_receive", "amount_spend"}, 3, P([&app, &n, &w](const Params& p) {
                  AssetId sp = json::parse_asset_id(p[0]), rc = json::parse_asset_id(p[1]);
                  uint64_t amount = json::parse_u64(p[2]);
                  if (sp == rc) throw app_error("state error: Invalid AMM swap");
                  auto ps = app.pool(sp, rc);
                  if (!ps) throw app_error("state error: missing AMM pool state for " + std::min(sp, rc).to_hex() + "-" + std::max(sp, rc).to_hex());
                  std::string err;
                  uint64_t receive;
                  // kept fix (their B12): the amount received is old - new reserve (theirs subtracts
                  // the other way round, which underflows and wraps)
                  if (sp < rc) {
                      auto np = ps->swap_asset0_for_asset1(amount, err);
                      if (!np) throw app_error(err);
                      receive = ps->reserve1 - np->reserve1;
                  } else {
                      auto np = ps->swap_asset1_for_asset0(amount, err);
                      if (!np) throw app_error(err);
                      receive = ps->reserve0 - np->reserve0;
                  }
                  { auto tx_ = w.amm_swap(sp, rc, amount, receive); w.check_fillable(tx_); n.broadcast_transaction(w.authorize(tx_)); }
                  return UniValue(receive);
              }));
        s.add("bitcoin_balance", {}, 0, [&w](const Params&) {
            auto b = w.balance();
            UniValue o(UniValue::VOBJ);
            o.pushKV("total_sats", UniValue(b.total));
            o.pushKV("available_sats", UniValue(b.available));
            return o;
        });
        s.add("create_deposit", {"address", "value_sats", "fee_sats"}, 3, P([&app](const Params& p) {
                  return json::btc_hash(app.deposit(json::parse_address(p[0]), json::parse_u64(p[1]), json::parse_u64(p[2])));
              }));
        s.add("decrypt_msg", {"encryption_pubkey", "ciphertext"}, 2, P([&w](const Params& p) {
                  auto epk = json::parse_epk(p[0]);
                  auto ct = bt::hex_decode(str_param(p[1]));
                  if (!ct) throw app_error("Invalid character in hex ciphertext");
                  return UniValue(bt::hex_encode(w.decrypt_msg(epk, *ct)));
              }));
        s.add("dutch_auction_bid", {"dutch_auction_id", "bid_size"}, 2, P([&app, &n, &w](const Params& p) {
                  Hash32 id = json::parse_hash(p[0]);
                  uint64_t bid = json::parse_u64(p[1]);
                  auto h = n.height();
                  uint32_t height = h ? *h + 1 : 0; // getblockcount
                  auto st = app.auction(id);
                  if (!st) throw app_error("state error: Auction not found");
                  std::string err;
                  auto ns = st->bid(Hash32{}, bid, height, err);
                  if (!ns) throw app_error(err);
                  uint64_t q = st->base_amount_remaining.latest().data - ns->base_amount_remaining.latest().data;
                  { auto tx_ = w.dutch_auction_bid(id, st->base_asset, st->quote_asset, bid, q); w.check_fillable(tx_); n.broadcast_transaction(w.authorize(tx_)); }
                  return UniValue(q);
              }));
        s.add("dutch_auction_create", {"dutch_auction_params"}, 1, P([&w, sign_and_send](const Params& p) {
                  return sign_and_send(w.dutch_auction_create(json::parse_auction_params(p[0])));
              }));
        s.add("dutch_auction_collect", {"dutch_auction_id"}, 1, P([&app, &n, &w](const Params& p) {
                  Hash32 id = json::parse_hash(p[0]);
                  auto h = n.height();
                  uint64_t height = h ? *h + 1 : 0;
                  auto st = app.auction(id);
                  if (!st) throw app_error("state error: Auction not found");
                  if (height <= uint64_t(st->start_block) + st->duration) throw app_error("state error: Auction has not ended yet");
                  uint64_t base = st->base_amount_remaining.latest().data, quote = st->quote_amount.latest().data;
                  { auto tx_ = w.dutch_auction_collect(id, st->base_asset, st->quote_asset, base, quote); w.check_fillable(tx_); n.broadcast_transaction(w.authorize(tx_)); }
                  UniValue r = arr();
                  r.push_back(UniValue(base));
                  r.push_back(UniValue(quote));
                  return r;
              }));
        s.add("encrypt_msg", {"encryption_pubkey", "msg"}, 2, P([](const Params& p) {
                  auto epk = json::parse_epk(p[0]);
                  std::string m = str_param(p[1]);
                  return UniValue(bt::hex_encode(bt::ecies::encrypt(epk, ByteSpan((const uint8_t*)m.data(), m.size()))));
              }));
        s.add("format_deposit_address", {"address"}, 1,
              P([](const Params& p) { return UniValue(json::parse_address(p[0]).format_for_deposit(THIS_SIDECHAIN)); }));
        s.add("generate_mnemonic", {}, 0, [](const Params&) { return UniValue(bt::hd::generate_mnemonic(12)); });
        s.add("get_block_template", {}, 0, [&app](const Params&) {
            auto t = app.build_block_template(std::nullopt);
            UniValue o(UniValue::VOBJ);
            o.pushKV("critical_hash", json::hash(t.header.hash()));
            o.pushKV("block", json::block(t.header, t.body, t.height));
            o.pushKV("fees_sats", UniValue(t.fees));
            return o;
        });
        s.add("get_new_address", {}, 0, [&w](const Params&) { return json::address(w.get_new_address()); });
        s.add("get_new_encryption_key", {}, 0, [&w](const Params&) { return UniValue(epk_to_string(w.get_new_encryption_key())); });
        s.add("get_new_verifying_key", {}, 0, [&w](const Params&) { return UniValue(vk_to_string(w.get_new_verifying_key())); });
        s.add("get_wallet_addresses", {}, 0, [&w](const Params&) {
            UniValue a = arr();
            for (auto& x : w.addresses()) a.push_back(json::address(x));
            return a;
        });
        auto wallet_utxos = [&w](const Params&) {
            UniValue a = arr();
            for (auto& [op, o] : w.utxos()) a.push_back(json::pointed(op, json::filled_output(o)));
            return a;
        };
        s.add("get_wallet_utxos", {}, 0, wallet_utxos);
        s.add("my_utxos", {}, 0, wallet_utxos);
        s.add("my_unconfirmed_utxos", {}, 0, [&n, &w](const Params&) {
            // mempool outputs paying our addresses (their mempool.get_unconfirmed_utxos), unfilled
            UniValue a = arr();
            auto mine = w.addresses();
            std::set<Address> ms(mine.begin(), mine.end());
            for (auto& at : n.mempool()) {
                Hash32 txid = at.transaction.txid();
                for (uint32_t v = 0; v < at.transaction.outputs.size(); v++)
                    if (ms.count(at.transaction.outputs[v].address))
                        a.push_back(json::pointed(OutPoint{OutPoint::Regular, txid, v}, json::output(at.transaction.outputs[v])));
            }
            return a;
        });
        s.add("mine", {"fee"}, 0, P([&app](const Params& p) {
                  std::optional<uint64_t> fee;
                  if (!p[0].isNull()) fee = json::parse_u64(p[0]);
                  app.mine(fee);
                  return UniValue();
              }));
        s.add("register_bitasset", {"plain_name", "initial_supply", "bitasset_data"}, 2, P([&w, sign_and_send](const Params& p) {
                  return sign_and_send(w.register_bitasset(str_param(p[0]), json::parse_bitasset_data(p[2]), json::parse_u64(p[1])));
              }));
        s.add("reserve_bitasset", {"plain_name"}, 1, P([&w, sign_and_send](const Params& p) { return sign_and_send(w.reserve_bitasset(str_param(p[0]))); }));
        s.add("set_seed_from_mnemonic", {"mnemonic"}, 1, P([&w](const Params& p) {
                  w.set_seed_from_mnemonic(str_param(p[0]));
                  return UniValue();
              }));
        s.add("sign_arbitrary_msg", {"verifying_key", "msg"}, 2, P([&w](const Params& p) {
                  return json::signature(w.sign_arbitrary_msg(json::parse_vk(p[0]), str_param(p[1])));
              }));
        s.add("sign_arbitrary_msg_as_addr", {"address", "msg"}, 2, P([&w](const Params& p) {
                  return json::authorization(w.sign_arbitrary_msg_as_addr(json::parse_address(p[0]), str_param(p[1])));
              }));
        s.add("sign_transaction", {"transaction", "broadcast"}, 1, P([&n, &w](const Params& p) {
                  auto at = w.authorize(json::parse_transaction(p[0]));
                  if (!p[1].isNull() && !p[1].isBool()) throw json::Error("expected a bool");
                  if (p[1].isBool() && p[1].get_bool()) n.broadcast_transaction(at);
                  return json::authorized_transaction(at);
              }));
        s.add("transfer", {"dest", "value", "fee", "memo"}, 3, P([&w, sign_and_send, hex_memo](const Params& p) {
                  return sign_and_send(w.create_transfer(json::parse_address(p[0]), json::parse_u64(p[1]), json::parse_u64(p[2]), hex_memo(p[3])));
              }));
        s.add("transfer_many", {"dests", "fee_sats"}, 2, P([&w, sign_and_send](const Params& p) {
                  if (!p[0].isObject()) throw json::Error("expected a map");
                  std::map<Address, uint64_t> d;
                  for (size_t i = 0; i < p[0].size(); i++) {
                      Address a = json::parse_address(UniValue(p[0].getKeys()[i]));
                      if (d.count(a)) throw json::Error("invalid entry: found duplicate key"); // MapPreventDuplicates
                      d[a] = json::parse_u64(p[0].getValues()[i]);
                  }
                  return sign_and_send(w.create_transfer_many(d, json::parse_u64(p[1])));
              }));
        s.add("transfer_bitasset", {"dest", "asset_id", "amount", "fee_sats", "memo"}, 4, P([&w, sign_and_send, hex_memo](const Params& p) {
                  return sign_and_send(w.create_bitasset_transfer(json::parse_address(p[0]), json::parse_hash(p[1]), json::parse_u64(p[2]),
                                                                  json::parse_u64(p[3]), hex_memo(p[4])));
              }));
        s.add("verify_signature", {"signature", "verifying_key", "dst", "msg"}, 4, P([](const Params& p) {
                  auto sig = json::parse_signature(p[0]);
                  auto vk = json::parse_vk(p[1]);
                  std::string dst = str_param(p[2]), m = str_param(p[3]);
                  uint8_t d;
                  if (dst == "transaction") d = 0;
                  else if (dst == "arbitrary") d = 255;
                  else throw json::Error("unknown variant `" + dst + "`, expected `transaction` or `arbitrary`");
                  return UniValue(verify_with_dst(sig, vk, d, ByteSpan((const uint8_t*)m.data(), m.size())));
              }));
        s.add("withdraw", {"mainchain_address", "amount_sats", "fee_sats", "mainchain_fee_sats"}, 4, P([&w, sign_and_send](const Params& p) {
                  return sign_and_send(w.create_withdrawal(json::parse_btc_address(p[0]), json::parse_u64(p[1]), json::parse_u64(p[3]), json::parse_u64(p[2])));
              }));
    }
}

} // namespace bitassets
