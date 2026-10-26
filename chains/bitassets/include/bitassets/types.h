// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// BitAssets (eCash betanet slot 4) consensus types, byte-exact with the octobocto fork of
// plain-bitassets at v0.17.1 (16f2f69), which is what the betanet chain runs. Every hash is
// BLAKE3(borsh(x)); the P2P wire uses bincode 1.3 over serde (non-human-readable forms).
// Citations are fork paths at 16f2f69 (lib/types/...).
//
// Fork deltas over upstream 0.16.3 that change bytes: Coinbase{memo, outputs} with its own
// merkle tree and CoinbaseTxid outpoints, a CBMT of per-transaction roots, FROST-ristretto255
// authorizations (message = 0x00 || borsh(tx)), the `s4_<b58>_<hex>` deposit address form.
#pragma once

#include <bt/bitcoin.h>
#include <bt/bytes.h>
#include <bt/codec.h>
#include <bt/crypto.h>
#include <bt/frost.h>
#include <bt/sidechain/peg.h>
#include <bt/sidechain/types.h>

#include <tuple>

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace bitassets {

using bt::append;
using bt::Bytes;
using bt::ByteSpan;
using bt::Hash32;
using bt::Reader;
using bt::Writer;
using bt::sidechain::Address;
using bt::sidechain::InPoint;
using bt::sidechain::OutPoint;
using bt::sidechain::SocketV4;
using bt::sidechain::SocketV6;
using bt::sidechain::StateError;
using bt::sidechain::Update;

constexpr uint8_t THIS_SIDECHAIN = 4; // types/mod.rs:43

/** hashes.rs AssetId = Bitcoin | BitAsset(BitAssetId) | BitAssetControl(BitAssetId).
 *  Ord (derived) = variant index, then the 32 id bytes. Its serde form (JSON and bincode) is
 *  its borsh bytes: hex in JSON, a length-prefixed byte string in bincode. */
struct AssetId {
    enum Kind : uint8_t { Bitcoin = 0, BitAsset = 1, BitAssetControl = 2 } kind = Bitcoin;
    Hash32 id; // zero for Bitcoin
    static AssetId bitcoin() { return AssetId{}; }
    static AssetId bitasset(const Hash32& h) { return AssetId{BitAsset, h}; }
    static AssetId control(const Hash32& h) { return AssetId{BitAssetControl, h}; }
    friend auto operator<=>(const AssetId&, const AssetId&) = default;
    friend bool operator==(const AssetId&, const AssetId&) = default;
    Bytes borsh_bytes() const;
    std::string to_hex() const; // Display / FromStr
    static std::optional<AssetId> from_borsh(ByteSpan b);
};

/** bitasset_data.rs BitAssetData: five optional fields in this order. */
struct BitAssetData {
    std::optional<Hash32> commitment;
    std::optional<SocketV4> socket_addr_v4;
    std::optional<SocketV6> socket_addr_v6;
    std::optional<bt::ByteArray<32>> encryption_pubkey; // x25519
    std::optional<bt::frost::Point> signing_pubkey;     // FROST ristretto255 verifying key
    bool operator==(const BitAssetData&) const = default;
};

struct BitAssetDataUpdates {
    Update<Hash32> commitment;
    Update<SocketV4> socket_addr_v4;
    Update<SocketV6> socket_addr_v6;
    Update<bt::ByteArray<32>> encryption_pubkey;
    Update<bt::frost::Point> signing_pubkey;
};

/** transaction/mod.rs DutchAuctionParams */
struct DutchAuctionParams {
    uint32_t start_block = 0;
    uint32_t duration = 0;
    AssetId base_asset;
    uint64_t base_amount = 0;
    AssetId quote_asset;
    uint64_t initial_price = 0;
    uint64_t final_price = 0;
};

/** transaction/mod.rs TransactionData (variant order = borsh/bincode tag). */
struct TxData {
    enum Kind : uint8_t {
        AmmBurn = 0, AmmMint = 1, AmmSwap = 2, BitAssetReservation = 3, BitAssetRegistration = 4, BitAssetMint = 5, BitAssetUpdate = 6,
        DutchAuctionCreate = 7, DutchAuctionBid = 8, DutchAuctionCollect = 9
    } kind = AmmBurn;
    // AmmBurn {amount0, amount1, lp_token_burn} / AmmMint {amount0, amount1, lp_token_mint}
    uint64_t amount0 = 0, amount1 = 0, lp_tokens = 0;
    // AmmSwap {amount_spent, amount_receive, pair_asset}
    uint64_t amount_spent = 0, amount_receive = 0;
    AssetId pair_asset;
    // BitAssetReservation {commitment}
    Hash32 commitment;
    // BitAssetRegistration {name_hash, revealed_nonce, bitasset_data, initial_supply}
    Hash32 name_hash, revealed_nonce;
    BitAssetData bitasset_data;
    uint64_t initial_supply = 0;
    // BitAssetMint(u64)
    uint64_t mint_amount = 0;
    // BitAssetUpdate(Box<BitAssetDataUpdates>)
    BitAssetDataUpdates updates;
    // DutchAuctionCreate(DutchAuctionParams)
    DutchAuctionParams params;
    // DutchAuctionBid {auction_id, receive_asset, quantity, bid_size}
    Hash32 auction_id;
    AssetId receive_asset;
    uint64_t quantity = 0, bid_size = 0;
    // DutchAuctionCollect {asset_offered, asset_receive, amount_offered_remaining, amount_received}
    AssetId asset_offered, asset_receive;
    uint64_t amount_offered_remaining = 0, amount_received = 0;
};

/** transaction/output.rs Content (variant order = borsh/bincode tag). */
struct Content {
    enum Kind : uint8_t { AmmLpToken = 0, Bitcoin = 1, BitAsset = 2, BitAssetControl = 3, BitAssetReservation = 4, DutchAuctionReceipt = 5, Withdrawal = 6 } kind =
        Bitcoin;
    uint64_t value = 0;            // AmmLpToken amount / Bitcoin sats / BitAsset amount / Withdrawal value
    uint64_t main_fee = 0;         // Withdrawal
    bt::btc::Address main_address; // Withdrawal
    /** GetBitcoinValue: Bitcoin value; Withdrawal value + main_fee saturating; else 0. */
    uint64_t bitcoin_value() const;
    bool operator==(const Content& o) const;
};

struct Output {
    Address address;
    Content content;
    Bytes memo;
    uint64_t bitcoin_value() const { return content.bitcoin_value(); }
    bool operator==(const Output&) const = default;
};

/** transaction/output.rs FilledContent (serde variant order = bincode tag; no borsh). */
struct FilledContent {
    enum Kind : uint8_t {
        AmmLpToken = 0, Bitcoin = 1, BitcoinWithdrawal = 2, BitAsset = 3, BitAssetControl = 4, BitAssetReservation = 5, DutchAuctionReceipt = 6
    } kind = Bitcoin;
    AssetId asset0, asset1;        // AmmLpToken
    uint64_t value = 0;            // AmmLpToken amount / Bitcoin / BitAsset amount / withdrawal value
    uint64_t main_fee = 0;         // BitcoinWithdrawal
    bt::btc::Address main_address; // BitcoinWithdrawal
    Hash32 id;                     // BitAsset / Control: BitAssetId; Reservation: txid; Receipt: auction id
    Hash32 commitment;             // BitAssetReservation
    Content to_content() const;
    uint64_t bitcoin_value() const { return to_content().bitcoin_value(); }
    bool is_withdrawal() const { return kind == BitcoinWithdrawal; }
    bool is_bitasset() const { return kind == BitAsset; }
    bool is_control() const { return kind == BitAssetControl; }
    bool is_bitcoin() const { return kind == Bitcoin; }
    bool is_reservation() const { return kind == BitAssetReservation; }
    bool is_receipt() const { return kind == DutchAuctionReceipt; }
    bool is_lp_token() const { return kind == AmmLpToken; }
    /** asset_value: BitAsset (id, v), control (id, 1), Bitcoin (sats); else nullopt. */
    std::optional<std::pair<AssetId, uint64_t>> asset_value() const;
    bool operator==(const FilledContent& o) const;
};

struct FilledOutput {
    Address address;
    FilledContent content;
    Bytes memo;
    uint64_t bitcoin_value() const { return content.bitcoin_value(); }
    Output to_output() const { return Output{address, content.to_content(), memo}; }
    bool operator==(const FilledOutput&) const = default;
};

using SpentOutput = bt::sidechain::SpentOutputT<FilledOutput>;

struct Transaction {
    std::vector<OutPoint> inputs;
    std::vector<Output> outputs;
    Bytes memo;
    std::optional<TxData> data;

    Bytes canonical_bytes() const; // borsh
    uint64_t canonical_size() const { return canonical_bytes().size(); }
    Hash32 txid() const;
    bool is(TxData::Kind k) const { return data && data->kind == k; }
    /** blake3::keyed_hash(revealed_nonce, name_hash) for registrations. */
    std::optional<Hash32> implied_reservation_commitment() const;
    /** compute_merkle_root (transaction/mod.rs:724-737): blake3(inputs_root || outputs_root);
     *  nullopt when the outputs tree fails (value or size overflow). */
    std::optional<Hash32> merkle_root() const;
};

struct Authorization {
    bt::frost::Point verifying_key;
    bt::frost::Signature signature;
};

struct AuthorizedTransaction {
    Transaction transaction;
    std::vector<Authorization> authorizations;
};

struct Coinbase {
    Bytes memo;
    std::vector<Output> outputs;
};

struct Body {
    Coinbase coinbase;
    std::vector<Transaction> transactions;
    std::vector<Authorization> authorizations;
    size_t inputs_len() const;
    /** Body::authorized_transactions: authorizations dealt out by input count. */
    std::vector<AuthorizedTransaction> authorized_transactions() const;
    static Body from_authorized(std::vector<AuthorizedTransaction> txs, Coinbase cb);
    static constexpr size_t MAX_SIZE = 8 * 1024 * 1024;
};

/** types/mod.rs:163-169 */
struct Header {
    Hash32 merkle_root;
    std::optional<Hash32> prev_side_hash;
    Hash32 prev_main_hash; // bitcoin block hash, internal byte order

    Hash32 hash() const;
    Hash32 coinbase_txid() const;
    bool operator==(const Header&) const = default;
};

struct Block {
    Header header;
    Body body;
};

/** AMM and auction operations as FilledTransaction derives them (transaction/mod.rs:499-558). */
struct AmmBurnOp {
    AssetId asset0, asset1;
    uint64_t amount0, amount1, lp_token_burn;
};
struct AmmMintOp {
    AssetId asset0, asset1;
    uint64_t amount0, amount1, lp_token_mint;
};
struct AmmSwapOp {
    AssetId asset_spend, asset_receive;
    uint64_t amount_spend, amount_receive;
};
struct AuctionBidOp {
    Hash32 auction_id;
    AssetId asset_spend, asset_receive;
    uint64_t amount_spend, amount_receive;
};
struct AuctionCollectOp {
    Hash32 auction_id;
    AssetId asset_offered, asset_receive;
    uint64_t amount_offered_remaining, amount_received;
};

/** A transaction together with the outputs it spends (transaction/mod.rs FilledTransaction). */
struct FilledTransaction {
    const Transaction* tx = nullptr;
    std::vector<FilledOutput> spent_utxos;

    /** bitcoin_fee: checked sums of Bitcoin value in and out (withdrawals count value + fee);
     *  nullopt on overflow or underfunding. AMM and auction Bitcoin flows are ignored (B4). */
    std::optional<uint64_t> bitcoin_fee() const;
    /** unique_spent_assets / _bitassets / _lp_tokens: first-occurrence order, combined values
     *  summed with u64 `+=`, as theirs. */
    std::vector<std::pair<AssetId, uint64_t>> unique_spent_assets() const;
    std::vector<std::pair<Hash32, uint64_t>> unique_spent_bitassets() const;
    std::vector<std::tuple<AssetId, AssetId, uint64_t>> unique_spent_lp_tokens() const;

    std::optional<AmmBurnOp> amm_burn() const;
    std::optional<AmmMintOp> amm_mint() const;
    std::optional<AmmSwapOp> amm_swap() const;
    std::optional<std::pair<Hash32, uint64_t>> bitasset_mint() const;
    std::optional<AuctionBidOp> dutch_auction_bid() const;
    std::optional<DutchAuctionParams> dutch_auction_create() const;
    std::optional<AuctionCollectOp> dutch_auction_collect() const;

    /** filled_outputs (transaction/mod.rs:1578-1666); nullopt if the outputs cannot be filled. */
    std::optional<std::vector<FilledOutput>> filled_outputs() const;

private:
    std::vector<std::pair<AssetId, std::optional<uint64_t>>> output_asset_total_values() const;
};

// ---------------------------------------------------------------- borsh
void borsh(Writer& w, const AssetId& a);
void borsh(Writer& w, const Content& c);
void borsh(Writer& w, const Output& o);
void borsh(Writer& w, const BitAssetData& d);
void borsh(Writer& w, const BitAssetDataUpdates& u);
void borsh(Writer& w, const DutchAuctionParams& p);
void borsh(Writer& w, const TxData& d);
void borsh(Writer& w, const Transaction& t);
void borsh(Writer& w, const Authorization& a);
void borsh(Writer& w, const Coinbase& c);
void borsh(Writer& w, const Body& b);
void borsh(Writer& w, const Header& h);
template <class T>
Bytes borsh_bytes(const T& x)
{
    Writer w;
    borsh(w, x);
    return w.buf;
}
template <class T>
Hash32 hash(const T& x)
{
    return bt::blake3(borsh_bytes(x));
}

/** borsh decoders (the seed index serves txs as borsh hex; wallet tests). */
Transaction borsh_de_transaction(Reader& r);
AssetId borsh_de_asset_id(Reader& r);

// ---------------------------------------------------------------- bincode (serde)
void bincode(Writer& w, const AssetId& a);
void bincode(Writer& w, const Content& c);
void bincode(Writer& w, const Output& o);
void bincode(Writer& w, const BitAssetData& d);
void bincode(Writer& w, const BitAssetDataUpdates& u);
void bincode(Writer& w, const DutchAuctionParams& p);
void bincode(Writer& w, const TxData& d);
void bincode(Writer& w, const Transaction& t);
void bincode(Writer& w, const Authorization& a);
void bincode(Writer& w, const AuthorizedTransaction& t);
void bincode(Writer& w, const Coinbase& c);
void bincode(Writer& w, const Body& b);
void bincode(Writer& w, const Header& h);
void bincode(Writer& w, const FilledContent& c);
void bincode(Writer& w, const FilledOutput& o);

AssetId de_asset_id(Reader& r);
Content de_content(Reader& r);
Output de_output(Reader& r);
BitAssetData de_bitasset_data(Reader& r);
TxData de_txdata(Reader& r);
Transaction de_transaction(Reader& r);
Authorization de_authorization(Reader& r);
AuthorizedTransaction de_authorized_transaction(Reader& r);
Coinbase de_coinbase(Reader& r);
Body de_body(Reader& r);
Header de_header(Reader& r);
FilledContent de_filled_content(Reader& r);
FilledOutput de_filled_output(Reader& r);

// ---------------------------------------------------------------- merkle (types/mod.rs:547-849)
/** Outputs::compute_merkle_root: value/size-annotated CBMT; nullopt on a checked-add overflow. */
std::optional<Hash32> outputs_merkle_root(const std::vector<Output>& outs);
/** Inputs::compute_merkle_root: plain CBMT of blake3(borsh(outpoint)). */
Hash32 inputs_merkle_root(const std::vector<OutPoint>& ins);
/** Coinbase::compute_merkle_root = blake3(borsh((memo, outputs_root))). */
std::optional<Hash32> coinbase_merkle_root(const Coinbase& cb);
/** Body::compute_merkle_root: blake3(coinbase_root || txs_commitment), txs_commitment the
 *  fee/size CBMT of blake3(borsh(fee, canonical_size, tx_merkle_root)) leaves (no 1000-tx switch
 *  in this fork); nullopt where theirs errs (block invalid). */
std::optional<Hash32> compute_merkle_root(const Coinbase& cb, const std::vector<FilledTransaction>& txs);
/** Coinbase::compute_txid = blake3(borsh{merkle_root, prev_main_hash, prev_side_hash}). */
Hash32 compute_coinbase_txid(const Hash32& merkle_root, const Hash32& prev_main_hash, const std::optional<Hash32>& prev_side);

// ---------------------------------------------------------------- authorizations (authorization.rs)
/** Dst::Transaction (0) prefixed message the signatures cover. */
Bytes tx_signing_message(const Transaction& t);
/** verify_authorized_transaction: exact count; zero inputs pass without signatures. */
bool verify_authorized_transaction(const AuthorizedTransaction& tx, std::string* why = nullptr);
/** verify_authorizations: exact count Σinputs, every signature over its tx message. */
bool verify_authorizations(const Body& body, std::string* why = nullptr);
/** authorization::verify with a domain tag (0 = Transaction, 255 = Arbitrary). */
bool verify_with_dst(const bt::frost::Signature& sig, const bt::frost::Point& vk, uint8_t dst, ByteSpan msg);
bt::frost::Signature sign_with_dst(const bt::frost::SigningKey& sk, uint8_t dst, ByteSpan msg);

/** Key strings (keys.rs): "ba-svk" verifying keys, "ba-enc" encryption keys. */
std::string vk_to_string(const bt::frost::Point& vk);
std::optional<bt::frost::Point> vk_from_string(std::string_view s);
std::string epk_to_string(const bt::ByteArray<32>& k);
std::optional<bt::ByteArray<32>> epk_from_string(std::string_view s);

/** blake3(plain name), the BitAsset id wallets use. */
Hash32 name_hash(std::string_view plain);

} // namespace bitassets
