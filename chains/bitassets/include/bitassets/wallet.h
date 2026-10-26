// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// BitAssets wallet, key-compatible with the plain-bitassets fork v0.17.1 (lib/wallet.rs):
//   seed (BIP-39, tiny-bip39, empty passphrase) -> rust-bitcoin Xpriv::new_master (BIP-32 over
//   secp256k1, "Bitcoin seed"); tx signing keys m/0'/i, encryption keys m/1'/i, message signing
//   keys m/2'/i. A FROST key is the 32 secret bytes read LITTLE-endian mod L
//   (Scalar::from_bytes_mod_order); an x25519 secret is the same 32 bytes. Addresses start at
//   index 0. Reservation nonces are keyed_hash(scalar bytes, name_hash).
// Their LMDB wallet is not read: import by mnemonic (set_seed_from_mnemonic or
// --mnemonic-seed-phrase-path) restores the same keys and addresses.
// Kept fixes (wallet only, no consensus effect): reservations spend one coin, the control-coin
// selection picks the control coin (theirs picks a BitAsset coin), Dutch-auction collects include
// a BitAsset coin, coins already spent by mempool transactions are not selected again.
#pragma once

#include <bitassets/chain.h>

#include <bt/kv.h>
#include <bt/sidechain/node.h>

#include <array>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace bitassets {

using Node = bt::sidechain::Node<Chain>;

struct Balance {
    uint64_t total = 0, available = 0;
};

struct WalletError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class Wallet {
public:
    Wallet(const std::string& path, Node& node);
    bool has_seed();
    /** set_seed: error if a different seed exists. */
    void set_seed(const bt::ByteArray<64>& seed);
    void set_seed_from_mnemonic(const std::string& mnemonic);

    Address get_new_address();
    /** The last address, or a new one once the last has received (get_receive_address). */
    Address get_receive_address();
    std::vector<Address> addresses(); // sorted by base58
    bool is_mine(const Address& a);
    bt::ByteArray<32> get_new_encryption_key();
    bt::frost::Point get_new_verifying_key();

    std::vector<std::pair<OutPoint, FilledOutput>> utxos();
    std::vector<std::pair<OutPoint, SpentOutput>> stxos();
    Balance balance();

    Transaction create_transfer(const Address& dest, uint64_t value, uint64_t fee, const Bytes& memo);
    Transaction create_transfer_many(const std::map<Address, uint64_t>& dests, uint64_t fee);
    Transaction create_withdrawal(const bt::btc::Address& main_address, uint64_t value, uint64_t main_fee, uint64_t fee);
    Transaction create_bitasset_transfer(const Address& dest, const Hash32& asset, uint64_t amount, uint64_t fee, const Bytes& memo);
    /** reserve_bitasset: like theirs, plus (policy) one value input with full change when the wallet
     *  has a coin (policy). */
    Transaction reserve_bitasset(const std::string& plain_name);
    Transaction register_bitasset(const std::string& plain_name, const BitAssetData& data, uint64_t initial_supply);
    Transaction amm_mint(const AssetId& asset0, const AssetId& asset1, uint64_t amount0, uint64_t amount1, uint64_t lp_token_mint);
    Transaction amm_burn(const AssetId& asset0, const AssetId& asset1, uint64_t amount0, uint64_t amount1, uint64_t lp_token_burn);
    Transaction amm_swap(const AssetId& spend, const AssetId& receive, uint64_t amount_spend, uint64_t amount_receive);
    Transaction dutch_auction_create(const DutchAuctionParams& p);
    Transaction dutch_auction_bid(const Hash32& auction_id, const AssetId& base, const AssetId& quote, uint64_t bid_size, uint64_t receive_quantity);
    /** dutch_auction_collect: the receipt plus one of the wallet's BitAsset coins, paid back in
     *  full (kept fix, wallet only: see build_dutch_auction_collect). */
    Transaction dutch_auction_collect(const Hash32& auction_id, const AssetId& base, const AssetId& quote, uint64_t amount_base, uint64_t amount_quote);
    /** The collect builder without wallet state (also used by the tests): `receipt` first, then
     *  one coin of `coins`; outputs to `to` = {the coin back, base, quote}. Throws WalletError if
     *  no coin gives a transaction that fills and credits exactly the coin plus the amounts. */
    static Transaction build_dutch_auction_collect(const std::pair<OutPoint, FilledOutput>& receipt, std::vector<std::pair<OutPoint, FilledOutput>> coins,
                                                   const AssetId& base, const AssetId& quote, uint64_t amount_base, uint64_t amount_quote,
                                                   const std::array<Address, 3>& to);
    AuthorizedTransaction authorize(const Transaction& tx);
    /** Kept fix (wallet only): refuse to send a wallet-built transaction whose outputs consensus
     *  cannot fill. */
    void check_fillable(const Transaction& tx);

    bt::frost::Signature sign_arbitrary_msg(const bt::frost::Point& vk, const std::string& msg);
    Authorization sign_arbitrary_msg_as_addr(const Address& a, const std::string& msg);
    Bytes decrypt_msg(const bt::ByteArray<32>& epk, ByteSpan ciphertext);
    std::optional<std::string> plain_name(const Hash32& id);

    // key derivation (exposed for tests and differential checks)
    static bt::ByteArray<32> secret_bytes(const bt::ByteArray<64>& seed, uint32_t account, uint32_t index);
    static bt::frost::SigningKey tx_signing_key(const bt::ByteArray<64>& seed, uint32_t index);
    static bt::frost::SigningKey message_signing_key(const bt::ByteArray<64>& seed, uint32_t index);
    static bt::ByteArray<32> encryption_secret(const bt::ByteArray<64>& seed, uint32_t index);
    /** blake3::keyed_hash(scalar bytes of the reservation address's key, name_hash). */
    static Hash32 reservation_nonce(const bt::frost::SigningKey& sk, const Hash32& name_hash);

private:
    std::unique_ptr<bt::kv::Store> store_;
    Node& node_;
    std::mutex mu_;
    std::optional<bt::ByteArray<64>> seed();
    bt::ByteArray<64> need_seed();
    std::optional<uint32_t> index_of(uint8_t table, ByteSpan key);
    bt::frost::SigningKey key_for_address(const Address& a);
    std::set<OutPoint> pending_inputs();
    std::pair<uint64_t, std::vector<std::pair<OutPoint, FilledOutput>>> select_coins(uint64_t value);
    std::pair<uint64_t, std::vector<OutPoint>> select_asset(const AssetId& a, uint64_t amount);
    std::pair<uint64_t, std::vector<OutPoint>> select_lp_tokens(const AssetId& a0, const AssetId& a1, uint64_t amount);
    void remember_name(const Hash32& id, const std::string& plain);
};

} // namespace bitassets
