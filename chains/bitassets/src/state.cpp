// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Literal port of the plain-bitassets fork v0.17.1 (16f2f69) lib/state/{mod.rs, block.rs,
// bitassets.rs, amm.rs, dutch_auction.rs}. Function and branch order follow theirs so that
// every accept/reject decision and every state write is identical; their panics (assert!,
// expect, division by zero) are StateError{panic = true}, which rejects the block their node
// cannot connect either. Unchecked u64 arithmetic wraps (release build, no overflow checks).
#include <bitassets/state.h>

#include <bt/crypto.h>

#include <algorithm>
#include <set>

namespace bitassets {

using bt::kv::Txn;

// ------------------------------------------------------------------ deposits (proto.rs)
FilledOutput StateTraits::deposit_output(const bt::mainchain::Deposit& d)
{
    FilledOutput o;
    o.content.kind = FilledContent::Bitcoin;
    o.content.value = d.value_sats;
    o.address = bt::sidechain::deposit_address(d.address, THIS_SIDECHAIN);
    return o;
}

// ------------------------------------------------------------------ u128 helpers
u128 isqrt_u128(u128 x)
{
    if (x < 2) return x;
    // Newton iteration from a power of two above sqrt(x): exact floor(sqrt(x))
    int bits = 0;
    for (u128 y = x; y; y >>= 1) bits++;
    u128 r = u128(1) << ((bits + 1) / 2);
    while (true) {
        u128 nr = (r + x / r) / 2;
        if (nr >= r) break;
        r = nr;
    }
    // adjust without squaring (r + 1)^2 overflows u128 when r = 2^64 - 1: found by the oracle,
    // isqrt(u64::MAX * (u64::MAX - 1)) looped forever (a hang theirs does not have)
    while (r > x / r) r--;
    while (r + 1 <= x / (r + 1)) r++;
    return r;
}

static u128 div_ceil(u128 a, u128 b, const char* what)
{
    if (b == 0) throw StateError(std::string("attempt to divide by zero (") + what + ")", true);
    return a / b + (a % b != 0 ? 1 : 0);
}

// ------------------------------------------------------------------ BitAsset records (bitassets.rs:20-246)
BitAssetRecord BitAssetRecord::init(const BitAssetData& d, uint64_t initial_supply, const Hash32& txid, uint32_t height)
{
    BitAssetRecord r;
    r.commitment = TxHistory<std::optional<Hash32>>::make(d.commitment, txid, height);
    r.socket_addr_v4 = TxHistory<std::optional<SocketV4>>::make(d.socket_addr_v4, txid, height);
    r.socket_addr_v6 = TxHistory<std::optional<SocketV6>>::make(d.socket_addr_v6, txid, height);
    r.encryption_pubkey = TxHistory<std::optional<bt::ByteArray<32>>>::make(d.encryption_pubkey, txid, height);
    r.signing_pubkey = TxHistory<std::optional<bt::frost::Point>>::make(d.signing_pubkey, txid, height);
    r.total_supply = TxHistory<uint64_t>::make(initial_supply, txid, height);
    return r;
}

BitAssetData BitAssetRecord::current() const
{
    return BitAssetData{commitment.latest().data, socket_addr_v4.latest().data, socket_addr_v6.latest().data, encryption_pubkey.latest().data,
                        signing_pubkey.latest().data};
}

std::optional<BitAssetData> BitAssetRecord::at_block_height(uint32_t h) const
{
    auto c = commitment.at_block_height(h);
    auto v4 = socket_addr_v4.at_block_height(h);
    auto v6 = socket_addr_v6.at_block_height(h);
    auto ek = encryption_pubkey.at_block_height(h);
    auto sk = signing_pubkey.at_block_height(h);
    if (!c || !v4 || !v6 || !ek || !sk) return std::nullopt;
    return BitAssetData{c->data, v4->data, v6->data, ek->data, sk->data};
}

template <class T>
static void apply_field(TxHistory<std::optional<T>>& f, const Update<T>& u, const Hash32& txid, uint32_t h)
{
    if (u.kind == Update<T>::Delete) f.push(std::nullopt, txid, h);
    else if (u.kind == Update<T>::Set) f.push(u.value, txid, h);
}

void BitAssetRecord::apply_updates(const BitAssetDataUpdates& u, const Hash32& txid, uint32_t height)
{
    apply_field(commitment, u.commitment, txid, height);
    apply_field(socket_addr_v4, u.socket_addr_v4, txid, height);
    apply_field(socket_addr_v6, u.socket_addr_v6, txid, height);
    apply_field(encryption_pubkey, u.encryption_pubkey, txid, height);
    apply_field(signing_pubkey, u.signing_pubkey, txid, height);
}

// bitassets.rs:150-178 revert_field_update: NonEmpty::pop keeps the head (None when only the
// head remains), then assert!s on the popped entry.
template <class T>
static void revert_field(TxHistory<std::optional<T>>& f, const Update<T>& u, const Hash32& txid, uint32_t h)
{
    if (u.kind == Update<T>::Retain) return;
    auto popped = f.pop();
    if (!popped) throw StateError("revert_updates: assert!(popped.is_some())", true);
    if (u.kind == Update<T>::Delete) {
        if (popped->data.has_value()) throw StateError("revert_updates: assert!(popped.data.is_none())", true);
    } else {
        if (!popped->data.has_value()) throw StateError("revert_updates: assert!(popped.data.is_some())", true);
        if (!(*popped->data == u.value)) throw StateError("revert_updates: assert_eq!(popped.data, value)", true);
    }
    if (popped->txid != txid) throw StateError("revert_updates: assert_eq!(popped.txid, txid)", true);
    if (popped->height != h) throw StateError("revert_updates: assert_eq!(popped.height, height)", true);
}

void BitAssetRecord::revert_updates(const BitAssetDataUpdates& u, const Hash32& txid, uint32_t height)
{
    // reverse field order (bitassets.rs:188-212)
    revert_field(signing_pubkey, u.signing_pubkey, txid, height);
    revert_field(encryption_pubkey, u.encryption_pubkey, txid, height);
    revert_field(socket_addr_v6, u.socket_addr_v6, txid, height);
    revert_field(socket_addr_v4, u.socket_addr_v4, txid, height);
    revert_field(commitment, u.commitment, txid, height);
}

// ------------------------------------------------------------------ AMM pool math (amm.rs:50-338)
std::optional<PoolState> PoolState::mint(uint64_t amount0, uint64_t amount1, std::string& err) const
{
    uint64_t nr0 = reserve0 + amount0;
    if (nr0 < reserve0) return err = "Invalid AMM mint", std::nullopt;
    uint64_t nr1 = reserve1 + amount1;
    if (nr1 < reserve1) return err = "Invalid AMM mint", std::nullopt;
    if (reserve0 == 0 || reserve1 == 0 || outstanding_lp_tokens == 0) {
        uint64_t minted = uint64_t(isqrt_u128(u128(nr0) * u128(nr1)));
        return PoolState{nr0, nr1, outstanding_lp_tokens + minted /* unchecked: wraps */, creation_txid};
    }
    u128 m0 = u128(outstanding_lp_tokens) * u128(amount0) / u128(reserve0);
    u128 m1 = u128(outstanding_lp_tokens) * u128(amount1) / u128(reserve1);
    u128 m = std::min(m0, m1);
    if (m > UINT64_MAX) return err = "AMM LP token overflow", std::nullopt;
    uint64_t no = outstanding_lp_tokens + uint64_t(m);
    if (no < outstanding_lp_tokens) return err = "AMM LP token overflow", std::nullopt;
    return PoolState{nr0, nr1, no, creation_txid};
}

std::optional<PoolState> PoolState::revert_mint(uint64_t amount0, uint64_t amount1, uint64_t lp, std::string& err) const
{
    if (amount0 > reserve0 || amount1 > reserve1) return err = "Invalid AMM mint", std::nullopt;
    if (lp > outstanding_lp_tokens) return err = "AMM LP token underflow", std::nullopt;
    PoolState ns{reserve0 - amount0, reserve1 - amount1, outstanding_lp_tokens - lp, creation_txid};
    auto again = ns.mint(amount0, amount1, err);
    if (!again) return std::nullopt;
    if (*again == *this) return ns;
    err = "Failed to revert AMM mint";
    return std::nullopt;
}

std::optional<PoolState> PoolState::burn(uint64_t lp_token_burn, std::string& err) const
{
    if (outstanding_lp_tokens == 0) return err = "Invalid AMM burn", std::nullopt;
    auto payout = [&](uint64_t reserve) -> std::optional<uint64_t> {
        u128 p = u128(reserve) * u128(lp_token_burn) / u128(outstanding_lp_tokens);
        if (p > UINT64_MAX) return std::nullopt;
        return uint64_t(p);
    };
    auto p0 = payout(reserve0);
    if (!p0) return err = "AMM burn overflow", std::nullopt;
    auto p1 = payout(reserve1);
    if (!p1) return err = "AMM burn overflow", std::nullopt;
    if (*p0 > reserve0 || *p1 > reserve1 || lp_token_burn > outstanding_lp_tokens) return err = "AMM burn underflow", std::nullopt;
    return PoolState{reserve0 - *p0, reserve1 - *p1, outstanding_lp_tokens - lp_token_burn, creation_txid};
}

std::optional<PoolState> PoolState::unburn(uint64_t lp, uint64_t payout0, uint64_t payout1, std::string& err) const
{
    uint64_t no = outstanding_lp_tokens + lp;
    if (no < outstanding_lp_tokens) return err = "AMM burn overflow", std::nullopt;
    uint64_t r0 = reserve0 + payout0;
    if (r0 < reserve0) return err = "AMM burn overflow", std::nullopt;
    uint64_t r1 = reserve1 + payout1;
    if (r1 < reserve1) return err = "AMM burn overflow", std::nullopt;
    return PoolState{r0, r1, no, creation_txid};
}

// amm.rs:211-305, as theirs (consensus: do not change).
static std::optional<std::pair<uint64_t, uint64_t>> swap_core(uint64_t spend_reserve, uint64_t receive_reserve, uint64_t amount_spend, std::string& err)
{
    u128 product = u128(spend_reserve) * u128(receive_reserve);
    uint64_t after_fee = uint64_t(u128(amount_spend) * 997 / 1000);
    if (after_fee > amount_spend) return err = "Invalid AMM swap", std::nullopt; // _spend_fee (never)
    u128 effective = u128(spend_reserve) + u128(after_fee);
    u128 nrb = div_ceil(product, effective, "AMM swap");
    if (nrb > UINT64_MAX) return err = "Invalid AMM swap", std::nullopt;
    if (uint64_t(nrb) > receive_reserve) return err = "Invalid AMM swap", std::nullopt;
    uint64_t before = receive_reserve - uint64_t(nrb);
    uint64_t receive_after_fee = uint64_t(u128(before) * 997 / 1000);
    // _receive_fee = before.checked_sub(before): always Some(0)
    uint64_t new_spend_reserve = spend_reserve + amount_spend;
    if (new_spend_reserve < spend_reserve) return err = "AMM pool reserve overflow", std::nullopt;
    if (receive_after_fee > receive_reserve) return err = "Insufficient liquidity", std::nullopt;
    return std::make_pair(new_spend_reserve, receive_reserve - receive_after_fee);
}

std::optional<PoolState> PoolState::swap_asset0_for_asset1(uint64_t amount_spend, std::string& err) const
{
    auto r = swap_core(reserve0, reserve1, amount_spend, err);
    if (!r) return std::nullopt;
    return PoolState{r->first, r->second, outstanding_lp_tokens, creation_txid};
}

std::optional<PoolState> PoolState::swap_asset1_for_asset0(uint64_t amount_spend, std::string& err) const
{
    // order of their checks: reserve0 underflow (InsufficientLiquidity) before reserve1 overflow
    u128 product = u128(reserve0) * u128(reserve1);
    uint64_t after_fee = uint64_t(u128(amount_spend) * 997 / 1000);
    u128 effective = u128(reserve1) + u128(after_fee);
    u128 nrb = div_ceil(product, effective, "AMM swap");
    if (nrb > UINT64_MAX) return err = "Invalid AMM swap", std::nullopt;
    if (uint64_t(nrb) > reserve0) return err = "Invalid AMM swap", std::nullopt;
    uint64_t before = reserve0 - uint64_t(nrb);
    uint64_t receive_after_fee = uint64_t(u128(before) * 997 / 1000);
    if (receive_after_fee > reserve0) return err = "Insufficient liquidity", std::nullopt;
    uint64_t nr1 = reserve1 + amount_spend;
    if (nr1 < reserve1) return err = "AMM pool reserve overflow", std::nullopt;
    return PoolState{reserve0 - receive_after_fee, nr1, outstanding_lp_tokens, creation_txid};
}

// amm.rs:308-337, as theirs
std::optional<PoolState> PoolState::revert_swap(const AmmSwapOp& s, std::string& err) const
{
    AssetId a0 = std::min(s.asset_receive, s.asset_spend), a1 = std::max(s.asset_receive, s.asset_spend);
    PoolState ns = *this;
    if (s.asset_spend == a1) {
        ns.reserve0 = reserve0 + s.amount_receive;
        ns.reserve1 = reserve1 - s.amount_spend;
    } else {
        ns.reserve0 = reserve0 - s.amount_spend;
        ns.reserve1 = reserve1 + s.amount_receive;
    }
    auto check = s.asset_spend == a0 ? ns.swap_asset0_for_asset1(s.amount_spend, err) : ns.swap_asset1_for_asset0(s.amount_spend, err);
    if (!check) return std::nullopt;
    if (*check == *this) return ns;
    err = "Failed to revert AMM swap";
    return std::nullopt;
}

// ------------------------------------------------------------------ auctions (dutch_auction.rs:47-199)
std::optional<AuctionState> AuctionState::bid(const Hash32& txid, uint64_t bid_amount, uint32_t height, std::string& err) const
{
    if (height < start_block) return err = "Auction has not started yet", std::nullopt;
    uint32_t last = most_recent_bid_block.latest().data;
    uint32_t elapsed = height - last;                  // u32, wraps
    uint32_t end_block = start_block + (duration - 1); // saturating_add below
    {
        uint64_t e = uint64_t(start_block) + uint32_t(duration - 1);
        end_block = e > UINT32_MAX ? UINT32_MAX : uint32_t(e);
    }
    uint64_t rem = base_amount_remaining.latest().data;
    if (height > end_block || rem == 0) return err = "Auction has already ended", std::nullopt;
    uint32_t remaining_duration = end_block - last;
    uint64_t P = price_after_most_recent_bid.latest().data, E = end_price_after_most_recent_bid.latest().data;
    uint64_t price;
    if (remaining_duration == 0) {
        price = P;
    } else {
        u128 max_dec = u128(uint64_t(P - E)); // u64 subtraction first (wraps), then the cast
        u128 dec = max_dec * u128(elapsed) / u128(remaining_duration);
        if (elapsed / remaining_duration >= 1) throw StateError("assert!(elapsed_blocks / remaining_duration_at_most_recent_bid < 1)", true); // B8
        price = P - uint64_t(dec);
    }
    if (price == 0) return err = "Tx can only be applied at the specified price", std::nullopt; // InvalidPrice
    u128 q = div_ceil(u128(bid_amount) * u128(rem), u128(price), "auction bid");
    if (q > u128(rem)) return err = "Bid quantity is more than is offered in the auction", std::nullopt;
    uint64_t new_rem = rem - uint64_t(q);
    uint64_t end_price = uint64_t(div_ceil(u128(E) * u128(new_rem), u128(rem), "auction end price"));
    AuctionState s = *this;
    s.most_recent_bid_block.push(height, txid, height);
    s.base_amount_remaining.push(new_rem, txid, height);
    s.quote_amount.push(quote_amount.latest().data + bid_amount /* unchecked */, txid, height);
    s.price_after_most_recent_bid.push(price - bid_amount /* unchecked */, txid, height);
    s.end_price_after_most_recent_bid.push(end_price, txid, height);
    return s;
}

AuctionState AuctionState::revert_bid(const Hash32& txid) const
{
    AuctionState s = *this;
    auto chk = [&](auto& h, const char* what) {
        auto p = h.pop();
        if (!p || p->txid != txid) throw StateError(std::string("revert_bid: assert! ") + what, true);
    };
    chk(s.most_recent_bid_block, "most_recent_bid_block");
    chk(s.base_amount_remaining, "base_amount_remaining");
    chk(s.quote_amount, "quote_amount");
    chk(s.price_after_most_recent_bid, "price_after_most_recent_bid");
    chk(s.end_price_after_most_recent_bid, "end_price_after_most_recent_bid");
    return s;
}

// ------------------------------------------------------------------ encodings (ours; LMDB key forms where order matters)
static Bytes u32le(uint32_t v)
{
    Writer w;
    w.u32(v);
    return w.buf;
}

template <class T, class F>
static void enc_hist(Writer& w, const TxHistory<T>& h, F encv)
{
    w.u64(h.v.size());
    for (auto& e : h.v) {
        encv(w, e.data);
        w.raw(e.txid);
        w.u32(e.height);
    }
}
template <class T, class F>
static TxHistory<T> dec_hist(Reader& r, F decv)
{
    TxHistory<T> h;
    uint64_t n = r.u64();
    for (uint64_t i = 0; i < n; i++) {
        bt::sidechain::TxidStamped<T> e{decv(r), {}, 0};
        e.txid = r.array<32>();
        e.height = r.u32();
        h.v.push_back(std::move(e));
    }
    if (h.v.empty()) throw bt::DecodeError("empty history");
    return h;
}
template <class T, class F>
static auto opt_enc(F f)
{
    return [f](Writer& w, const std::optional<T>& v) { bt::sidechain::borsh_opt(w, v, [&](const T& x) { f(w, x); }); };
}
template <class T, class F>
static auto opt_dec(F f)
{
    return [f](Reader& r) { return bt::sidechain::de_opt<T>(r, [&] { return f(r); }); };
}

static Bytes enc_record(const BitAssetRecord& b)
{
    Writer w;
    enc_hist(w, b.commitment, opt_enc<Hash32>([](Writer& x, const Hash32& v) { x.raw(v); }));
    enc_hist(w, b.socket_addr_v4, opt_enc<SocketV4>([](Writer& x, const SocketV4& v) { bt::sidechain::raw_v4(x, v); }));
    enc_hist(w, b.socket_addr_v6, opt_enc<SocketV6>([](Writer& x, const SocketV6& v) { bt::sidechain::raw_v6(x, v); }));
    enc_hist(w, b.encryption_pubkey, opt_enc<bt::ByteArray<32>>([](Writer& x, const bt::ByteArray<32>& v) { x.raw(v); }));
    enc_hist(w, b.signing_pubkey, opt_enc<bt::frost::Point>([](Writer& x, const bt::frost::Point& v) { x.raw(v); }));
    enc_hist(w, b.total_supply, [](Writer& x, uint64_t v) { x.u64(v); });
    return w.buf;
}

static BitAssetRecord dec_record(ByteSpan b)
{
    Reader r(b);
    BitAssetRecord x;
    x.commitment = dec_hist<std::optional<Hash32>>(r, opt_dec<Hash32>([](Reader& rr) { return rr.array<32>(); }));
    x.socket_addr_v4 = dec_hist<std::optional<SocketV4>>(r, opt_dec<SocketV4>([](Reader& rr) { return bt::sidechain::de_v4(rr); }));
    x.socket_addr_v6 = dec_hist<std::optional<SocketV6>>(r, opt_dec<SocketV6>([](Reader& rr) { return bt::sidechain::de_v6(rr); }));
    x.encryption_pubkey = dec_hist<std::optional<bt::ByteArray<32>>>(r, opt_dec<bt::ByteArray<32>>([](Reader& rr) { return rr.array<32>(); }));
    x.signing_pubkey = dec_hist<std::optional<bt::frost::Point>>(r, opt_dec<bt::frost::Point>([](Reader& rr) { return rr.array<32>(); }));
    x.total_supply = dec_hist<uint64_t>(r, [](Reader& rr) { return rr.u64(); });
    return x;
}

static Bytes enc_pool(const PoolState& p)
{
    Writer w;
    w.u64(p.reserve0);
    w.u64(p.reserve1);
    w.u64(p.outstanding_lp_tokens);
    w.raw(p.creation_txid);
    return w.buf;
}
static PoolState dec_pool(ByteSpan b)
{
    Reader r(b);
    PoolState p;
    p.reserve0 = r.u64();
    p.reserve1 = r.u64();
    p.outstanding_lp_tokens = r.u64();
    p.creation_txid = r.array<32>();
    return p;
}

static Bytes enc_auction(const AuctionState& a)
{
    Writer w;
    w.u32(a.start_block);
    enc_hist(w, a.most_recent_bid_block, [](Writer& x, uint32_t v) { x.u32(v); });
    w.u32(a.duration);
    bincode(w, a.base_asset);
    w.u64(a.initial_base_amount);
    enc_hist(w, a.base_amount_remaining, [](Writer& x, uint64_t v) { x.u64(v); });
    bincode(w, a.quote_asset);
    enc_hist(w, a.quote_amount, [](Writer& x, uint64_t v) { x.u64(v); });
    w.u64(a.initial_price);
    enc_hist(w, a.price_after_most_recent_bid, [](Writer& x, uint64_t v) { x.u64(v); });
    w.u64(a.initial_end_price);
    enc_hist(w, a.end_price_after_most_recent_bid, [](Writer& x, uint64_t v) { x.u64(v); });
    return w.buf;
}
static AuctionState dec_auction(ByteSpan b)
{
    Reader r(b);
    AuctionState a;
    auto u64d = [](Reader& rr) { return rr.u64(); };
    a.start_block = r.u32();
    a.most_recent_bid_block = dec_hist<uint32_t>(r, [](Reader& rr) { return rr.u32(); });
    a.duration = r.u32();
    a.base_asset = de_asset_id(r);
    a.initial_base_amount = r.u64();
    a.base_amount_remaining = dec_hist<uint64_t>(r, u64d);
    a.quote_asset = de_asset_id(r);
    a.quote_amount = dec_hist<uint64_t>(r, u64d);
    a.initial_price = r.u64();
    a.price_after_most_recent_bid = dec_hist<uint64_t>(r, u64d);
    a.initial_end_price = r.u64();
    a.end_price_after_most_recent_bid = dec_hist<uint64_t>(r, u64d);
    return a;
}

// AmmPair serde (bincode): the two AssetIds in AmmPair::new order, each as its borsh bytes
Bytes State::pair_key(const AssetId& a0, const AssetId& a1)
{
    Writer w;
    bincode(w, std::min(a0, a1));
    bincode(w, std::max(a0, a1));
    return w.buf;
}

// ------------------------------------------------------------------ getters
std::optional<BitAssetRecord> State::bitasset(const Txn& t, const Hash32& id) const
{
    auto v = t.get(T_BITASSETS, id.span());
    if (!v) return std::nullopt;
    return dec_record(*v);
}

void State::put_bitasset(Txn& t, const Hash32& id, const BitAssetRecord& r) const { t.put(T_BITASSETS, id.span(), enc_record(r)); }

std::vector<std::pair<Hash32, BitAssetRecord>> State::bitassets(const Txn& t) const
{
    std::vector<std::pair<Hash32, BitAssetRecord>> r;
    t.scan(T_BITASSETS, [&](ByteSpan k, ByteSpan v) {
        r.emplace_back(Hash32::from_span(k), dec_record(v));
        return true;
    });
    return r;
}

std::optional<uint32_t> State::bitasset_seq(const Txn& t, const Hash32& id) const
{
    auto v = t.get(T_BITASSET_TO_SEQ, id.span());
    if (!v) return std::nullopt;
    Reader r(*v);
    return r.u32();
}

std::vector<std::pair<uint32_t, Hash32>> State::seq_to_bitasset(const Txn& t) const
{
    std::vector<std::pair<uint32_t, Hash32>> r;
    t.scan(T_SEQ_TO_BITASSET, [&](ByteSpan k, ByteSpan v) {
        Reader kr(k);
        r.emplace_back(kr.u32(), Hash32::from_span(v));
        return true;
    });
    return r;
}

std::optional<Hash32> State::reservation(const Txn& t, const Hash32& txid) const
{
    auto v = t.get(T_RESERVATIONS, txid.span());
    if (!v) return std::nullopt;
    return Hash32::from_span(*v);
}

std::vector<std::pair<Hash32, Hash32>> State::reservations(const Txn& t) const
{
    std::vector<std::pair<Hash32, Hash32>> r;
    t.scan(T_RESERVATIONS, [&](ByteSpan k, ByteSpan v) {
        r.emplace_back(Hash32::from_span(k), Hash32::from_span(v));
        return true;
    });
    return r;
}

std::optional<BitAssetData> State::bitasset_at(const Txn& t, const Hash32& id, uint32_t h) const
{
    auto r = bitasset(t, id);
    if (!r) return std::nullopt;
    return r->at_block_height(h);
}

std::optional<PoolState> State::pool(const Txn& t, const AssetId& a0, const AssetId& a1) const
{
    auto v = t.get(T_AMM_POOLS, pair_key(a0, a1));
    if (!v) return std::nullopt;
    return dec_pool(*v);
}

void State::put_pool(Txn& t, const AssetId& a0, const AssetId& a1, const PoolState& p) const { t.put(T_AMM_POOLS, pair_key(a0, a1), enc_pool(p)); }

std::vector<std::pair<std::pair<AssetId, AssetId>, PoolState>> State::pools(const Txn& t) const
{
    std::vector<std::pair<std::pair<AssetId, AssetId>, PoolState>> r;
    t.scan(T_AMM_POOLS, [&](ByteSpan k, ByteSpan v) {
        Reader kr(k);
        AssetId a0 = de_asset_id(kr), a1 = de_asset_id(kr);
        r.emplace_back(std::make_pair(a0, a1), dec_pool(v));
        return true;
    });
    return r;
}

std::optional<AuctionState> State::auction(const Txn& t, const Hash32& id) const
{
    auto v = t.get(T_DUTCH_AUCTIONS, id.span());
    if (!v) return std::nullopt;
    return dec_auction(*v);
}

void State::put_auction(Txn& t, const Hash32& id, const AuctionState& a) const { t.put(T_DUTCH_AUCTIONS, id.span(), enc_auction(a)); }

std::vector<std::pair<Hash32, AuctionState>> State::auctions(const Txn& t) const
{
    std::vector<std::pair<Hash32, AuctionState>> r;
    t.scan(T_DUTCH_AUCTIONS, [&](ByteSpan k, ByteSpan v) {
        r.emplace_back(Hash32::from_span(k), dec_auction(v));
        return true;
    });
    return r;
}

// ------------------------------------------------------------------ bitassets.rs
static std::optional<Hash32> burned_reservation_txid(const FilledTransaction& ft, const Hash32& implied)
{
    for (auto& u : ft.spent_utxos)
        if (u.content.is_reservation() && u.content.commitment == implied) return u.content.id;
    return std::nullopt;
}

// bitassets.rs:505-556
void State::apply_registration(Txn& t, const FilledTransaction& ft, uint32_t height) const
{
    const TxData& d = *ft.tx->data;
    auto implied = ft.tx->implied_reservation_commitment();
    if (!implied) throw StateError("no reservation for registration of BitAsset " + bt::hex_encode(d.name_hash));
    auto burned = burned_reservation_txid(ft, *implied);
    if (!burned) throw StateError("no reservation for registration of BitAsset " + bt::hex_encode(d.name_hash));
    if (!t.del(T_RESERVATIONS, burned->span())) throw StateError("missing BitAsset reservation " + bt::hex_encode(*burned));
    // next_seq: last() of the LE-keyed table under memcmp order, + 1 (u32 `+` wraps)
    uint32_t seq = 0;
    if (auto l = t.last(T_SEQ_TO_BITASSET)) {
        Reader kr(l->first);
        seq = kr.u32() + 1;
    }
    t.put(T_SEQ_TO_BITASSET, u32le(seq), d.name_hash.span());
    t.put(T_BITASSET_TO_SEQ, d.name_hash.span(), u32le(seq));
    put_bitasset(t, d.name_hash, BitAssetRecord::init(d.bitasset_data, d.initial_supply, ft.tx->txid(), height));
}

// bitassets.rs:559-596
void State::revert_registration(Txn& t, const FilledTransaction& ft) const
{
    const Hash32& id = ft.tx->data->name_hash;
    auto seq = bitasset_seq(t, id);
    if (!seq) throw StateError("missing BitAsset " + bt::hex_encode(id));
    t.del(T_BITASSET_TO_SEQ, id.span());
    if (!t.del(T_SEQ_TO_BITASSET, u32le(*seq))) throw StateError("missing BitAsset " + bt::hex_encode(id));
    if (!t.del(T_BITASSETS, id.span())) throw StateError("missing BitAsset " + bt::hex_encode(id));
    auto implied = ft.tx->implied_reservation_commitment();
    if (!implied) throw StateError("A BitAsset registration tx should have an implied commitment", true);
    auto burned = burned_reservation_txid(ft, *implied);
    if (!burned) throw StateError("A BitAsset registration tx should correspond to a burned reservation", true);
    t.put(T_RESERVATIONS, burned->span(), implied->span());
}

static std::optional<Hash32> last_spent_control(const FilledTransaction& ft)
{
    for (auto it = ft.spent_utxos.rbegin(); it != ft.spent_utxos.rend(); ++it)
        if (it->content.is_control()) return it->content.id;
    return std::nullopt;
}
static std::optional<Hash32> last_spent_bitasset(const FilledTransaction& ft)
{
    for (auto it = ft.spent_utxos.rbegin(); it != ft.spent_utxos.rend(); ++it)
        if (it->content.is_bitasset()) return it->content.id;
    return std::nullopt;
}

// bitassets.rs:599-636
void State::apply_mint(Txn& t, const FilledTransaction& ft, uint32_t height) const
{
    auto id = last_spent_control(ft);
    if (!id) throw StateError("no BitAssets to mint");
    auto rec = bitasset(t, *id);
    if (!rec) throw StateError("missing BitAsset " + bt::hex_encode(*id));
    uint64_t cur = rec->total_supply.latest().data, amount = ft.tx->data->mint_amount;
    if (cur + amount < cur) throw StateError("Mint would cause total supply to overflow");
    rec->total_supply.push(cur + amount, ft.tx->txid(), height);
    put_bitasset(t, *id, *rec);
}

// bitassets.rs:639-673
void State::revert_mint(Txn& t, const FilledTransaction& ft) const
{
    auto id = last_spent_control(ft);
    if (!id) throw StateError("no BitAssets to mint");
    auto rec = bitasset(t, *id);
    if (!rec) throw StateError("missing BitAsset " + bt::hex_encode(*id));
    uint64_t total = rec->total_supply.latest().data, amount = ft.tx->data->mint_amount;
    rec->total_supply.pop(); // NonEmpty::pop keeps the head
    uint64_t now = rec->total_supply.latest().data;
    if (amount > total) throw StateError("Reverting Mint would cause total supply to underflow");
    if (now != total - amount) throw StateError("revert_mint: assert_eq!(new_total_supply, total_supply - mint_amount)", true);
    put_bitasset(t, *id, *rec);
}

// bitassets.rs:441-502, as theirs
void State::apply_updates(Txn& t, const FilledTransaction& ft, uint32_t height) const
{
    auto id = last_spent_bitasset(ft);
    if (!id) throw StateError("no BitAssets to update");
    auto rec = bitasset(t, *id);
    if (!rec) throw StateError("missing BitAsset " + bt::hex_encode(*id));
    rec->apply_updates(ft.tx->data->updates, ft.tx->txid(), height);
    put_bitasset(t, *id, *rec);
}

void State::revert_updates(Txn& t, const FilledTransaction& ft, uint32_t height) const
{
    auto id = last_spent_bitasset(ft);
    if (!id) throw StateError("no BitAssets to update");
    auto rec = bitasset(t, *id);
    if (!rec) throw StateError("missing BitAsset " + bt::hex_encode(*id));
    rec->revert_updates(ft.tx->data->updates, ft.tx->txid(), height);
    put_bitasset(t, *id, *rec);
}

// ------------------------------------------------------------------ amm.rs:346-532
static std::string missing_pool(const AssetId& a0, const AssetId& a1)
{
    return "missing AMM pool state for " + std::min(a0, a1).to_hex() + "-" + std::max(a0, a1).to_hex();
}

void State::apply_burn(Txn& t, const FilledTransaction& ft) const
{
    auto b = ft.amm_burn();
    if (!b) throw StateError("Invalid AMM burn");
    auto p = pool(t, b->asset0, b->asset1);
    if (!p) throw StateError(missing_pool(b->asset0, b->asset1));
    std::string err;
    auto np = p->burn(b->lp_token_burn, err);
    if (!np) throw StateError(err);
    if (p->reserve0 - np->reserve0 != b->amount0) throw StateError("Invalid AMM burn");
    if (p->reserve1 - np->reserve1 != b->amount1) throw StateError("Invalid AMM burn");
    put_pool(t, b->asset0, b->asset1, *np);
}

void State::revert_burn(Txn& t, const FilledTransaction& ft) const
{
    auto b = ft.amm_burn();
    if (!b) throw StateError("Invalid AMM burn");
    auto p = pool(t, b->asset0, b->asset1);
    if (!p) throw StateError(missing_pool(b->asset0, b->asset1));
    std::string err;
    auto np = p->unburn(b->lp_token_burn, b->amount0, b->amount1, err);
    if (!np) throw StateError(err);
    put_pool(t, b->asset0, b->asset1, *np);
}

void State::apply_amm_mint(Txn& t, const FilledTransaction& ft) const
{
    auto m = ft.amm_mint();
    if (!m) throw StateError("Invalid AMM mint");
    if (m->asset0 == m->asset1) throw StateError("Invalid AMM mint");
    auto p = pool(t, m->asset0, m->asset1);
    PoolState cur = p ? *p : PoolState::fresh(ft.tx->txid());
    std::string err;
    auto np = cur.mint(m->amount0, m->amount1, err);
    if (!np) throw StateError(err);
    if (np->outstanding_lp_tokens < cur.outstanding_lp_tokens) throw StateError("Invalid AMM mint"); // checked_sub
    if (np->outstanding_lp_tokens - cur.outstanding_lp_tokens != m->lp_token_mint) throw StateError("Invalid AMM mint");
    put_pool(t, m->asset0, m->asset1, *np);
}

void State::revert_amm_mint(Txn& t, const FilledTransaction& ft) const
{
    auto m = ft.amm_mint();
    if (!m) throw StateError("Invalid AMM mint");
    if (m->asset0 == m->asset1) throw StateError("Invalid AMM mint");
    auto p = pool(t, m->asset0, m->asset1);
    if (!p) throw StateError(missing_pool(m->asset0, m->asset1));
    if (p->creation_txid == ft.tx->txid()) {
        t.del(T_AMM_POOLS, pair_key(m->asset0, m->asset1));
        return;
    }
    std::string err;
    auto np = p->revert_mint(m->amount0, m->amount1, m->lp_token_mint, err);
    if (!np) throw StateError(err);
    put_pool(t, m->asset0, m->asset1, *np);
}

void State::apply_swap(Txn& t, const FilledTransaction& ft) const
{
    auto s = ft.amm_swap();
    if (!s) throw StateError("Invalid AMM swap");
    auto p = pool(t, s->asset_spend, s->asset_receive);
    if (!p) throw StateError(missing_pool(s->asset_spend, s->asset_receive));
    std::string err;
    std::optional<PoolState> np;
    uint64_t received;
    if (s->asset_spend < s->asset_receive) {
        np = p->swap_asset0_for_asset1(s->amount_spend, err);
        if (!np) throw StateError(err);
        received = p->reserve1 - np->reserve1;
    } else {
        np = p->swap_asset1_for_asset0(s->amount_spend, err);
        if (!np) throw StateError(err);
        received = p->reserve0 - np->reserve0;
    }
    if (s->amount_receive != received) throw StateError("Invalid AMM swap");
    put_pool(t, s->asset_spend, s->asset_receive, *np);
}

void State::revert_swap(Txn& t, const FilledTransaction& ft) const
{
    auto s = ft.amm_swap();
    if (!s) throw StateError("Invalid AMM swap");
    auto p = pool(t, s->asset_spend, s->asset_receive);
    if (!p) throw StateError(missing_pool(s->asset_spend, s->asset_receive));
    std::string err;
    auto np = p->revert_swap(*s, err);
    if (!np) throw StateError(err);
    put_pool(t, s->asset_spend, s->asset_receive, *np);
}

// ------------------------------------------------------------------ dutch_auction.rs:213-447
void State::apply_bid(Txn& t, const FilledTransaction& ft, uint32_t height) const
{
    auto b = ft.dutch_auction_bid();
    if (!b) throw StateError("Invalid TxData");
    auto a = auction(t, b->auction_id);
    if (!a) throw StateError("Auction not found");
    if (b->asset_receive != a->base_asset) throw StateError("Incorrect receive asset specified");
    if (b->asset_spend != a->quote_asset) throw StateError("Incorrect spend asset");
    if (b->amount_receive > a->base_amount_remaining.latest().data) throw StateError("Bid quantity is more than is offered in the auction");
    std::string err;
    auto na = a->bid(ft.tx->txid(), b->amount_spend, height, err);
    if (!na) throw StateError(err);
    uint64_t q = a->base_amount_remaining.latest().data - na->base_amount_remaining.latest().data;
    if (b->amount_receive != q) throw StateError("Tx can only be applied at the specified price");
    put_auction(t, b->auction_id, *na);
}

void State::revert_bid(Txn& t, const FilledTransaction& ft) const
{
    auto b = ft.dutch_auction_bid();
    if (!b) throw StateError("Invalid TxData");
    auto a = auction(t, b->auction_id);
    if (!a) throw StateError("Auction not found");
    if (b->asset_receive != a->base_asset) throw StateError("Incorrect receive asset specified");
    if (b->asset_spend != a->quote_asset) throw StateError("Incorrect spend asset");
    put_auction(t, b->auction_id, a->revert_bid(ft.tx->txid()));
}

void State::apply_create(Txn& t, const FilledTransaction& ft, uint32_t height) const
{
    const DutchAuctionParams& p = ft.tx->data->params;
    if (height >= p.start_block) throw StateError("Tx expired; Auction start block already exists");
    if (p.final_price > p.initial_price) throw StateError("Invalid tx; Final price cannot be greater than initial price");
    if (p.duration == 0) throw StateError("Invalid tx; Auction duration cannot be `0` blocks");
    if (p.duration == 1 && p.final_price != p.initial_price) throw StateError("Invalid tx; Final price must equal initial price for an auction of duration `1`");
    Hash32 txid = ft.tx->txid();
    AuctionState a;
    a.start_block = p.start_block;
    a.most_recent_bid_block = TxHistory<uint32_t>::make(p.start_block, txid, height);
    a.duration = p.duration;
    a.base_asset = p.base_asset;
    a.initial_base_amount = p.base_amount;
    a.base_amount_remaining = TxHistory<uint64_t>::make(p.base_amount, txid, height);
    a.quote_asset = p.quote_asset;
    a.quote_amount = TxHistory<uint64_t>::make(0, txid, height);
    a.initial_price = p.initial_price;
    a.price_after_most_recent_bid = TxHistory<uint64_t>::make(p.initial_price, txid, height);
    a.initial_end_price = p.final_price;
    a.end_price_after_most_recent_bid = TxHistory<uint64_t>::make(p.final_price, txid, height);
    put_auction(t, txid, a);
}

void State::revert_create(Txn& t, const FilledTransaction& ft) const
{
    Hash32 id = ft.tx->txid();
    if (!t.del(T_DUTCH_AUCTIONS, id.span())) throw StateError("missing Dutch auction " + bt::hex_encode(id));
}

void State::apply_collect(Txn& t, const FilledTransaction& ft, uint32_t height) const
{
    auto c = ft.dutch_auction_collect();
    if (!c) throw StateError("Invalid TxData");
    auto a = auction(t, c->auction_id);
    if (!a) throw StateError("Auction not found");
    if (a->base_asset != c->asset_offered) throw StateError("Incorrect offered asset");
    if (a->quote_asset != c->asset_receive) throw StateError("Incorrect receive asset specified");
    uint64_t end = uint64_t(a->start_block) + a->duration;
    uint32_t end32 = end > UINT32_MAX ? UINT32_MAX : uint32_t(end); // saturating_add
    if (height < end32) throw StateError("Auction has not ended yet");
    if (c->amount_offered_remaining != a->base_amount_remaining.latest().data) throw StateError("Incorrect offered asset amount");
    if (c->amount_received != a->quote_amount.latest().data) throw StateError("Incorrect receive asset amount");
    Hash32 txid = ft.tx->txid();
    a->base_amount_remaining.push(0, txid, height);
    a->quote_amount.push(0, txid, height);
    put_auction(t, c->auction_id, *a);
}

void State::revert_collect(Txn& t, const FilledTransaction& ft) const
{
    auto c = ft.dutch_auction_collect();
    if (!c) throw StateError("Invalid TxData");
    Hash32 txid = ft.tx->txid();
    auto a = auction(t, c->auction_id);
    if (!a) throw StateError("Failed to revert Dutch Auction collect");
    auto p = a->base_amount_remaining.pop();
    if (!p || p->txid != txid || p->data != 0) throw StateError("revert_collect: assert! base_amount_remaining", true);
    if (a->base_amount_remaining.latest().data != c->amount_offered_remaining) throw StateError("revert_collect: assert_eq! base_amount_remaining", true);
    auto q = a->quote_amount.pop();
    if (!q || q->txid != txid || q->data != 0) throw StateError("revert_collect: assert! quote_amount", true);
    if (a->quote_amount.latest().data != c->amount_received) throw StateError("revert_collect: assert_eq! quote_amount", true);
    put_auction(t, c->auction_id, *a);
}

// ------------------------------------------------------------------ transactions (mod.rs:403-701)
void State::validate_reservations(const FilledTransaction& ft)
{
    size_t in = 0, out = 0;
    for (auto& u : ft.spent_utxos)
        if (u.content.is_reservation()) in++;
    for (auto& o : ft.tx->outputs)
        if (o.content.kind == Content::BitAssetReservation) out++;
    if (ft.tx->is(TxData::BitAssetReservation)) {
        if (out == in + 1) return;
    } else if (ft.tx->is(TxData::BitAssetRegistration)) {
        if (in == out + 1) return;
    } else if (in == out) {
        return;
    }
    throw StateError("unbalanced reservations: " + std::to_string(in) + " reservation inputs, " + std::to_string(out) + " reservation outputs");
}

// mod.rs:491-653
void State::validate_bitassets(const Txn& t, const FilledTransaction& ft) const
{
    std::set<Hash32> unique_in;
    size_t control_in = 0;
    for (auto& u : ft.spent_utxos) {
        if (u.content.is_bitasset()) unique_in.insert(u.content.id);
        if (u.content.is_control()) control_in++;
    }
    size_t n_unique_bitasset_inputs = unique_in.size();
    size_t n_bitasset_outputs = 0, n_control_outputs = 0;
    for (auto& o : ft.tx->outputs) {
        if (o.content.kind == Content::BitAsset) n_bitasset_outputs++;
        if (o.content.kind == Content::BitAssetControl) n_control_outputs++;
    }
    // as theirs (consensus: do not change)
    size_t n_unique_bitasset_outputs = ft.unique_spent_bitassets().size();
    const Transaction& tx = *ft.tx;
    if (tx.is(TxData::BitAssetUpdate) && (control_in < 1 || n_control_outputs < 1)) throw StateError("no BitAssets to update");
    if (tx.is(TxData::AmmBurn) &&
        (n_unique_bitasset_outputs < 2 || n_unique_bitasset_inputs > n_unique_bitasset_outputs || n_unique_bitasset_outputs > n_unique_bitasset_inputs + 2))
        throw StateError("Invalid AMM burn");
    if (tx.is(TxData::AmmMint) &&
        (n_unique_bitasset_inputs < 2 || n_unique_bitasset_outputs > n_unique_bitasset_inputs || n_unique_bitasset_inputs > n_unique_bitasset_outputs + 2))
        throw StateError("Too few BitAssets to mint an AMM position");
    if (tx.is(TxData::AmmSwap) || tx.is(TxData::DutchAuctionBid)) {
        size_t lo = n_unique_bitasset_inputs == 0 ? 0 : n_unique_bitasset_inputs - 1, hi = n_unique_bitasset_inputs + 1;
        if (n_unique_bitasset_inputs < 1 || n_unique_bitasset_outputs < lo || n_unique_bitasset_outputs > hi) throw StateError("Invalid Dutch auction bid");
    }
    if (tx.is(TxData::DutchAuctionCreate) &&
        (n_unique_bitasset_inputs < 1 || n_unique_bitasset_outputs > n_unique_bitasset_inputs || n_unique_bitasset_inputs > n_unique_bitasset_outputs + 1))
        throw StateError("Too few BitAssets to create a Dutch auction");
    if (tx.is(TxData::DutchAuctionCollect) &&
        (n_unique_bitasset_outputs < 1 || n_unique_bitasset_inputs > n_unique_bitasset_outputs || n_unique_bitasset_outputs > n_unique_bitasset_inputs + 2))
        throw StateError("Invalid Dutch auction collect");
    if (tx.is(TxData::BitAssetRegistration)) {
        const TxData& d = *tx.data;
        if (n_control_outputs != control_in + 1) throw StateError("unbalanced BitAsset controls");
        if (tx.outputs.empty() || tx.outputs.back().content.kind != Content::BitAssetControl) throw StateError("last output is not a BitAsset control coin");
        if (d.initial_supply == 0) {
            if (n_bitasset_outputs < n_unique_bitasset_inputs) throw StateError("unbalanced BitAssets");
        } else {
            if (n_bitasset_outputs < n_unique_bitasset_inputs + 1) throw StateError("unbalanced BitAssets");
            // outputs.get(outputs.len() - 2): len >= 2 here (a BitAsset and the last control)
            if (tx.outputs.size() < 2 || tx.outputs[tx.outputs.size() - 2].content.kind != Content::BitAsset) throw StateError("second-to-last output is not a BitAsset");
        }
        Hash32 implied = bt::sidechain::keyed_hash(d.revealed_nonce, d.name_hash.span());
        bool burns = false;
        for (auto& u : ft.spent_utxos)
            if (u.content.is_reservation() && u.content.commitment == implied) burns = true;
        if (!burns) throw StateError("no reservation for registration of BitAsset " + bt::hex_encode(d.name_hash));
        if (bitasset(t, d.name_hash)) throw StateError("bitasset " + bt::hex_encode(d.name_hash) + " already registered");
        return;
    }
    if (n_control_outputs != control_in) throw StateError("unbalanced BitAsset controls");
    if (n_bitasset_outputs < n_unique_bitasset_inputs) throw StateError("unbalanced BitAssets");
    if (n_unique_bitasset_inputs == 0 && n_bitasset_outputs != 0) throw StateError("unbalanced BitAssets");
}

uint64_t State::validate_filled(const Txn& t, const FilledTransaction& ft) const
{
    validate_reservations(ft);
    validate_bitassets(t, ft);
    auto fee = ft.bitcoin_fee();
    if (!fee) throw StateError("bitcoin fee: value in overflow, value out overflow, or underfunded");
    for (size_t i = 0; i < ft.spent_utxos.size(); i++)
        if (ft.spent_utxos[i].content.is_withdrawal()) throw StateError("cannot spend withdrawal output " + ft.tx->inputs[i].to_string());
    return *fee;
}

uint64_t State::validate_transaction(const Txn& t, const AuthorizedTransaction& tx) const
{
    FilledTransaction ft = fill(t, tx.transaction);
    for (size_t i = 0; i < tx.authorizations.size() && i < ft.spent_utxos.size(); i++)
        if (bt::sidechain::address_from_vk(tx.authorizations[i].verifying_key) != ft.spent_utxos[i].address) throw StateError("wrong public key for address");
    std::string why;
    if (!verify_authorized_transaction(tx, &why)) throw StateError("failed to verify authorization: " + why);
    return validate_filled(t, ft);
}

// ------------------------------------------------------------------ blocks (block.rs:103-201)
PrevalidatedBlock State::prevalidate(const Txn& t, const Header& h, const Body& body) const
{
    if (borsh_bytes(body).size() > Body::MAX_SIZE) throw StateError("body too large");
    if (h.prev_side_hash != tip(t)) throw StateError("invalid header: prev side hash");
    PrevalidatedBlock pre;
    pre.filled.reserve(body.transactions.size());
    for (auto& tx : body.transactions) pre.filled.push_back(fill(t, tx));
    auto mr = compute_merkle_root(body.coinbase, pre.filled);
    if (!mr) throw StateError("failed to compute the merkle root");
    if (*mr != h.merkle_root) throw StateError("invalid body: merkle root");
    uint64_t cb_value = 0;
    for (auto& o : body.coinbase.outputs) {
        if (cb_value + o.bitcoin_value() < cb_value) throw StateError("Bitcoin amount overflow");
        cb_value += o.bitcoin_value();
    }
    std::vector<std::array<uint8_t, 37>> keys;
    for (auto& tx : body.transactions)
        for (auto& op : tx.inputs) keys.push_back(op.key());
    std::sort(keys.begin(), keys.end());
    for (size_t i = 1; i < keys.size(); i++)
        if (keys[i] == keys[i - 1]) throw StateError("utxo double spent");
    uint64_t total_fees = 0;
    for (auto& ft : pre.filled) {
        uint64_t fee = validate_filled(t, ft);
        if (total_fees + fee < total_fees) throw StateError("Bitcoin amount overflow");
        total_fees += fee;
    }
    if (cb_value > total_fees) throw StateError("not enough fees");
    // authorizations zipped with the spent outputs (stops at the shorter), then verified
    size_t k = 0;
    for (auto& ft : pre.filled)
        for (auto& u : ft.spent_utxos) {
            if (k >= body.authorizations.size()) break;
            if (bt::sidechain::address_from_vk(body.authorizations[k].verifying_key) != u.address) throw StateError("wrong public key for address");
            k++;
        }
    std::string why;
    if (!verify_authorizations(body, &why)) throw StateError("failed to verify authorization: " + why);
    auto ht = height(t);
    pre.next_height = ht ? *ht + 1 : 0;
    pre.merkle_root = *mr;
    pre.total_fees = total_fees;
    pre.coinbase_value = cb_value;
    return pre;
}

void State::apply_tx_data(Txn& t, const FilledTransaction& ft, uint32_t height) const
{
    const Transaction& tx = *ft.tx;
    if (!tx.data) return;
    switch (tx.data->kind) {
    case TxData::AmmBurn: apply_burn(t, ft); break;
    case TxData::AmmMint: apply_amm_mint(t, ft); break;
    case TxData::AmmSwap: apply_swap(t, ft); break;
    case TxData::BitAssetReservation: t.put(T_RESERVATIONS, tx.txid().span(), tx.data->commitment.span()); break;
    case TxData::BitAssetRegistration: apply_registration(t, ft, height); break;
    case TxData::BitAssetMint: apply_mint(t, ft, height); break;
    case TxData::BitAssetUpdate: apply_updates(t, ft, height); break;
    case TxData::DutchAuctionBid: apply_bid(t, ft, height); break;
    case TxData::DutchAuctionCreate: apply_create(t, ft, height); break;
    case TxData::DutchAuctionCollect: apply_collect(t, ft, height); break;
    }
}

void State::template_check(Txn& t, const AuthorizedTransaction& at, uint32_t height) const
{
    FilledTransaction ft = fill(t, at.transaction);
    if (!ft.filled_outputs()) throw StateError("failed to fill tx output contents");
    apply_tx_data(t, ft, height);
}

// block.rs:203-400 connect_prevalidated: UTXO writes are collected and applied after all
// transactions (deletes, then STXOs, then new UTXOs, each in key order); the BitAssets, AMM and
// auction tables are written in transaction order as they go.
void State::connect_prevalidated(Txn& t, const Header& h, const Body& body, const PrevalidatedBlock& pre) const
{
    uint32_t height = pre.next_height;
    std::vector<std::array<uint8_t, 37>> utxo_deletes;
    std::vector<std::pair<std::array<uint8_t, 37>, SpentOutput>> stxo_puts;
    std::vector<std::pair<std::array<uint8_t, 37>, FilledOutput>> utxo_puts;
    Hash32 cb_txid = h.coinbase_txid();
    for (uint32_t vout = 0; vout < body.coinbase.outputs.size(); vout++) {
        auto& o = body.coinbase.outputs[vout];
        FilledOutput f;
        f.address = o.address;
        f.memo = o.memo;
        if (o.content.kind == Content::Bitcoin) {
            f.content.kind = FilledContent::Bitcoin;
            f.content.value = o.content.value;
        } else if (o.content.kind == Content::Withdrawal) {
            f.content.kind = FilledContent::BitcoinWithdrawal;
            f.content.value = o.content.value;
            f.content.main_fee = o.content.main_fee;
            f.content.main_address = o.content.main_address;
        } else {
            throw StateError("bad coinbase output content"); // only checked at connect
        }
        utxo_puts.emplace_back(OutPoint{OutPoint::Coinbase, cb_txid, vout}.key(), f);
    }
    for (size_t i = 0; i < body.transactions.size(); i++) {
        const FilledTransaction& ft = pre.filled.at(i);
        const Transaction& tx = *ft.tx;
        Hash32 txid = tx.txid();
        for (uint32_t vin = 0; vin < tx.inputs.size(); vin++) {
            auto key = tx.inputs[vin].key();
            utxo_deletes.push_back(key);
            stxo_puts.emplace_back(key, SpentOutput{ft.spent_utxos[vin], InPoint{InPoint::Regular, txid, vin}});
        }
        auto outs = ft.filled_outputs();
        if (!outs) throw StateError("failed to fill tx output contents"); // FillTxOutputContents
        for (uint32_t vout = 0; vout < outs->size(); vout++) utxo_puts.emplace_back(OutPoint{OutPoint::Regular, txid, vout}.key(), (*outs)[vout]);
        apply_tx_data(t, ft, height);
    }
    // par_sort_unstable(_by_key): equal keys only come from identical replays (same values)
    std::sort(utxo_deletes.begin(), utxo_deletes.end());
    std::stable_sort(stxo_puts.begin(), stxo_puts.end(), [](auto& a, auto& b) { return a.first < b.first; });
    std::stable_sort(utxo_puts.begin(), utxo_puts.end(), [](auto& a, auto& b) { return a.first < b.first; });
    for (auto& k : utxo_deletes) t.del(T_UTXOS, ByteSpan(k.data(), k.size()));
    for (auto& [k, s] : stxo_puts) {
        Writer w;
        enc_spent(w, s);
        t.put(T_STXOS, ByteSpan(k.data(), k.size()), w.buf);
    }
    for (auto& [k, o] : utxo_puts) {
        Writer w;
        bincode(w, o);
        t.put(T_UTXOS, ByteSpan(k.data(), k.size()), w.buf);
    }
    set_tip(t, h.hash(), height);
}

void State::revert_tx_data(Txn& t, const FilledTransaction& ft, uint32_t height) const
{
    const Transaction& tx = *ft.tx;
    if (!tx.data) return;
    switch (tx.data->kind) {
    case TxData::AmmBurn: revert_burn(t, ft); break;
    case TxData::AmmMint: revert_amm_mint(t, ft); break;
    case TxData::AmmSwap: revert_swap(t, ft); break;
    case TxData::BitAssetMint: revert_mint(t, ft); break;
    case TxData::BitAssetRegistration: revert_registration(t, ft); break;
    case TxData::BitAssetReservation: {
        Hash32 txid = tx.txid();
        if (!t.del(T_RESERVATIONS, txid.span())) throw StateError("missing BitAsset reservation " + bt::hex_encode(txid));
        break;
    }
    case TxData::BitAssetUpdate: revert_updates(t, ft, height); break;
    case TxData::DutchAuctionBid: revert_bid(t, ft); break;
    case TxData::DutchAuctionCollect: revert_collect(t, ft); break;
    case TxData::DutchAuctionCreate: revert_create(t, ft); break;
    }
}

// block.rs:570-736
void State::disconnect_tip(Txn& t, const Header& h, const Body& body) const
{
    auto tip_hash = tip(t);
    if (!tip_hash) throw StateError("no tip");
    if (*tip_hash != h.hash()) throw StateError("invalid header: block hash");
    std::vector<FilledTransaction> filled;
    filled.reserve(body.transactions.size());
    for (auto& tx : body.transactions) filled.push_back(fill_from_stxos(t, tx));
    auto mr = compute_merkle_root(body.coinbase, filled);
    if (!mr) throw StateError("failed to compute the merkle root");
    if (*mr != h.merkle_root) throw StateError("invalid body: merkle root");
    auto ht = height(t);
    if (!ht) throw StateError("Height should not be None", true);
    uint32_t height_ = *ht;
    for (size_t i = body.transactions.size(); i-- > 0;) {
        const Transaction& tx = body.transactions[i];
        Hash32 txid = tx.txid();
        FilledTransaction ft = fill_from_stxos(t, tx);
        revert_tx_data(t, ft, height_);
        for (int64_t vout = int64_t(tx.outputs.size()) - 1; vout >= 0; vout--) {
            OutPoint op{OutPoint::Regular, txid, uint32_t(vout)};
            if (!del_utxo(t, op)) throw StateError("utxo " + op.to_string() + " doesn't exist");
        }
        for (auto in = tx.inputs.rbegin(); in != tx.inputs.rend(); ++in) {
            auto s = stxo(t, *in);
            if (!s) throw StateError("no stxo " + in->to_string());
            del_stxo(t, *in);
            put_utxo(t, *in, s->output);
        }
    }
    Hash32 cb_txid = h.coinbase_txid();
    for (int64_t vout = int64_t(body.coinbase.outputs.size()) - 1; vout >= 0; vout--) {
        OutPoint op{OutPoint::Coinbase, cb_txid, uint32_t(vout)};
        if (!del_utxo(t, op)) throw StateError("utxo " + op.to_string() + " doesn't exist");
    }
    if (!h.prev_side_hash && height_ == 0) {
        clear_tip(t);
    } else if (!h.prev_side_hash || height_ == 0) {
        throw StateError("no tip");
    } else {
        set_tip(t, *h.prev_side_hash, height_ - 1);
    }
}

} // namespace bitassets
