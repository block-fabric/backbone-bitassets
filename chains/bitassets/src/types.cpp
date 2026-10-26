// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Port of the plain-bitassets fork v0.17.1 lib/types (mod.rs, hashes.rs, keys.rs,
// bitasset_data.rs, transaction/{mod,output,outputs,inputs}.rs, authorization.rs).
#include <bitassets/types.h>

#include <bt/bech32.h>
#include <bt/crypto.h>
#include <bt/parse.h>
#include <bt/sidechain/cbmt.h>

#include <algorithm>
#include <cstring>
#include <deque>
#include <tuple>

namespace bitassets {

using bt::DecodeError;
using bt::sidechain::de_opt;
using bt::sidechain::borsh_opt;
using bt::sidechain::de_sig64;
using bt::sidechain::de_v4;
using bt::sidechain::de_v6;
using bt::sidechain::de_vk_raw;
using bt::sidechain::raw_v4;
using bt::sidechain::raw_v6;

// ---------------------------------------------------------------- asset ids (hashes.rs)
Bytes AssetId::borsh_bytes() const
{
    Writer w;
    borsh(w, *this);
    return w.buf;
}

std::string AssetId::to_hex() const { return bt::hex_encode(borsh_bytes()); }

std::optional<AssetId> AssetId::from_borsh(ByteSpan b)
{
    try {
        Reader r(b);
        AssetId a = borsh_de_asset_id(r);
        if (!r.empty()) return std::nullopt; // borsh::from_slice: "Not all bytes read"
        return a;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void borsh(Writer& w, const AssetId& a)
{
    w.u8(a.kind);
    if (a.kind != AssetId::Bitcoin) w.raw(a.id);
}

AssetId borsh_de_asset_id(Reader& r)
{
    AssetId a;
    uint8_t tag = r.u8();
    if (tag > 2) throw DecodeError("invalid AssetId tag");
    a.kind = AssetId::Kind(tag);
    if (a.kind != AssetId::Bitcoin) a.id = r.array<32>();
    return a;
}

// serde: the borsh bytes as a Vec<u8> (u64 length + bytes) in bincode
void bincode(Writer& w, const AssetId& a) { w.bincode_bytes(a.borsh_bytes()); }

AssetId de_asset_id(Reader& r)
{
    Bytes b = r.bincode_bytes();
    auto a = AssetId::from_borsh(b);
    if (!a) throw DecodeError("invalid AssetId");
    return *a;
}

// ---------------------------------------------------------------- keys (keys.rs)
std::string vk_to_string(const bt::frost::Point& vk) { return bt::sidechain::vk_to_bech32("ba-svk", vk); }
std::optional<bt::frost::Point> vk_from_string(std::string_view s) { return bt::sidechain::vk_from_bech32(s, "ba-svk"); }
std::string epk_to_string(const bt::ByteArray<32>& k) { return bt::sidechain::epk_to_bech32("ba-enc", k); }
std::optional<bt::ByteArray<32>> epk_from_string(std::string_view s) { return bt::sidechain::epk_from_bech32(s, "ba-enc"); }

Hash32 name_hash(std::string_view plain) { return bt::blake3(ByteSpan((const uint8_t*)plain.data(), plain.size())); }

// ---------------------------------------------------------------- contents (output.rs)
uint64_t Content::bitcoin_value() const
{
    switch (kind) {
    case Bitcoin: return value;
    case Withdrawal: {
        uint64_t s = value + main_fee;
        return s < value ? UINT64_MAX : s; // checked_add.unwrap_or(Amount::MAX)
    }
    default: return 0;
    }
}

bool Content::operator==(const Content& o) const
{
    if (kind != o.kind) return false;
    switch (kind) {
    case AmmLpToken:
    case Bitcoin:
    case BitAsset: return value == o.value;
    case BitAssetControl:
    case BitAssetReservation:
    case DutchAuctionReceipt: return true;
    case Withdrawal: return value == o.value && main_fee == o.main_fee && main_address == o.main_address;
    }
    return false;
}

Content FilledContent::to_content() const
{
    Content c;
    switch (kind) {
    case AmmLpToken: c.kind = Content::AmmLpToken; c.value = value; break;
    case Bitcoin: c.kind = Content::Bitcoin; c.value = value; break;
    case BitcoinWithdrawal:
        c.kind = Content::Withdrawal;
        c.value = value;
        c.main_fee = main_fee;
        c.main_address = main_address;
        break;
    case BitAsset: c.kind = Content::BitAsset; c.value = value; break;
    case BitAssetControl: c.kind = Content::BitAssetControl; break;
    case BitAssetReservation: c.kind = Content::BitAssetReservation; break;
    case DutchAuctionReceipt: c.kind = Content::DutchAuctionReceipt; break;
    }
    return c;
}

std::optional<std::pair<AssetId, uint64_t>> FilledContent::asset_value() const
{
    switch (kind) {
    case BitAsset: return std::make_pair(AssetId::bitasset(id), value);
    case BitAssetControl: return std::make_pair(AssetId::control(id), uint64_t(1));
    case Bitcoin: return std::make_pair(AssetId::bitcoin(), value);
    default: return std::nullopt;
    }
}

bool FilledContent::operator==(const FilledContent& o) const
{
    if (kind != o.kind) return false;
    switch (kind) {
    case AmmLpToken: return asset0 == o.asset0 && asset1 == o.asset1 && value == o.value;
    case Bitcoin: return value == o.value;
    case BitcoinWithdrawal: return value == o.value && main_fee == o.main_fee && main_address == o.main_address;
    case BitAsset: return id == o.id && value == o.value;
    case BitAssetControl:
    case DutchAuctionReceipt: return id == o.id;
    case BitAssetReservation: return id == o.id && commitment == o.commitment;
    }
    return false;
}

// ---------------------------------------------------------------- transactions
std::optional<Hash32> Transaction::implied_reservation_commitment() const
{
    if (!is(TxData::BitAssetRegistration)) return std::nullopt;
    return bt::sidechain::keyed_hash(data->revealed_nonce, data->name_hash.span());
}

Hash32 Transaction::txid() const { return bt::blake3(canonical_bytes()); }
Bytes Transaction::canonical_bytes() const { return borsh_bytes(*this); }

std::optional<Hash32> Transaction::merkle_root() const
{
    auto outs = outputs_merkle_root(outputs);
    if (!outs) return std::nullopt;
    Hash32 ins = inputs_merkle_root(inputs);
    Writer w;
    w.raw(ins);
    w.raw(*outs);
    return bt::blake3(w.buf);
}

Hash32 Header::hash() const { return bitassets::hash(*this); }
Hash32 Header::coinbase_txid() const { return compute_coinbase_txid(merkle_root, prev_main_hash, prev_side_hash); }

Hash32 compute_coinbase_txid(const Hash32& merkle_root, const Hash32& prev_main_hash, const std::optional<Hash32>& prev_side)
{
    Writer w;
    w.raw(merkle_root);
    w.raw(prev_main_hash);
    if (prev_side) {
        w.u8(1);
        w.raw(*prev_side);
    } else {
        w.u8(0);
    }
    return bt::blake3(w.buf);
}

size_t Body::inputs_len() const
{
    size_t n = 0;
    for (auto& t : transactions) n += t.inputs.size();
    return n;
}

std::vector<AuthorizedTransaction> Body::authorized_transactions() const
{
    std::vector<AuthorizedTransaction> r;
    size_t k = 0;
    for (auto& t : transactions) {
        AuthorizedTransaction at;
        at.transaction = t;
        for (size_t i = 0; i < t.inputs.size() && k < authorizations.size(); i++) at.authorizations.push_back(authorizations[k++]);
        r.push_back(std::move(at));
    }
    return r;
}

Body Body::from_authorized(std::vector<AuthorizedTransaction> txs, Coinbase cb)
{
    Body b;
    b.coinbase = std::move(cb);
    for (auto& t : txs) {
        for (auto& a : t.authorizations) b.authorizations.push_back(a);
        b.transactions.push_back(std::move(t.transaction));
    }
    return b;
}

// ---------------------------------------------------------------- filled transactions
// transaction/mod.rs:1024-1063 bitcoin_fee: checked sums, Underfunded when value out > in.
std::optional<uint64_t> FilledTransaction::bitcoin_fee() const
{
    uint64_t in = 0, out = 0;
    for (auto& o : spent_utxos) {
        uint64_t v = o.bitcoin_value();
        if (in + v < in) return std::nullopt;
        in += v;
    }
    for (auto& o : tx->outputs) {
        uint64_t v = o.bitcoin_value();
        if (out + v < out) return std::nullopt;
        out += v;
    }
    if (in < out) return std::nullopt;
    return in - out;
}

// unique_spent_assets (:1118-1133): spent Bitcoin / BitAsset / control outputs, in the order the
// assets first occur; values combined with `+=` on u64 (no overflow checks in release: wraps).
std::vector<std::pair<AssetId, uint64_t>> FilledTransaction::unique_spent_assets() const
{
    std::vector<std::pair<AssetId, uint64_t>> r;
    for (auto& u : spent_utxos) {
        if (!(u.content.is_bitcoin() || u.content.is_bitasset() || u.content.is_control())) continue;
        auto av = u.content.asset_value();
        if (!av) continue;
        auto it = std::find_if(r.begin(), r.end(), [&](auto& p) { return p.first == av->first; });
        if (it == r.end()) r.emplace_back(av->first, av->second);
        else it->second += av->second;
    }
    return r;
}

std::vector<std::pair<Hash32, uint64_t>> FilledTransaction::unique_spent_bitassets() const
{
    std::vector<std::pair<Hash32, uint64_t>> r;
    for (auto& u : spent_utxos) {
        if (!u.content.is_bitasset()) continue;
        auto it = std::find_if(r.begin(), r.end(), [&](auto& p) { return p.first == u.content.id; });
        if (it == r.end()) r.emplace_back(u.content.id, u.content.value);
        else it->second += u.content.value;
    }
    return r;
}

std::vector<std::tuple<AssetId, AssetId, uint64_t>> FilledTransaction::unique_spent_lp_tokens() const
{
    std::vector<std::tuple<AssetId, AssetId, uint64_t>> r;
    for (auto& u : spent_utxos) {
        if (!u.content.is_lp_token()) continue;
        auto it = std::find_if(r.begin(), r.end(), [&](auto& p) { return std::get<0>(p) == u.content.asset0 && std::get<1>(p) == u.content.asset1; });
        if (it == r.end()) r.emplace_back(u.content.asset0, u.content.asset1, u.content.value);
        else std::get<2>(*it) += u.content.value;
    }
    return r;
}

std::optional<AmmBurnOp> FilledTransaction::amm_burn() const
{
    if (!tx->is(TxData::AmmBurn)) return std::nullopt;
    auto lp = unique_spent_lp_tokens();
    if (lp.empty()) return std::nullopt;
    auto& [a0, a1, _] = lp.front();
    return AmmBurnOp{a0, a1, tx->data->amount0, tx->data->amount1, tx->data->lp_tokens};
}

std::optional<AmmMintOp> FilledTransaction::amm_mint() const
{
    if (!tx->is(TxData::AmmMint)) return std::nullopt;
    auto u = unique_spent_assets();
    if (u.size() < 2) return std::nullopt;
    AssetId a = u[0].first, b = u[1].first;
    if (b < a) std::swap(a, b);
    return AmmMintOp{a, b, tx->data->amount0, tx->data->amount1, tx->data->lp_tokens};
}

std::optional<AmmSwapOp> FilledTransaction::amm_swap() const
{
    if (!tx->is(TxData::AmmSwap)) return std::nullopt;
    auto u = unique_spent_assets();
    if (u.empty()) return std::nullopt;
    return AmmSwapOp{u.front().first, tx->data->pair_asset, tx->data->amount_spent, tx->data->amount_receive};
}

// bitasset_mint (:921-931): the last spent control coin's BitAsset
std::optional<std::pair<Hash32, uint64_t>> FilledTransaction::bitasset_mint() const
{
    if (!tx->is(TxData::BitAssetMint)) return std::nullopt;
    for (auto it = spent_utxos.rbegin(); it != spent_utxos.rend(); ++it)
        if (it->content.is_control()) return std::make_pair(it->content.id, tx->data->mint_amount);
    return std::nullopt;
}

std::optional<AuctionBidOp> FilledTransaction::dutch_auction_bid() const
{
    if (!tx->is(TxData::DutchAuctionBid)) return std::nullopt;
    auto u = unique_spent_assets();
    if (u.empty()) return std::nullopt;
    return AuctionBidOp{tx->data->auction_id, u.front().first, tx->data->receive_asset, tx->data->bid_size, tx->data->quantity};
}

std::optional<DutchAuctionParams> FilledTransaction::dutch_auction_create() const
{
    if (!tx->is(TxData::DutchAuctionCreate)) return std::nullopt;
    return tx->data->params;
}

std::optional<AuctionCollectOp> FilledTransaction::dutch_auction_collect() const
{
    if (!tx->is(TxData::DutchAuctionCollect)) return std::nullopt;
    for (auto& u : spent_utxos)
        if (u.content.is_receipt())
            return AuctionCollectOp{u.content.id, tx->data->asset_offered, tx->data->asset_receive, tx->data->amount_offered_remaining, tx->data->amount_received};
    return std::nullopt;
}

namespace {
using Leg = std::optional<std::pair<AssetId, uint64_t>>;
std::optional<uint64_t> checked_add(uint64_t a, uint64_t b)
{
    uint64_t s = a + b;
    if (s < a) return std::nullopt;
    return s;
}
std::optional<uint64_t> checked_sub(uint64_t a, uint64_t b)
{
    if (b > a) return std::nullopt;
    return a - b;
}
} // namespace

// output_asset_total_values (:1188-1401), ported literally (consensus: do not change; known
// upstream consensus behaviour reproduced, details held for coordinated disclosure).
std::vector<std::pair<AssetId, std::optional<uint64_t>>> FilledTransaction::output_asset_total_values() const
{
    std::optional<std::pair<Hash32, uint64_t>> new_bitasset;
    if (tx->is(TxData::BitAssetRegistration) && tx->data->initial_supply != 0) new_bitasset = std::make_pair(tx->data->name_hash, tx->data->initial_supply);
    auto mint = bitasset_mint();
    Leg burn0, burn1, mint0, mint1, swap_spend, swap_receive, bid_spend, bid_receive, create_spend, collect0, collect1;
    if (auto b = amm_burn()) {
        burn0 = std::make_pair(b->asset0, b->amount0);
        burn1 = std::make_pair(b->asset1, b->amount1);
    }
    if (auto m = amm_mint()) {
        mint0 = std::make_pair(m->asset0, m->amount0);
        mint1 = std::make_pair(m->asset1, m->amount1);
    }
    if (auto s = amm_swap()) {
        swap_spend = std::make_pair(s->asset_spend, s->amount_spend);
        swap_receive = std::make_pair(s->asset_receive, s->amount_receive);
    }
    if (auto b = dutch_auction_bid()) {
        bid_spend = std::make_pair(b->asset_spend, b->amount_spend);
        bid_receive = std::make_pair(b->asset_receive, b->amount_receive);
    }
    if (auto c = dutch_auction_create()) create_spend = std::make_pair(c->base_asset, c->base_amount);
    if (auto c = dutch_auction_collect()) {
        collect0 = std::make_pair(c->asset_offered, c->amount_offered_remaining);
        collect1 = std::make_pair(c->asset_receive, c->amount_received);
    }
    // the closure's own copies
    Leg c_burn0 = burn0, c_burn1 = burn1, c_mint0 = mint0, c_mint1 = mint1, c_swap_spend = swap_spend, c_swap_receive = swap_receive,
        c_bid_spend = bid_spend, c_bid_receive = bid_receive, c_create_spend = create_spend, c_collect0 = collect0, c_collect1 = collect1;
    std::vector<std::pair<AssetId, std::optional<uint64_t>>> r;
    for (auto& [asset, total] : unique_spent_assets()) {
        std::optional<uint64_t> v;
        auto take = [&](Leg& leg) {
            if (leg && leg->first == asset) {
                return true;
            }
            return false;
        };
        if (mint && AssetId::bitasset(mint->first) == asset) v = checked_add(total, mint->second);
        else if (take(c_burn0)) { v = checked_add(total, c_burn0->second); c_burn0.reset(); }
        else if (take(c_burn1)) { v = checked_add(total, c_burn1->second); c_burn1.reset(); }
        else if (take(c_mint0)) { v = checked_sub(total, c_mint0->second); c_mint0.reset(); }
        else if (take(c_mint1)) { v = checked_sub(total, c_mint1->second); c_mint1.reset(); }
        else if (take(c_swap_spend)) { v = checked_sub(total, c_swap_spend->second); c_swap_spend.reset(); }
        else if (take(c_swap_receive)) { v = checked_add(total, c_swap_receive->second); c_swap_receive.reset(); }
        else if (take(c_bid_receive)) { v = checked_add(total, c_bid_receive->second); c_bid_receive.reset(); }
        else if (take(c_bid_spend)) { v = checked_sub(total, c_bid_spend->second); c_bid_spend.reset(); }
        else if (take(c_create_spend)) { v = checked_sub(total, c_create_spend->second); c_create_spend.reset(); }
        else if (take(c_collect0)) { v = checked_add(total, c_collect0->second); c_collect0.reset(); }
        else if (take(c_collect1)) { v = checked_add(total, c_collect1->second); c_collect1.reset(); }
        else v = total;
        if (v == std::optional<uint64_t>(0)) continue; // .filter(|(_, amount)| *amount != Some(0))
        r.emplace_back(asset, v);
    }
    auto some = [&](const Leg& l) {
        if (l) r.emplace_back(l->first, std::optional<uint64_t>(l->second));
    };
    auto none = [&](const Leg& l) {
        if (l) r.emplace_back(l->first, std::nullopt);
    };
    some(burn0);
    some(burn1);
    none(mint0);
    none(mint1);
    none(swap_spend);
    some(swap_receive);
    some(bid_receive);
    none(bid_spend);
    none(create_spend);
    some(collect0);
    some(collect1);
    if (new_bitasset) {
        r.emplace_back(AssetId::bitasset(new_bitasset->first), std::optional<uint64_t>(new_bitasset->second));
        r.emplace_back(AssetId::control(new_bitasset->first), std::optional<uint64_t>(1));
    }
    return r;
}

// filled_outputs (:1578-1666)
std::optional<std::vector<FilledOutput>> FilledTransaction::filled_outputs() const
{
    auto totals = output_asset_total_values();
    // output_bitcoin_max_value: the first Bitcoin entry, else 0
    std::optional<uint64_t> btc_max = 0;
    for (auto& [a, v] : totals)
        if (a.kind == AssetId::Bitcoin) {
            btc_max = v;
            break;
        }
    if (!btc_max) return std::nullopt;
    uint64_t bitcoin_left = *btc_max;
    // output_bitasset_total_values
    std::deque<std::pair<Hash32, std::optional<uint64_t>>> bitassets;
    for (auto& [a, v] : totals)
        if (a.kind == AssetId::BitAsset) bitassets.emplace_back(a.id, v);
    // output_lp_token_total_amounts (:1436-1489), ported literally
    std::deque<std::tuple<AssetId, AssetId, std::optional<uint64_t>>> lps;
    {
        auto burn = amm_burn();
        auto mint = amm_mint();
        auto c_burn = burn;
        auto c_mint = mint;
        for (auto& [a0, a1, total] : unique_spent_lp_tokens()) {
            std::optional<uint64_t> v;
            if (c_burn && c_burn->asset0 == a0 && c_burn->asset1 == a1) {
                v = checked_sub(total, c_burn->lp_token_burn);
                c_burn.reset();
            } else if (c_mint && c_mint->asset0 == a0 && c_mint->asset1 == a1) {
                v = checked_add(total, c_mint->lp_token_mint);
                c_mint.reset();
            } else {
                v = total;
            }
            lps.emplace_back(a0, a1, v);
        }
        if (burn) lps.emplace_back(burn->asset0, burn->asset1, std::nullopt);
        if (mint) lps.emplace_back(mint->asset0, mint->asset1, std::optional<uint64_t>(mint->lp_token_mint));
    }
    // filled_bitasset_control_output_content
    std::deque<Hash32> controls;
    for (auto& [a, v] : totals)
        if (a.kind == AssetId::BitAssetControl) controls.push_back(a.id);
    // filled_dutch_auction_receipts: spent receipts (minus the first one a collect burns), then
    // the new one of a create
    std::deque<FilledContent> receipts;
    {
        bool skip = tx->is(TxData::DutchAuctionCollect);
        for (auto& u : spent_utxos) {
            if (!u.content.is_receipt()) continue;
            if (skip) {
                skip = false;
                continue;
            }
            receipts.push_back(u.content);
        }
        if (tx->is(TxData::DutchAuctionCreate)) {
            FilledContent c;
            c.kind = FilledContent::DutchAuctionReceipt;
            c.id = tx->txid();
            receipts.push_back(c);
        }
    }
    // filled_reservation_output_content: spent reservations minus the first one a registration
    // burns, then the new one of a reservation
    std::deque<FilledContent> res;
    {
        auto burn = tx->implied_reservation_commitment();
        for (auto& u : spent_utxos) {
            if (!u.content.is_reservation()) continue;
            if (burn && u.content.commitment == *burn) {
                burn.reset();
                continue;
            }
            res.push_back(u.content);
        }
        if (tx->is(TxData::BitAssetReservation)) {
            FilledContent c;
            c.kind = FilledContent::BitAssetReservation;
            c.id = tx->txid();
            c.commitment = tx->data->commitment;
            res.push_back(c);
        }
    }
    std::vector<FilledOutput> out;
    out.reserve(tx->outputs.size());
    for (auto& o : tx->outputs) {
        FilledOutput f;
        f.address = o.address;
        f.memo = o.memo;
        switch (o.content.kind) {
        case Content::AmmLpToken: {
            if (lps.empty()) return std::nullopt;
            auto& [a0, a1, rem] = lps.front();
            if (!rem) return std::nullopt;
            f.content.kind = FilledContent::AmmLpToken;
            f.content.asset0 = a0;
            f.content.asset1 = a1;
            f.content.value = o.content.value;
            if (o.content.value > *rem) return std::nullopt;
            if (o.content.value == *rem) lps.pop_front();
            else *rem -= o.content.value;
            break;
        }
        case Content::BitAsset: {
            if (bitassets.empty()) return std::nullopt;
            auto& [id, rem] = bitassets.front();
            if (!rem) return std::nullopt;
            f.content.kind = FilledContent::BitAsset;
            f.content.id = id;
            f.content.value = o.content.value;
            if (o.content.value > *rem) return std::nullopt;
            if (o.content.value == *rem) bitassets.pop_front();
            else *rem -= o.content.value;
            break;
        }
        case Content::BitAssetControl:
            if (controls.empty()) return std::nullopt;
            f.content.kind = FilledContent::BitAssetControl;
            f.content.id = controls.front();
            controls.pop_front();
            break;
        case Content::BitAssetReservation:
            if (res.empty()) return std::nullopt;
            f.content = res.front();
            res.pop_front();
            break;
        case Content::DutchAuctionReceipt:
            if (receipts.empty()) return std::nullopt;
            f.content = receipts.front();
            receipts.pop_front();
            break;
        case Content::Bitcoin:
            if (o.content.value > bitcoin_left) return std::nullopt;
            bitcoin_left -= o.content.value;
            f.content.kind = FilledContent::Bitcoin;
            f.content.value = o.content.value;
            break;
        case Content::Withdrawal:
            f.content.kind = FilledContent::BitcoinWithdrawal;
            f.content.value = o.content.value;
            f.content.main_fee = o.content.main_fee;
            f.content.main_address = o.content.main_address;
            break;
        }
        out.push_back(std::move(f));
    }
    return out;
}

// ---------------------------------------------------------------- borsh
void borsh(Writer& w, const Content& c)
{
    w.u8(c.kind);
    switch (c.kind) {
    case Content::AmmLpToken:
    case Content::Bitcoin:
    case Content::BitAsset: w.u64(c.value); break;
    case Content::BitAssetControl:
    case Content::BitAssetReservation:
    case Content::DutchAuctionReceipt: break;
    case Content::Withdrawal:
        w.u64(c.value);
        w.u64(c.main_fee);
        w.borsh_bytes(c.main_address.script_pubkey());
        break;
    }
}

void borsh(Writer& w, const Output& o)
{
    w.raw(o.address.b);
    borsh(w, o.content);
    w.borsh_bytes(o.memo);
}

void borsh(Writer& w, const BitAssetData& d)
{
    borsh_opt(w, d.commitment, [&](const Hash32& h) { w.raw(h); });
    borsh_opt(w, d.socket_addr_v4, [&](const SocketV4& s) { raw_v4(w, s); });
    borsh_opt(w, d.socket_addr_v6, [&](const SocketV6& s) { raw_v6(w, s); });
    borsh_opt(w, d.encryption_pubkey, [&](const bt::ByteArray<32>& k) { w.raw(k); });
    borsh_opt(w, d.signing_pubkey, [&](const bt::frost::Point& k) { w.raw(k); });
}

template <class T, class F>
static void borsh_update(Writer& w, const Update<T>& u, F f)
{
    w.u8(u.kind);
    if (u.kind == Update<T>::Set) f(u.value);
}

void borsh(Writer& w, const BitAssetDataUpdates& u)
{
    borsh_update(w, u.commitment, [&](const Hash32& h) { w.raw(h); });
    borsh_update(w, u.socket_addr_v4, [&](const SocketV4& s) { raw_v4(w, s); });
    borsh_update(w, u.socket_addr_v6, [&](const SocketV6& s) { raw_v6(w, s); });
    borsh_update(w, u.encryption_pubkey, [&](const bt::ByteArray<32>& k) { w.raw(k); });
    borsh_update(w, u.signing_pubkey, [&](const bt::frost::Point& k) { w.raw(k); });
}

void borsh(Writer& w, const DutchAuctionParams& p)
{
    w.u32(p.start_block);
    w.u32(p.duration);
    borsh(w, p.base_asset);
    w.u64(p.base_amount);
    borsh(w, p.quote_asset);
    w.u64(p.initial_price);
    w.u64(p.final_price);
}

void borsh(Writer& w, const TxData& d)
{
    w.u8(d.kind);
    switch (d.kind) {
    case TxData::AmmBurn:
    case TxData::AmmMint:
        w.u64(d.amount0);
        w.u64(d.amount1);
        w.u64(d.lp_tokens);
        break;
    case TxData::AmmSwap:
        w.u64(d.amount_spent);
        w.u64(d.amount_receive);
        borsh(w, d.pair_asset);
        break;
    case TxData::BitAssetReservation: w.raw(d.commitment); break;
    case TxData::BitAssetRegistration:
        w.raw(d.name_hash);
        w.raw(d.revealed_nonce);
        borsh(w, d.bitasset_data);
        w.u64(d.initial_supply);
        break;
    case TxData::BitAssetMint: w.u64(d.mint_amount); break;
    case TxData::BitAssetUpdate: borsh(w, d.updates); break;
    case TxData::DutchAuctionCreate: borsh(w, d.params); break;
    case TxData::DutchAuctionBid:
        w.raw(d.auction_id);
        borsh(w, d.receive_asset);
        w.u64(d.quantity);
        w.u64(d.bid_size);
        break;
    case TxData::DutchAuctionCollect:
        borsh(w, d.asset_offered);
        borsh(w, d.asset_receive);
        w.u64(d.amount_offered_remaining);
        w.u64(d.amount_received);
        break;
    }
}

void borsh(Writer& w, const Transaction& t)
{
    w.borsh_len(t.inputs.size());
    for (auto& op : t.inputs) bt::sidechain::borsh(w, op);
    w.borsh_len(t.outputs.size());
    for (auto& o : t.outputs) borsh(w, o);
    w.borsh_bytes(t.memo);
    if (t.data) {
        w.u8(1);
        borsh(w, *t.data);
    } else {
        w.u8(0);
    }
}

void borsh(Writer& w, const Authorization& a)
{
    w.raw(a.verifying_key);
    w.raw(a.signature.R);
    w.raw(a.signature.z);
}

void borsh(Writer& w, const Coinbase& c)
{
    w.borsh_bytes(c.memo);
    w.borsh_len(c.outputs.size());
    for (auto& o : c.outputs) borsh(w, o);
}

void borsh(Writer& w, const Body& b)
{
    borsh(w, b.coinbase);
    w.borsh_len(b.transactions.size());
    for (auto& t : b.transactions) borsh(w, t);
    w.borsh_len(b.authorizations.size());
    for (auto& a : b.authorizations) borsh(w, a);
}

void borsh(Writer& w, const Header& h)
{
    w.raw(h.merkle_root);
    if (h.prev_side_hash) {
        w.u8(1);
        w.raw(*h.prev_side_hash);
    } else {
        w.u8(0);
    }
    w.raw(h.prev_main_hash);
}

// ---------------------------------------------------------------- borsh decode
template <class T, class F>
static Update<T> de_update(uint32_t tag, F f)
{
    Update<T> u;
    if (tag > 2) throw DecodeError("invalid Update tag");
    u.kind = typename Update<T>::Kind(tag);
    if (u.kind == Update<T>::Set) u.value = f();
    return u;
}

static BitAssetData borsh_de_bitasset_data(Reader& r)
{
    BitAssetData d;
    d.commitment = de_opt<Hash32>(r, [&] { return r.array<32>(); });
    d.socket_addr_v4 = de_opt<SocketV4>(r, [&] { return de_v4(r); });
    d.socket_addr_v6 = de_opt<SocketV6>(r, [&] { return de_v6(r); });
    d.encryption_pubkey = de_opt<bt::ByteArray<32>>(r, [&] { return r.array<32>(); });
    d.signing_pubkey = de_opt<bt::frost::Point>(r, [&] { return de_vk_raw(r); });
    return d;
}

static BitAssetDataUpdates borsh_de_updates(Reader& r)
{
    BitAssetDataUpdates u;
    u.commitment = de_update<Hash32>(r.u8(), [&] { return r.array<32>(); });
    u.socket_addr_v4 = de_update<SocketV4>(r.u8(), [&] { return de_v4(r); });
    u.socket_addr_v6 = de_update<SocketV6>(r.u8(), [&] { return de_v6(r); });
    u.encryption_pubkey = de_update<bt::ByteArray<32>>(r.u8(), [&] { return r.array<32>(); });
    u.signing_pubkey = de_update<bt::frost::Point>(r.u8(), [&] { return de_vk_raw(r); });
    return u;
}

static DutchAuctionParams borsh_de_params(Reader& r)
{
    DutchAuctionParams p;
    p.start_block = r.u32();
    p.duration = r.u32();
    p.base_asset = borsh_de_asset_id(r);
    p.base_amount = r.u64();
    p.quote_asset = borsh_de_asset_id(r);
    p.initial_price = r.u64();
    p.final_price = r.u64();
    return p;
}

Transaction borsh_de_transaction(Reader& r)
{
    Transaction t;
    size_t n = r.borsh_len(37);
    for (size_t i = 0; i < n; i++) t.inputs.push_back(bt::sidechain::borsh_de_outpoint(r));
    size_t m = r.borsh_len(25);
    for (size_t i = 0; i < m; i++) {
        Output o;
        o.address.b = r.array<20>();
        uint8_t tag = r.u8();
        if (tag > 6) throw DecodeError("invalid Content tag");
        o.content.kind = Content::Kind(tag);
        switch (o.content.kind) {
        case Content::AmmLpToken:
        case Content::Bitcoin:
        case Content::BitAsset: o.content.value = r.u64(); break;
        case Content::Withdrawal:
            o.content.value = r.u64();
            o.content.main_fee = r.u64();
            o.content.main_address = bt::sidechain::address_from_spk(r.borsh_bytes());
            break;
        default: break;
        }
        o.memo = r.borsh_bytes();
        t.outputs.push_back(std::move(o));
    }
    t.memo = r.borsh_bytes();
    uint8_t has = r.u8();
    if (has == 1) {
        TxData d;
        uint8_t tag = r.u8();
        if (tag > 9) throw DecodeError("invalid TxData tag");
        d.kind = TxData::Kind(tag);
        switch (d.kind) {
        case TxData::AmmBurn:
        case TxData::AmmMint:
            d.amount0 = r.u64();
            d.amount1 = r.u64();
            d.lp_tokens = r.u64();
            break;
        case TxData::AmmSwap:
            d.amount_spent = r.u64();
            d.amount_receive = r.u64();
            d.pair_asset = borsh_de_asset_id(r);
            break;
        case TxData::BitAssetReservation: d.commitment = r.array<32>(); break;
        case TxData::BitAssetRegistration:
            d.name_hash = r.array<32>();
            d.revealed_nonce = r.array<32>();
            d.bitasset_data = borsh_de_bitasset_data(r);
            d.initial_supply = r.u64();
            break;
        case TxData::BitAssetMint: d.mint_amount = r.u64(); break;
        case TxData::BitAssetUpdate: d.updates = borsh_de_updates(r); break;
        case TxData::DutchAuctionCreate: d.params = borsh_de_params(r); break;
        case TxData::DutchAuctionBid:
            d.auction_id = r.array<32>();
            d.receive_asset = borsh_de_asset_id(r);
            d.quantity = r.u64();
            d.bid_size = r.u64();
            break;
        case TxData::DutchAuctionCollect:
            d.asset_offered = borsh_de_asset_id(r);
            d.asset_receive = borsh_de_asset_id(r);
            d.amount_offered_remaining = r.u64();
            d.amount_received = r.u64();
            break;
        }
        t.data = std::move(d);
    } else if (has != 0) {
        throw DecodeError("invalid Option tag");
    }
    return t;
}

// ---------------------------------------------------------------- bincode
void bincode(Writer& w, const Content& c)
{
    w.u32(c.kind); // DefaultRepr enum
    switch (c.kind) {
    case Content::AmmLpToken:
    case Content::Bitcoin: // BitcoinContent(Amount): serde u64 sats
    case Content::BitAsset: w.u64(c.value); break;
    case Content::Withdrawal: bt::sidechain::bincode_withdrawal(w, c.value, c.main_fee, c.main_address); break;
    default: break;
    }
}

void bincode(Writer& w, const Output& o)
{
    w.raw(o.address.b);
    bincode(w, o.content);
    w.bincode_bytes(o.memo);
}

void bincode(Writer& w, const BitAssetData& d)
{
    // serde: [u8; 32] tuple, SocketAddr (ip octets, port), x25519 MontgomeryPoint tuple,
    // frost VerifyingKey (serdect array) - all raw bytes in bincode
    borsh_opt(w, d.commitment, [&](const Hash32& h) { w.raw(h); });
    borsh_opt(w, d.socket_addr_v4, [&](const SocketV4& s) { raw_v4(w, s); });
    borsh_opt(w, d.socket_addr_v6, [&](const SocketV6& s) { raw_v6(w, s); });
    borsh_opt(w, d.encryption_pubkey, [&](const bt::ByteArray<32>& k) { w.raw(k); });
    borsh_opt(w, d.signing_pubkey, [&](const bt::frost::Point& k) { w.raw(k); });
}

template <class T, class F>
static void bincode_update(Writer& w, const Update<T>& u, F f)
{
    w.u32(u.kind);
    if (u.kind == Update<T>::Set) f(u.value);
}

void bincode(Writer& w, const BitAssetDataUpdates& u)
{
    bincode_update(w, u.commitment, [&](const Hash32& h) { w.raw(h); });
    bincode_update(w, u.socket_addr_v4, [&](const SocketV4& s) { raw_v4(w, s); });
    bincode_update(w, u.socket_addr_v6, [&](const SocketV6& s) { raw_v6(w, s); });
    bincode_update(w, u.encryption_pubkey, [&](const bt::ByteArray<32>& k) { w.raw(k); });
    bincode_update(w, u.signing_pubkey, [&](const bt::frost::Point& k) { w.raw(k); });
}

void bincode(Writer& w, const DutchAuctionParams& p)
{
    w.u32(p.start_block);
    w.u32(p.duration);
    bincode(w, p.base_asset);
    w.u64(p.base_amount);
    bincode(w, p.quote_asset);
    w.u64(p.initial_price);
    w.u64(p.final_price);
}

void bincode(Writer& w, const TxData& d)
{
    w.u32(d.kind);
    switch (d.kind) {
    case TxData::AmmBurn:
    case TxData::AmmMint:
        w.u64(d.amount0);
        w.u64(d.amount1);
        w.u64(d.lp_tokens);
        break;
    case TxData::AmmSwap:
        w.u64(d.amount_spent);
        w.u64(d.amount_receive);
        bincode(w, d.pair_asset);
        break;
    case TxData::BitAssetReservation: w.raw(d.commitment); break;
    case TxData::BitAssetRegistration:
        w.raw(d.name_hash);
        w.raw(d.revealed_nonce);
        bincode(w, d.bitasset_data);
        w.u64(d.initial_supply);
        break;
    case TxData::BitAssetMint: w.u64(d.mint_amount); break;
    case TxData::BitAssetUpdate: bincode(w, d.updates); break;
    case TxData::DutchAuctionCreate: bincode(w, d.params); break;
    case TxData::DutchAuctionBid:
        w.raw(d.auction_id);
        bincode(w, d.receive_asset);
        w.u64(d.quantity);
        w.u64(d.bid_size);
        break;
    case TxData::DutchAuctionCollect:
        bincode(w, d.asset_offered);
        bincode(w, d.asset_receive);
        w.u64(d.amount_offered_remaining);
        w.u64(d.amount_received);
        break;
    }
}

void bincode(Writer& w, const Transaction& t)
{
    w.bincode_len(t.inputs.size());
    for (auto& op : t.inputs) bt::sidechain::bincode(w, op);
    w.bincode_len(t.outputs.size());
    for (auto& o : t.outputs) bincode(w, o);
    w.bincode_bytes(t.memo);
    if (t.data) {
        w.u8(1);
        bincode(w, *t.data);
    } else {
        w.u8(0);
    }
}

void bincode(Writer& w, const Authorization& a)
{
    w.raw(a.verifying_key); // serdect::array -> tuple, 32 bytes
    w.bincode_len(64);      // frost Signature serde: serdect::slice -> bytes with u64 length
    w.raw(a.signature.R);
    w.raw(a.signature.z);
}

void bincode(Writer& w, const AuthorizedTransaction& t)
{
    bincode(w, t.transaction);
    w.bincode_len(t.authorizations.size());
    for (auto& a : t.authorizations) bincode(w, a);
}

void bincode(Writer& w, const Coinbase& c)
{
    w.bincode_bytes(c.memo);
    w.bincode_len(c.outputs.size());
    for (auto& o : c.outputs) bincode(w, o);
}

void bincode(Writer& w, const Body& b)
{
    bincode(w, b.coinbase);
    w.bincode_len(b.transactions.size());
    for (auto& t : b.transactions) bincode(w, t);
    w.bincode_len(b.authorizations.size());
    for (auto& a : b.authorizations) bincode(w, a);
}

void bincode(Writer& w, const Header& h)
{
    w.raw(h.merkle_root);
    if (h.prev_side_hash) {
        w.u8(1);
        w.raw(*h.prev_side_hash);
    } else {
        w.u8(0);
    }
    w.bincode_bytes(h.prev_main_hash.span()); // bitcoin::BlockHash serde = bytes
}

void bincode(Writer& w, const FilledContent& c)
{
    w.u32(c.kind);
    switch (c.kind) {
    case FilledContent::AmmLpToken:
        bincode(w, c.asset0);
        bincode(w, c.asset1);
        w.u64(c.value);
        break;
    case FilledContent::Bitcoin: w.u64(c.value); break;
    case FilledContent::BitcoinWithdrawal: bt::sidechain::bincode_withdrawal(w, c.value, c.main_fee, c.main_address); break;
    case FilledContent::BitAsset:
        w.raw(c.id);
        w.u64(c.value);
        break;
    case FilledContent::BitAssetControl:
    case FilledContent::DutchAuctionReceipt: w.raw(c.id); break;
    case FilledContent::BitAssetReservation:
        w.raw(c.id);
        w.raw(c.commitment);
        break;
    }
}

void bincode(Writer& w, const FilledOutput& o)
{
    w.raw(o.address.b);
    bincode(w, o.content);
    w.bincode_bytes(o.memo);
}

Content de_content(Reader& r)
{
    Content c;
    uint32_t tag = r.u32();
    if (tag > 6) throw DecodeError("invalid Content tag");
    c.kind = Content::Kind(tag);
    switch (c.kind) {
    case Content::AmmLpToken:
    case Content::Bitcoin:
    case Content::BitAsset: c.value = r.u64(); break;
    case Content::Withdrawal: bt::sidechain::de_withdrawal(r, c.value, c.main_fee, c.main_address); break;
    default: break;
    }
    return c;
}

Output de_output(Reader& r)
{
    Output o;
    o.address.b = r.array<20>();
    o.content = de_content(r);
    o.memo = r.bincode_bytes();
    return o;
}

BitAssetData de_bitasset_data(Reader& r)
{
    BitAssetData d;
    d.commitment = de_opt<Hash32>(r, [&] { return r.array<32>(); });
    d.socket_addr_v4 = de_opt<SocketV4>(r, [&] { return de_v4(r); });
    d.socket_addr_v6 = de_opt<SocketV6>(r, [&] { return de_v6(r); });
    d.encryption_pubkey = de_opt<bt::ByteArray<32>>(r, [&] { return r.array<32>(); });
    d.signing_pubkey = de_opt<bt::frost::Point>(r, [&] { return de_vk_raw(r); });
    return d;
}

static BitAssetDataUpdates de_updates(Reader& r)
{
    BitAssetDataUpdates u;
    u.commitment = de_update<Hash32>(r.u32(), [&] { return r.array<32>(); });
    u.socket_addr_v4 = de_update<SocketV4>(r.u32(), [&] { return de_v4(r); });
    u.socket_addr_v6 = de_update<SocketV6>(r.u32(), [&] { return de_v6(r); });
    u.encryption_pubkey = de_update<bt::ByteArray<32>>(r.u32(), [&] { return r.array<32>(); });
    u.signing_pubkey = de_update<bt::frost::Point>(r.u32(), [&] { return de_vk_raw(r); });
    return u;
}

static DutchAuctionParams de_params(Reader& r)
{
    DutchAuctionParams p;
    p.start_block = r.u32();
    p.duration = r.u32();
    p.base_asset = de_asset_id(r);
    p.base_amount = r.u64();
    p.quote_asset = de_asset_id(r);
    p.initial_price = r.u64();
    p.final_price = r.u64();
    return p;
}

TxData de_txdata(Reader& r)
{
    TxData d;
    uint32_t tag = r.u32();
    if (tag > 9) throw DecodeError("invalid TxData tag");
    d.kind = TxData::Kind(tag);
    switch (d.kind) {
    case TxData::AmmBurn:
    case TxData::AmmMint:
        d.amount0 = r.u64();
        d.amount1 = r.u64();
        d.lp_tokens = r.u64();
        break;
    case TxData::AmmSwap:
        d.amount_spent = r.u64();
        d.amount_receive = r.u64();
        d.pair_asset = de_asset_id(r);
        break;
    case TxData::BitAssetReservation: d.commitment = r.array<32>(); break;
    case TxData::BitAssetRegistration:
        d.name_hash = r.array<32>();
        d.revealed_nonce = r.array<32>();
        d.bitasset_data = de_bitasset_data(r);
        d.initial_supply = r.u64();
        break;
    case TxData::BitAssetMint: d.mint_amount = r.u64(); break;
    case TxData::BitAssetUpdate: d.updates = de_updates(r); break;
    case TxData::DutchAuctionCreate: d.params = de_params(r); break;
    case TxData::DutchAuctionBid:
        d.auction_id = r.array<32>();
        d.receive_asset = de_asset_id(r);
        d.quantity = r.u64();
        d.bid_size = r.u64();
        break;
    case TxData::DutchAuctionCollect:
        d.asset_offered = de_asset_id(r);
        d.asset_receive = de_asset_id(r);
        d.amount_offered_remaining = r.u64();
        d.amount_received = r.u64();
        break;
    }
    return d;
}

Transaction de_transaction(Reader& r)
{
    Transaction t;
    size_t n = r.bincode_len(4 + 32 + 4);
    for (size_t i = 0; i < n; i++) t.inputs.push_back(bt::sidechain::de_outpoint(r));
    size_t m = r.bincode_len(20 + 4 + 8);
    for (size_t i = 0; i < m; i++) t.outputs.push_back(de_output(r));
    t.memo = r.bincode_bytes();
    t.data = de_opt<TxData>(r, [&] { return de_txdata(r); });
    return t;
}

Authorization de_authorization(Reader& r)
{
    Authorization a;
    a.verifying_key = r.array<32>();
    if (!bt::frost::valid_element(a.verifying_key)) throw DecodeError("invalid verifying key");
    Bytes sig = r.bincode_bytes();
    if (sig.size() != 64) throw DecodeError("MalformedSignature");
    a.signature = de_sig64(sig);
    return a;
}

AuthorizedTransaction de_authorized_transaction(Reader& r)
{
    AuthorizedTransaction t;
    t.transaction = de_transaction(r);
    size_t n = r.bincode_len(32 + 8 + 64);
    for (size_t i = 0; i < n; i++) t.authorizations.push_back(de_authorization(r));
    return t;
}

Coinbase de_coinbase(Reader& r)
{
    Coinbase c;
    c.memo = r.bincode_bytes();
    size_t n = r.bincode_len(20 + 4 + 8);
    for (size_t i = 0; i < n; i++) c.outputs.push_back(de_output(r));
    return c;
}

Body de_body(Reader& r)
{
    Body b;
    b.coinbase = de_coinbase(r);
    size_t n = r.bincode_len(8 + 8 + 8 + 1);
    for (size_t i = 0; i < n; i++) b.transactions.push_back(de_transaction(r));
    size_t m = r.bincode_len(32 + 8 + 64);
    for (size_t i = 0; i < m; i++) b.authorizations.push_back(de_authorization(r));
    return b;
}

Header de_header(Reader& r)
{
    Header h;
    h.merkle_root = r.array<32>();
    uint8_t opt = r.u8();
    if (opt == 1) h.prev_side_hash = r.array<32>();
    else if (opt != 0) throw DecodeError("invalid Option tag");
    Bytes pm = r.bincode_bytes();
    if (pm.size() != 32) throw DecodeError("invalid block hash length");
    h.prev_main_hash = Hash32::from_span(pm);
    return h;
}

FilledContent de_filled_content(Reader& r)
{
    FilledContent c;
    uint32_t tag = r.u32();
    if (tag > 6) throw DecodeError("invalid FilledContent tag");
    c.kind = FilledContent::Kind(tag);
    switch (c.kind) {
    case FilledContent::AmmLpToken:
        c.asset0 = de_asset_id(r);
        c.asset1 = de_asset_id(r);
        c.value = r.u64();
        break;
    case FilledContent::Bitcoin: c.value = r.u64(); break;
    case FilledContent::BitcoinWithdrawal: bt::sidechain::de_withdrawal(r, c.value, c.main_fee, c.main_address); break;
    case FilledContent::BitAsset:
        c.id = r.array<32>();
        c.value = r.u64();
        break;
    case FilledContent::BitAssetControl:
    case FilledContent::DutchAuctionReceipt: c.id = r.array<32>(); break;
    case FilledContent::BitAssetReservation:
        c.id = r.array<32>();
        c.commitment = r.array<32>();
        break;
    }
    return c;
}

FilledOutput de_filled_output(Reader& r)
{
    FilledOutput o;
    o.address.b = r.array<20>();
    o.content = de_filled_content(r);
    o.memo = r.bincode_bytes();
    return o;
}

// ---------------------------------------------------------------- merkle (merkle-cbt 0.3.2 CBMT)
namespace {
struct ValueNode {
    Hash32 commitment;
    uint64_t value = 0; // fees (txs) or output Bitcoin value (outputs): checked_add
    uint64_t size = 0;  // canonical sizes: checked_add
};
std::optional<ValueNode> merge_value(const ValueNode& l, const ValueNode& r)
{
    ValueNode n;
    n.value = l.value + r.value;
    if (n.value < l.value) return std::nullopt; // ValueOverflow / FeeOverflow
    n.size = l.size + r.size;
    if (n.size < l.size) return std::nullopt; // SizeOverflow
    Writer w;
    w.raw(l.commitment);
    w.u64(n.value);
    w.u64(n.size);
    w.raw(r.commitment);
    n.commitment = bt::blake3(w.buf);
    return n;
}
} // namespace

// outputs.rs:174-215
std::optional<Hash32> outputs_merkle_root(const std::vector<Output>& outs)
{
    std::vector<ValueNode> leaves;
    leaves.reserve(outs.size());
    for (auto& o : outs) {
        ValueNode n;
        n.value = o.bitcoin_value();
        Bytes ob = borsh_bytes(o);
        n.size = ob.size();
        Writer w;
        w.u64(n.value);
        w.u64(n.size);
        w.raw(ob);
        n.commitment = bt::blake3(w.buf);
        leaves.push_back(n);
    }
    auto r = bt::sidechain::cbmt_root(leaves, merge_value);
    if (!r) return std::nullopt;
    return r->commitment;
}

// inputs.rs:110-124
Hash32 inputs_merkle_root(const std::vector<OutPoint>& ins)
{
    std::vector<Hash32> leaves;
    leaves.reserve(ins.size());
    for (auto& op : ins) {
        Writer w;
        bt::sidechain::borsh(w, op);
        leaves.push_back(bt::blake3(w.buf));
    }
    auto r = bt::sidechain::cbmt_root(leaves, [](const Hash32& l, const Hash32& r) -> std::optional<Hash32> {
        Writer w;
        w.raw(l);
        w.raw(r);
        return bt::blake3(w.buf);
    });
    return *r;
}

std::optional<Hash32> coinbase_merkle_root(const Coinbase& cb)
{
    auto r = outputs_merkle_root(cb.outputs);
    if (!r) return std::nullopt; // ComputeMerkleRootError::Coinbase
    Writer w;
    w.borsh_bytes(cb.memo);
    w.raw(*r);
    return bt::blake3(w.buf);
}

// types/mod.rs:792-846
std::optional<Hash32> compute_merkle_root(const Coinbase& cb, const std::vector<FilledTransaction>& txs)
{
    std::vector<ValueNode> leaves;
    leaves.reserve(txs.size());
    for (auto& ft : txs) {
        auto fee = ft.bitcoin_fee();
        if (!fee) return std::nullopt; // TxFee
        uint64_t size = ft.tx->canonical_size();
        auto tmr = ft.tx->merkle_root();
        if (!tmr) return std::nullopt; // TxMerkleRoot
        Writer w;
        w.u64(*fee);
        w.u64(size);
        w.raw(*tmr);
        leaves.push_back(ValueNode{bt::blake3(w.buf), *fee, size});
    }
    auto r = bt::sidechain::cbmt_root(leaves, merge_value);
    if (!r) return std::nullopt;
    auto cbr = coinbase_merkle_root(cb);
    if (!cbr) return std::nullopt;
    Writer w;
    w.raw(*cbr);
    w.raw(r->commitment);
    return bt::blake3(w.buf);
}

// ---------------------------------------------------------------- authorizations
Bytes tx_signing_message(const Transaction& t)
{
    Bytes m{0}; // Dst::Transaction
    append(m, t.canonical_bytes());
    return m;
}

bool verify_with_dst(const bt::frost::Signature& sig, const bt::frost::Point& vk, uint8_t dst, ByteSpan msg)
{
    Bytes m{dst};
    append(m, msg);
    return bt::frost::verify(vk, sig, m);
}

bt::frost::Signature sign_with_dst(const bt::frost::SigningKey& sk, uint8_t dst, ByteSpan msg)
{
    Bytes m{dst};
    append(m, msg);
    return sk.sign(m);
}

// authorization.rs verify_authorized_transaction
bool verify_authorized_transaction(const AuthorizedTransaction& tx, std::string* why)
{
    size_t need = tx.transaction.inputs.size();
    if (tx.authorizations.size() < need) {
        if (why) *why = "not enough authorizations";
        return false;
    }
    if (tx.authorizations.size() > need) {
        if (why) *why = "too many authorizations";
        return false;
    }
    if (tx.authorizations.empty()) return true; // a transaction without inputs has nothing to sign
    Bytes msg = tx_signing_message(tx.transaction);
    for (auto& a : tx.authorizations)
        if (!bt::frost::verify(a.verifying_key, a.signature, msg)) {
            if (why) *why = "signature verification error";
            return false;
        }
    return true;
}

// authorization.rs verify_authorizations
bool verify_authorizations(const Body& body, std::string* why)
{
    size_t need = body.inputs_len();
    if (body.authorizations.size() < need) {
        if (why) *why = "not enough authorizations";
        return false;
    }
    if (body.authorizations.size() > need) {
        if (why) *why = "too many authorizations";
        return false;
    }
    size_t k = 0;
    for (auto& tx : body.transactions) {
        if (tx.inputs.empty()) continue;
        Bytes msg = tx_signing_message(tx);
        for (size_t i = 0; i < tx.inputs.size(); i++, k++) {
            auto& a = body.authorizations[k];
            if (!bt::frost::verify(a.verifying_key, a.signature, msg)) {
                if (why) *why = "signature verification error";
                return false;
            }
        }
    }
    return true;
}

} // namespace bitassets
