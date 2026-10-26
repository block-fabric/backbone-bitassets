// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// JSON forms of BitAssets types exactly as serde_json renders them in the fork's JSON-RPC
// (human-readable serde): hex hashes (no prefix) and memos, base58 addresses, Bech32m keys
// ("ba-svk1...", "ba-enc1..."), AssetIds as the hex of their borsh bytes, display-order bitcoin
// hashes, externally tagged enums, [u8; 32] commitments as number arrays where the type has no
// hex adapter, const_hex::serde fields 0x-prefixed, RollBack histories as sequences.
#pragma once

#include <bitassets/state.h>

#include <bt/jsonrpc.h>
#include <univalue.h>

namespace bitassets::json {

UniValue hash(const Hash32& h);     // hex (BlockHash, Txid, MerkleRoot, BitAssetId, ...)
UniValue btc_hash(const Hash32& h); // display order (bitcoin::BlockHash, Txid, M6id)
UniValue bytes_array(ByteSpan b);   // serde of [u8; N]: array of numbers
UniValue address(const Address& a);
UniValue asset_id(const AssetId& a);
UniValue outpoint(const OutPoint& o);
UniValue content(const Content& c);
UniValue filled_content(const FilledContent& c);
UniValue output(const Output& o);
UniValue filled_output(const FilledOutput& o);
UniValue spent_output(const SpentOutput& s);
UniValue inpoint(const InPoint& i);
UniValue pointed(const OutPoint& op, const UniValue& output);
UniValue bitasset_data(const BitAssetData& d);
UniValue updates(const BitAssetDataUpdates& u);
UniValue auction_params(const DutchAuctionParams& p);
UniValue txdata(const TxData& d);
UniValue transaction(const Transaction& t);
UniValue signature(const bt::frost::Signature& s);
UniValue authorization(const Authorization& a);
UniValue authorized_transaction(const AuthorizedTransaction& t);
UniValue coinbase(const Coinbase& c);
UniValue body(const Body& b);
UniValue header(const Header& h);
UniValue block(const Header& h, const Body& b, uint32_t height);
UniValue bitcoin_tx(const std::vector<bt::btc::TxOut>& outs);
UniValue withdrawal_bundle(const WithdrawalBundle& w);
UniValue pool_state(const PoolState& p);
UniValue auction_state(const AuctionState& a);
/** fraction::Fraction (GenericFraction<u64>): {"Rational": ["Plus", [numer, denom]]}, reduced. */
UniValue fraction(uint64_t numer, uint64_t denom);

/** A request value that does not parse: jsonrpsee's -32602 "Invalid params", the cause (serde's) as data;
 *  `at`: the value it is about (the server adds serde_json's position of it). */
struct Error : bt::jsonrpc::RpcError {
    explicit Error(const std::string& cause, const UniValue* at = nullptr) : bt::jsonrpc::RpcError(-32602, "Invalid params", cause, at) {}
};
std::string serde_unexpected(const UniValue& v);
void require_struct(const UniValue& v, const char* name, std::initializer_list<const char*> fields);
Hash32 parse_hash(const UniValue& v);
Hash32 parse_btc_hash(const UniValue& v);
Address parse_address(const UniValue& v);
bt::btc::Address parse_btc_address(const UniValue& v);
AssetId parse_asset_id(const UniValue& v);
OutPoint parse_outpoint(const UniValue& v);
Output parse_output(const UniValue& v);
BitAssetData parse_bitasset_data(const UniValue& v);
DutchAuctionParams parse_auction_params(const UniValue& v);
TxData parse_txdata(const UniValue& v);
Transaction parse_transaction(const UniValue& v);
bt::frost::Signature parse_signature(const UniValue& v);
bt::frost::Point parse_vk(const UniValue& v);
bt::ByteArray<32> parse_epk(const UniValue& v);
AuthorizedTransaction parse_authorized_transaction(const UniValue& v);
Header parse_header(const UniValue& v);
Body parse_body(const UniValue& v);
uint64_t parse_u64(const UniValue& v);
uint32_t parse_u32(const UniValue& v);
Bytes parse_hex(const UniValue& v);

} // namespace bitassets::json
