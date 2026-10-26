// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// BitAssets state machine: a literal port of the plain-bitassets fork v0.17.1
// lib/state/{mod,block,bitassets,amm,dutch_auction}.rs over our ordered KV store. The UTXO
// store and the two-way peg are the core's PegState (identical in the BitNames and BitAssets
// forks). Key encodings mirror their LMDB databases where the order is observable:
// bitasset_seq_to_bitasset is keyed by bincode SeqId = u32 LITTLE-endian under memcmp order,
// so `last()` (next sequence number) misbehaves after 256 registrations exactly as theirs.
#pragma once

#include <bitassets/types.h>

#include <bt/kv.h>
#include <bt/mainchain.h>
#include <bt/sidechain/peg.h>
#include <bt/sidechain/rollback.h>

namespace bitassets {

using bt::sidechain::HeightStamped;
using bt::sidechain::RollBack;
using bt::sidechain::TxHistory;

struct StateTraits {
    using FilledOutput = bitassets::FilledOutput;
    static void enc_filled(Writer& w, const FilledOutput& o) { bincode(w, o); }
    static FilledOutput dec_filled(Reader& r) { return de_filled_output(r); }
    static uint64_t bitcoin_value(const FilledOutput& o) { return o.bitcoin_value(); }
    static bool is_withdrawal(const FilledOutput& o) { return o.content.is_withdrawal(); }
    static uint64_t withdrawal_value(const FilledOutput& o) { return o.content.value; }
    static uint64_t withdrawal_main_fee(const FilledOutput& o) { return o.content.main_fee; }
    static const bt::btc::Address& withdrawal_address(const FilledOutput& o) { return o.content.main_address; }
    /** proto.rs deposit: Bitcoin output at the `s4_` / base58 address, else ALL_ZEROS. */
    static FilledOutput deposit_output(const bt::mainchain::Deposit& d);
};

using WithdrawalBundle = bt::sidechain::WithdrawalBundleT<FilledOutput>;
using WithdrawalBundleInfo = bt::sidechain::WithdrawalBundleInfoT<FilledOutput>;
using BlockIndexEvents = bt::sidechain::BlockIndexEventsT<FilledOutput>;
using bt::sidechain::BundleStatus;

/** lib/state/bitassets.rs BitAssetData: per-field histories and the total supply. */
struct BitAssetRecord {
    TxHistory<std::optional<Hash32>> commitment;
    TxHistory<std::optional<SocketV4>> socket_addr_v4;
    TxHistory<std::optional<SocketV6>> socket_addr_v6;
    TxHistory<std::optional<bt::ByteArray<32>>> encryption_pubkey;
    TxHistory<std::optional<bt::frost::Point>> signing_pubkey;
    TxHistory<uint64_t> total_supply;

    static BitAssetRecord init(const BitAssetData& d, uint64_t initial_supply, const Hash32& txid, uint32_t height);
    BitAssetData current() const;
    std::optional<BitAssetData> at_block_height(uint32_t h) const;
    void apply_updates(const BitAssetDataUpdates& u, const Hash32& txid, uint32_t height);
    /** revert_updates: their assert!s become panic errors. */
    void revert_updates(const BitAssetDataUpdates& u, const Hash32& txid, uint32_t height);
};

/** lib/state/amm.rs PoolState */
struct PoolState {
    uint64_t reserve0 = 0, reserve1 = 0, outstanding_lp_tokens = 0;
    Hash32 creation_txid;
    bool operator==(const PoolState&) const = default;
    static PoolState fresh(const Hash32& txid) { return PoolState{0, 0, 0, txid}; }
    /** All return nullopt with the error text in `err`; a panic of theirs throws StateError{panic}. */
    std::optional<PoolState> mint(uint64_t amount0, uint64_t amount1, std::string& err) const;
    std::optional<PoolState> revert_mint(uint64_t amount0, uint64_t amount1, uint64_t lp, std::string& err) const;
    std::optional<PoolState> burn(uint64_t lp_token_burn, std::string& err) const;
    std::optional<PoolState> unburn(uint64_t lp, uint64_t payout0, uint64_t payout1, std::string& err) const;
    std::optional<PoolState> swap_asset0_for_asset1(uint64_t amount_spend, std::string& err) const;
    std::optional<PoolState> swap_asset1_for_asset0(uint64_t amount_spend, std::string& err) const;
    std::optional<PoolState> revert_swap(const AmmSwapOp& s, std::string& err) const;
};

/** lib/state/dutch_auction.rs DutchAuctionState */
struct AuctionState {
    uint32_t start_block = 0;
    TxHistory<uint32_t> most_recent_bid_block;
    uint32_t duration = 0;
    AssetId base_asset;
    uint64_t initial_base_amount = 0;
    TxHistory<uint64_t> base_amount_remaining;
    AssetId quote_asset;
    TxHistory<uint64_t> quote_amount;
    uint64_t initial_price = 0;
    TxHistory<uint64_t> price_after_most_recent_bid;
    uint64_t initial_end_price = 0;
    TxHistory<uint64_t> end_price_after_most_recent_bid;
    /** bid(): nullopt + err on an error; their assert! throws StateError{panic}. */
    std::optional<AuctionState> bid(const Hash32& txid, uint64_t bid_amount, uint32_t height, std::string& err) const;
    AuctionState revert_bid(const Hash32& txid) const;
};

struct PrevalidatedBlock {
    std::vector<FilledTransaction> filled; // point into the block body
    Hash32 merkle_root;
    uint64_t total_fees = 0;
    uint64_t coinbase_value = 0;
    uint32_t next_height = 0;
};

class State : public bt::sidechain::PegState<StateTraits> {
public:
    using Txn = bt::kv::Txn;
    enum Table : uint8_t { T_AMM_POOLS = 11, T_DUTCH_AUCTIONS, T_BITASSET_TO_SEQ, T_BITASSETS, T_RESERVATIONS, T_SEQ_TO_BITASSET };

    // ---- BitAssets tables (lib/state/bitassets.rs Dbs)
    std::optional<BitAssetRecord> bitasset(const Txn& t, const Hash32& id) const;
    /** All BitAssets in key (id) order. */
    std::vector<std::pair<Hash32, BitAssetRecord>> bitassets(const Txn& t) const;
    std::optional<uint32_t> bitasset_seq(const Txn& t, const Hash32& id) const;
    std::vector<std::pair<uint32_t, Hash32>> seq_to_bitasset(const Txn& t) const; // memcmp (LE) key order
    std::optional<Hash32> reservation(const Txn& t, const Hash32& txid) const;
    std::vector<std::pair<Hash32, Hash32>> reservations(const Txn& t) const;
    /** try_get_bitasset_data_at_block_height */
    std::optional<BitAssetData> bitasset_at(const Txn& t, const Hash32& id, uint32_t height) const;

    // ---- AMM pools / auctions
    std::optional<PoolState> pool(const Txn& t, const AssetId& a0, const AssetId& a1) const; // AmmPair::new order
    std::vector<std::pair<std::pair<AssetId, AssetId>, PoolState>> pools(const Txn& t) const;
    std::optional<AuctionState> auction(const Txn& t, const Hash32& id) const;
    std::vector<std::pair<Hash32, AuctionState>> auctions(const Txn& t) const;

    // ---- transactions (lib/state/mod.rs)
    FilledTransaction fill(const Txn& t, const Transaction& tx) const { return FilledTransaction{&tx, fill_spent(t, tx)}; }
    FilledTransaction fill_from_stxos(const Txn& t, const Transaction& tx) const { return FilledTransaction{&tx, fill_spent_from_stxos(t, tx)}; }
    static void validate_reservations(const FilledTransaction& ft);
    void validate_bitassets(const Txn& t, const FilledTransaction& ft) const;
    /** validate_filled_transaction: rules above, fee, no withdrawal spends; returns the fee. */
    uint64_t validate_filled(const Txn& t, const FilledTransaction& ft) const;
    /** validate_transaction (mempool / peers / submit_transaction); returns the fee. */
    uint64_t validate_transaction(const Txn& t, const AuthorizedTransaction& tx) const;

    // ---- blocks (lib/state/block.rs)
    PrevalidatedBlock prevalidate(const Txn& t, const Header& h, const Body& b) const;
    void connect_prevalidated(Txn& t, const Header& h, const Body& b, const PrevalidatedBlock& pre) const;
    void disconnect_tip(Txn& t, const Header& h, const Body& b) const;

    static Bytes pair_key(const AssetId& a0, const AssetId& a1);
    /** Node policy hook for templates (never on received blocks): fill the outputs and apply the
     *  AMM / auction / BitAsset effects on the (never committed) template transaction, as connect
     *  would; throws StateError where connect would fail. */
    void template_check(Txn& t, const AuthorizedTransaction& at, uint32_t height) const;

private:
    void put_bitasset(Txn& t, const Hash32& id, const BitAssetRecord& r) const;
    void put_pool(Txn& t, const AssetId& a0, const AssetId& a1, const PoolState& p) const;
    void put_auction(Txn& t, const Hash32& id, const AuctionState& a) const;
    void apply_tx_data(Txn& t, const FilledTransaction& ft, uint32_t height) const;
    void revert_tx_data(Txn& t, const FilledTransaction& ft, uint32_t height) const;
    // bitassets.rs
    void apply_registration(Txn& t, const FilledTransaction& ft, uint32_t height) const;
    void revert_registration(Txn& t, const FilledTransaction& ft) const;
    void apply_mint(Txn& t, const FilledTransaction& ft, uint32_t height) const;
    void revert_mint(Txn& t, const FilledTransaction& ft) const;
    void apply_updates(Txn& t, const FilledTransaction& ft, uint32_t height) const;
    void revert_updates(Txn& t, const FilledTransaction& ft, uint32_t height) const;
    // amm.rs
    void apply_burn(Txn& t, const FilledTransaction& ft) const;
    void revert_burn(Txn& t, const FilledTransaction& ft) const;
    void apply_amm_mint(Txn& t, const FilledTransaction& ft) const;
    void revert_amm_mint(Txn& t, const FilledTransaction& ft) const;
    void apply_swap(Txn& t, const FilledTransaction& ft) const;
    void revert_swap(Txn& t, const FilledTransaction& ft) const;
    // dutch_auction.rs
    void apply_bid(Txn& t, const FilledTransaction& ft, uint32_t height) const;
    void revert_bid(Txn& t, const FilledTransaction& ft) const;
    void apply_create(Txn& t, const FilledTransaction& ft, uint32_t height) const;
    void revert_create(Txn& t, const FilledTransaction& ft) const;
    void apply_collect(Txn& t, const FilledTransaction& ft, uint32_t height) const;
    void revert_collect(Txn& t, const FilledTransaction& ft) const;
};

/** u128 helpers used by the AMM and the auctions (Rust's u128 arithmetic). */
using u128 = unsigned __int128;
/** num::integer::sqrt for u128: floor(sqrt(x)). */
u128 isqrt_u128(u128 x);

} // namespace bitassets
