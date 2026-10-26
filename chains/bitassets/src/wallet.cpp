// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// Port of the plain-bitassets fork v0.17.1 lib/wallet.rs (keys, coin selection, the
// transaction builders the RPC uses). Coins are the node's UTXOs at our addresses.
#include <bitassets/wallet.h>

#include <bt/bip32.h>
#include <bt/crypto.h>
#include <bt/ecies.h>
#include <bt/hd.h>
#include <bt/log.h>

#include <algorithm>
#include <array>
#include <cstring>

#include <sodium.h>

namespace bitassets {

using bt::kv::Txn;
using bt::bip32::HARDENED;

enum : uint8_t {
    W_SEED = 1, W_INDEX_TO_ADDR, W_ADDR_TO_INDEX, W_INDEX_TO_EPK, W_EPK_TO_INDEX, W_INDEX_TO_VK, W_VK_TO_INDEX, W_RESERVATIONS, W_KNOWN_NAMES
};

Wallet::Wallet(const std::string& path, Node& node) : store_(bt::kv::open_sqlite(path)), node_(node) {}

// The app layer's previews (PreviewScope): this thread's builders take no new address while one lives.
static thread_local int g_preview_depth = 0;
Wallet::PreviewScope::PreviewScope() { ++g_preview_depth; }
Wallet::PreviewScope::~PreviewScope() { --g_preview_depth; }

// ------------------------------------------------------------------ keys (wallet.rs:285-387)
bt::ByteArray<32> Wallet::secret_bytes(const bt::ByteArray<64>& seed, uint32_t account, uint32_t index)
{
    auto m = bt::bip32::master(seed.span()); // Xpriv::new_master(NetworkKind::Test, seed)
    if (!m) throw WalletError("bip32 error");
    auto x = bt::bip32::derive_path(*m, {account | HARDENED, index});
    if (!x) throw WalletError("bip32 error");
    return x->secret; // private_key.secret_bytes(): 32 big-endian bytes
}

// signing_key_from_secret_bytes: Scalar::from_bytes_mod_order(bytes) reads them little-endian
static bt::frost::SigningKey frost_key(const bt::ByteArray<32>& b)
{
    bt::ByteArray<64> wide{};
    std::memcpy(wide.data(), b.data(), 32);
    bt::frost::SigningKey k{bt::frost::reduce_wide(wide)};
    if (k.s.is_zero()) throw WalletError("expected secret scalar to be non-zero");
    return k;
}

bt::frost::SigningKey Wallet::tx_signing_key(const bt::ByteArray<64>& seed, uint32_t index) { return frost_key(secret_bytes(seed, 0, index)); }
bt::frost::SigningKey Wallet::message_signing_key(const bt::ByteArray<64>& seed, uint32_t index) { return frost_key(secret_bytes(seed, 2, index)); }
bt::ByteArray<32> Wallet::encryption_secret(const bt::ByteArray<64>& seed, uint32_t index) { return secret_bytes(seed, 1, index); }

Hash32 Wallet::reservation_nonce(const bt::frost::SigningKey& sk, const Hash32& name_hash)
{
    return bt::sidechain::keyed_hash(Hash32::from_span(sk.s.span()), name_hash.span()); // keyed_hash(&scalar.to_bytes(), name_hash)
}

// ------------------------------------------------------------------ seed and indexes
std::optional<bt::ByteArray<64>> Wallet::seed()
{
    Txn t(*store_);
    auto v = t.get(W_SEED, Bytes{0});
    if (!v || v->size() != 64) return std::nullopt;
    return bt::ByteArray<64>::from_span(*v);
}

std::optional<Hash32> Wallet::seed_digest(std::string_view label)
{
    std::lock_guard<std::mutex> l(mu_);
    auto s = seed();
    if (!s) return std::nullopt;
    const uint8_t zero = 0;
    Hash32 h = bt::sha256_parts({ByteSpan(reinterpret_cast<const uint8_t*>(label.data()), label.size()), ByteSpan(&zero, 1), s->span()});
    sodium_memzero(s->data(), s->size());
    return h;
}

bt::ByteArray<64> Wallet::need_seed()
{
    auto s = seed();
    if (!s) throw WalletError("no seed");
    return *s;
}

bool Wallet::has_seed()
{
    std::lock_guard<std::mutex> l(mu_);
    return seed().has_value();
}

void Wallet::set_seed(const bt::ByteArray<64>& s)
{
    std::lock_guard<std::mutex> l(mu_);
    auto cur = seed();
    if (cur) {
        if (*cur == s) return;
        throw WalletError("seed already exists");
    }
    Txn t(*store_);
    t.put(W_SEED, Bytes{0}, s.span());
    for (uint8_t tb : {W_INDEX_TO_ADDR, W_ADDR_TO_INDEX, W_RESERVATIONS}) t.clear(tb);
    t.commit();
}

void Wallet::set_seed_from_mnemonic(const std::string& mnemonic)
{
    auto p = bt::hd::parse_mnemonic(mnemonic);
    if (!p) throw WalletError("failed to parse mnemonic seed phrase: " + bt::hd::mnemonic_error(mnemonic));
    set_seed(bt::hd::mnemonic_to_seed(*p));
}

static Bytes be32(uint32_t v)
{
    Writer w;
    w.u32be(v);
    return w.buf;
}
static uint32_t from_be32(ByteSpan b) { return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | b[3]; }

std::optional<uint32_t> Wallet::index_of(uint8_t table, ByteSpan key)
{
    Txn t(*store_);
    auto v = t.get(table, key);
    if (!v) return std::nullopt;
    return from_be32(*v);
}

static uint32_t next_index(const Txn& t, uint8_t table)
{
    auto l = t.last(table);
    return l ? from_be32(l->first) + 1 : 0;
}

Address Wallet::get_new_address()
{
    if (g_preview_depth > 0) return Address{}; // a preview: no address taken, nothing written
    std::lock_guard<std::mutex> l(mu_);
    auto s = need_seed();
    Txn t(*store_);
    uint32_t index = next_index(t, W_INDEX_TO_ADDR);
    Address a = bt::sidechain::address_from_vk(tx_signing_key(s, index).verifying_key());
    t.put(W_INDEX_TO_ADDR, be32(index), a.b.span());
    t.put(W_ADDR_TO_INDEX, a.b.span(), be32(index));
    t.commit();
    return a;
}

bool Wallet::is_mine(const Address& a) { return index_of(W_ADDR_TO_INDEX, a.b.span()).has_value(); }

Address Wallet::get_receive_address()
{
    std::optional<Address> last;
    {
        std::lock_guard<std::mutex> l(mu_);
        Txn t(*store_);
        if (auto lv = t.last(W_INDEX_TO_ADDR)) {
            Address a;
            std::memcpy(a.b.data(), lv->second.data(), 20);
            last = a;
        }
    }
    if (last) {
        std::set<Address> s{*last};
        bool received = !node_.utxos_by_addresses(s).empty() || !node_.stxos_by_addresses(s).empty();
        for (auto& tx : node_.mempool())
            for (auto& o : tx.transaction.outputs)
                if (o.address == *last) received = true; // their unconfirmed_utxos
        if (!received) return *last;
    }
    return get_new_address();
}

std::vector<Address> Wallet::addresses()
{
    std::lock_guard<std::mutex> l(mu_);
    Txn t(*store_);
    std::vector<Address> r;
    t.scan(W_INDEX_TO_ADDR, [&](ByteSpan, ByteSpan v) {
        Address a;
        std::memcpy(a.b.data(), v.data(), 20);
        r.push_back(a);
        return true;
    });
    std::sort(r.begin(), r.end(), [](const Address& a, const Address& b) { return a.to_base58() < b.to_base58(); });
    return r;
}

bt::ByteArray<32> Wallet::get_new_encryption_key()
{
    std::lock_guard<std::mutex> l(mu_);
    auto s = need_seed();
    Txn t(*store_);
    uint32_t index = next_index(t, W_INDEX_TO_EPK);
    auto epk = bt::ecies::x25519_public(encryption_secret(s, index));
    t.put(W_INDEX_TO_EPK, be32(index), epk.span());
    t.put(W_EPK_TO_INDEX, epk.span(), be32(index));
    t.commit();
    return epk;
}

bt::frost::Point Wallet::get_new_verifying_key()
{
    std::lock_guard<std::mutex> l(mu_);
    auto s = need_seed();
    Txn t(*store_);
    uint32_t index = next_index(t, W_INDEX_TO_VK);
    auto vk = message_signing_key(s, index).verifying_key();
    t.put(W_INDEX_TO_VK, be32(index), vk.span());
    t.put(W_VK_TO_INDEX, vk.span(), be32(index));
    t.commit();
    return vk;
}

bt::frost::SigningKey Wallet::key_for_address(const Address& a)
{
    auto idx = index_of(W_ADDR_TO_INDEX, a.b.span());
    if (!idx) throw WalletError("Address does not exist: " + a.to_base58());
    auto k = tx_signing_key(need_seed(), *idx);
    if (bt::sidechain::address_from_vk(k.verifying_key()) != a) throw WalletError("signing key does not match the address");
    return k;
}

void Wallet::remember_name(const Hash32& id, const std::string& plain)
{
    std::lock_guard<std::mutex> l(mu_);
    Txn t(*store_);
    t.put(W_KNOWN_NAMES, id.span(), ByteSpan((const uint8_t*)plain.data(), plain.size()));
    t.commit();
}

std::optional<std::string> Wallet::reserved_name(const Hash32& commitment)
{
    Txn t(*store_);
    auto v = t.get(W_RESERVATIONS, commitment.span());
    if (!v) return std::nullopt;
    return std::string(v->begin(), v->end());
}

bool Wallet::has_spendable_coin()
{
    try {
        select_coins(1);
        return true;
    } catch (const WalletError&) {
        return false;
    }
}

std::optional<std::string> Wallet::plain_name(const Hash32& id)
{
    Txn t(*store_);
    auto v = t.get(W_KNOWN_NAMES, id.span());
    if (!v) return std::nullopt;
    return std::string(v->begin(), v->end());
}

// ------------------------------------------------------------------ coins
// Their wallet db (app.rs update_wallet) moves a coin to its stxos as soon as a mempool
// transaction spends it, so get_wallet_utxos / bitcoin_balance leave such coins out: as theirs.
// (Their db also keeps coins a reorg removed from the chain; ours is a view on the node's UTXO
// set and drops them: kept fix, wallet only.)
std::vector<std::pair<OutPoint, FilledOutput>> Wallet::utxos()
{
    auto a = addresses();
    auto u = node_.utxos_by_addresses(std::set<Address>(a.begin(), a.end()));
    auto pending = pending_inputs();
    std::erase_if(u, [&](auto& e) { return pending.count(e.first) > 0; });
    return u;
}

std::vector<std::pair<OutPoint, SpentOutput>> Wallet::stxos()
{
    auto a = addresses();
    return node_.stxos_by_addresses(std::set<Address>(a.begin(), a.end()));
}

Balance Wallet::balance()
{
    Balance b;
    for (auto& [op, o] : utxos()) {
        uint64_t v = o.bitcoin_value();
        if (b.total + v < b.total) throw WalletError("Bitcoin amount overflow");
        b.total += v;
        if (!o.content.is_withdrawal()) b.available += v;
    }
    return b;
}

std::set<OutPoint> Wallet::pending_inputs()
{
    std::set<OutPoint> r;
    for (auto& tx : node_.mempool())
        for (auto& op : tx.transaction.inputs) r.insert(op);
    return r;
}

// select_bitcoins (wallet.rs:870-926): ascending value, skip withdrawal/BitAsset/reservation/zero,
// stop once the total reaches the target. Policy: never a coin a mempool tx already spends.
std::pair<uint64_t, std::vector<std::pair<OutPoint, FilledOutput>>> Wallet::select_coins(uint64_t value)
{
    auto u = utxos();
    std::stable_sort(u.begin(), u.end(), [](auto& a, auto& b) { return a.second.bitcoin_value() < b.second.bitcoin_value(); });
    auto pending = pending_inputs();
    uint64_t total = 0;
    std::vector<std::pair<OutPoint, FilledOutput>> sel;
    for (auto& [op, o] : u) {
        if (!o.content.is_bitcoin() || pending.count(op) || o.bitcoin_value() == 0) continue;
        if (total >= value) break;
        if (total + o.bitcoin_value() < total) throw WalletError("Bitcoin amount overflow");
        total += o.bitcoin_value();
        sel.emplace_back(op, o);
    }
    if (total < value) throw WalletError("not enough funds");
    return {total, sel};
}

// select_asset_utxos (wallet.rs:987-1009) with select_bitasset_utxos / the control coin
std::pair<uint64_t, std::vector<OutPoint>> Wallet::select_asset(const AssetId& a, uint64_t amount)
{
    std::vector<OutPoint> ops;
    if (a.kind == AssetId::Bitcoin) {
        auto [total, coins] = select_coins(amount);
        for (auto& [op, _] : coins) ops.push_back(op);
        return {total, ops};
    }
    auto pending = pending_inputs();
    auto u = utxos();
    if (a.kind == AssetId::BitAssetControl) {
        if (amount != 1) throw WalletError("not enough funds");
        for (auto& [op, o] : u)
            if (o.content.is_control() && o.content.id == a.id && !pending.count(op)) return {1, {op}}; // theirs selects a BitAsset coin here
        throw WalletError("not enough funds");
    }
    std::stable_sort(u.begin(), u.end(), [](auto& x, auto& y) { return x.second.content.value < y.second.content.value; });
    uint64_t total = 0;
    for (auto& [op, o] : u) {
        if (!o.content.is_bitasset() || o.content.id != a.id || pending.count(op)) continue;
        if (total >= amount) break;
        if (total + o.content.value < total) throw WalletError("BitAsset amount overflow");
        total += o.content.value;
        ops.push_back(op);
    }
    if (total < amount) throw WalletError("not enough funds");
    return {total, ops};
}

std::pair<uint64_t, std::vector<OutPoint>> Wallet::select_lp_tokens(const AssetId& a0, const AssetId& a1, uint64_t amount)
{
    auto pending = pending_inputs();
    auto u = utxos();
    std::stable_sort(u.begin(), u.end(), [](auto& x, auto& y) { return x.second.content.value < y.second.content.value; });
    uint64_t total = 0;
    std::vector<OutPoint> ops;
    for (auto& [op, o] : u) {
        if (!o.content.is_lp_token() || o.content.asset0 != a0 || o.content.asset1 != a1 || pending.count(op)) continue;
        if (total >= amount) break;
        if (total + o.content.value < total) throw WalletError("LP token amount overflow");
        total += o.content.value;
        ops.push_back(op);
    }
    if (total < amount) throw WalletError("not enough funds");
    return {total, ops};
}

static Output value_output(const Address& a, uint64_t v)
{
    Output o;
    o.address = a;
    o.content.kind = Content::Bitcoin;
    o.content.value = v;
    return o;
}

static Output asset_output(const Address& a, const AssetId& asset, uint64_t amount)
{
    Output o;
    o.address = a;
    switch (asset.kind) {
    case AssetId::Bitcoin: o.content.kind = Content::Bitcoin; break;
    case AssetId::BitAsset: o.content.kind = Content::BitAsset; break;
    case AssetId::BitAssetControl: o.content.kind = Content::BitAssetControl; return o;
    }
    o.content.value = amount;
    return o;
}

Transaction Wallet::create_transfer(const Address& dest, uint64_t value, uint64_t fee, const Bytes& memo)
{
    if (value + fee < value) throw WalletError("Bitcoin amount overflow");
    auto [total, coins] = select_coins(value + fee);
    uint64_t change = total - value - fee;
    Transaction tx;
    for (auto& [op, _] : coins) tx.inputs.push_back(op);
    Output o = value_output(dest, value);
    o.memo = memo;
    tx.outputs.push_back(o);
    if (change != 0) tx.outputs.push_back(value_output(get_new_address(), change));
    return tx;
}

Transaction Wallet::create_transfer_many(const std::map<Address, uint64_t>& dests, uint64_t fee)
{
    if (dests.empty()) throw WalletError("no transfer destination");
    uint64_t value = 0;
    for (auto& [a, v] : dests) {
        if (value + v < value) throw WalletError("Bitcoin amount overflow");
        value += v;
    }
    if (value + fee < value) throw WalletError("Bitcoin amount overflow");
    auto [total, coins] = select_coins(value + fee);
    uint64_t change = total - value - fee;
    Transaction tx;
    for (auto& [op, _] : coins) tx.inputs.push_back(op);
    for (auto& [a, v] : dests) tx.outputs.push_back(value_output(a, v));
    if (change != 0) tx.outputs.push_back(value_output(get_new_address(), change));
    return tx;
}

Transaction Wallet::create_withdrawal(const bt::btc::Address& main_address, uint64_t value, uint64_t main_fee, uint64_t fee)
{
    if (value + fee < value || value + fee + main_fee < value + fee) throw WalletError("Bitcoin amount overflow");
    auto [total, coins] = select_coins(value + fee + main_fee);
    uint64_t change = total - value - fee - main_fee;
    Transaction tx;
    for (auto& [op, _] : coins) tx.inputs.push_back(op);
    Output w;
    w.address = get_new_address();
    w.content.kind = Content::Withdrawal;
    w.content.value = value;
    w.content.main_fee = main_fee;
    w.content.main_address = main_address;
    tx.outputs.push_back(w);
    tx.outputs.push_back(value_output(get_new_address(), change));
    return tx;
}

// create_bitasset_transfer (wallet.rs:678-713)
Transaction Wallet::create_bitasset_transfer(const Address& dest, const Hash32& asset, uint64_t amount, uint64_t fee, const Bytes& memo)
{
    auto [total_sats, coins] = select_coins(fee);
    auto [total_asset, asset_ops] = select_asset(AssetId::bitasset(asset), amount);
    Transaction tx;
    for (auto& [op, _] : coins) tx.inputs.push_back(op);
    for (auto& op : asset_ops) tx.inputs.push_back(op);
    Output o = asset_output(dest, AssetId::bitasset(asset), amount);
    o.memo = memo;
    tx.outputs.push_back(o);
    if (total_sats - fee != 0) tx.outputs.push_back(value_output(get_new_address(), total_sats - fee));
    if (total_asset - amount != 0) tx.outputs.push_back(asset_output(get_new_address(), AssetId::bitasset(asset), total_asset - amount));
    return tx;
}

// reserve_bitasset (wallet.rs:716-781)
Transaction Wallet::reserve_bitasset(const std::string& plain)
{
    Transaction tx;
    // Policy (consensus-neutral): spend one coin back to ourselves so the reservation has an
    // input.
    try {
        auto [total, coins] = select_coins(1);
        tx.inputs.push_back(coins.front().first);
        tx.outputs.push_back(value_output(get_new_address(), coins.front().second.bitcoin_value()));
    } catch (const WalletError&) {
        LOG_WARN("reserve_bitasset: no coin to spend; the reservation has no inputs");
    }
    Address raddr = tx.outputs.empty() || !is_mine(tx.outputs.back().address) ? get_new_address() : tx.outputs.back().address;
    auto sk = key_for_address(raddr);
    Hash32 name = name_hash(plain);
    Hash32 nonce = reservation_nonce(sk, name);
    Hash32 commitment = bt::sidechain::keyed_hash(nonce, name.span());
    {
        std::lock_guard<std::mutex> l(mu_);
        Txn t(*store_);
        t.put(W_RESERVATIONS, commitment.span(), ByteSpan((const uint8_t*)plain.data(), plain.size()));
        t.put(W_KNOWN_NAMES, name.span(), ByteSpan((const uint8_t*)plain.data(), plain.size()));
        t.commit();
    }
    Output r;
    r.address = raddr;
    r.content.kind = Content::BitAssetReservation;
    tx.outputs.push_back(r);
    TxData d;
    d.kind = TxData::BitAssetReservation;
    d.commitment = commitment;
    tx.data = d;
    return tx;
}

// register_bitasset (wallet.rs:783-868)
Transaction Wallet::register_bitasset(const std::string& plain, const BitAssetData& data, uint64_t initial_supply)
{
    Hash32 name = name_hash(plain);
    std::optional<std::pair<OutPoint, Hash32>> found;
    auto pending = pending_inputs();
    for (auto& [op, o] : utxos()) {
        if (!o.content.is_reservation() || pending.count(op)) continue;
        auto sk = key_for_address(o.address);
        Hash32 nonce = reservation_nonce(sk, name);
        if (bt::sidechain::keyed_hash(nonce, name.span()) == o.content.commitment) {
            found = std::make_pair(op, nonce);
            break;
        }
    }
    remember_name(name, plain);
    if (!found) throw WalletError("could not find bitasset reservation for `" + plain + "`");
    Address addr = get_new_address();
    Transaction tx;
    tx.inputs.push_back(found->first);
    if (initial_supply != 0) tx.outputs.push_back(asset_output(addr, AssetId::bitasset(name), initial_supply));
    tx.outputs.push_back(asset_output(addr, AssetId::control(name), 1));
    TxData d;
    d.kind = TxData::BitAssetRegistration;
    d.name_hash = name;
    d.revealed_nonce = found->second;
    d.bitasset_data = data;
    d.initial_supply = initial_supply;
    tx.data = d;
    return tx;
}

// amm_mint (wallet.rs:1076-1157): the first two unique spent assets must be asset0 and asset1
Transaction Wallet::amm_mint(const AssetId& asset0, const AssetId& asset1, uint64_t amount0, uint64_t amount1, uint64_t lp_token_mint)
{
    Address lp_addr = get_new_address();
    auto [in0, ops0] = select_asset(asset0, amount0);
    auto [in1, ops1] = select_asset(asset1, amount1);
    Transaction tx;
    tx.inputs = ops0;
    tx.inputs.insert(tx.inputs.end(), ops1.begin(), ops1.end());
    if (in0 - amount0 != 0) tx.outputs.push_back(asset_output(get_new_address(), asset0, in0 - amount0));
    if (in1 - amount1 != 0) tx.outputs.push_back(asset_output(get_new_address(), asset1, in1 - amount1));
    Output lp;
    lp.address = lp_addr;
    lp.content.kind = Content::AmmLpToken;
    lp.content.value = lp_token_mint;
    tx.outputs.push_back(lp);
    TxData d;
    d.kind = TxData::AmmMint;
    d.amount0 = amount0;
    d.amount1 = amount1;
    d.lp_tokens = lp_token_mint;
    tx.data = d;
    return tx;
}

// amm_burn (wallet.rs:1160-1227).
// Kept fix (wallet only; consensus unchanged): under the consensus rules as reproduced, a burn is valid only when it also
// spends coins of two BitAssets; theirs spends the LP tokens alone, which their node and ours reject. Ours spends one
// coin of each BitAsset of the pool and pays it back with the amount taken out (as the Dutch-auction collect), test
// wallet_amm_burn_with_a_coin_of_each_asset_is_valid.
Transaction Wallet::amm_burn(const AssetId& asset0, const AssetId& asset1, uint64_t amount0, uint64_t amount1, uint64_t lp_token_burn)
{
    std::array<Address, 3> to{get_new_address(), get_new_address(), get_new_address()};
    auto [in_lp, lp_ops] = select_lp_tokens(asset0, asset1, lp_token_burn);
    (void)in_lp;
    auto mine = utxos(); // leaves out coins spent by mempool transactions
    std::vector<std::pair<OutPoint, FilledOutput>> lp_coins;
    for (auto& op : lp_ops) {
        auto it = std::find_if(mine.begin(), mine.end(), [&](auto& p) { return p.first == op; });
        if (it == mine.end()) throw WalletError("no utxo");
        lp_coins.push_back(*it);
    }
    auto smallest = [&](const AssetId& a) {
        std::optional<std::pair<OutPoint, FilledOutput>> r;
        if (a.kind != AssetId::BitAsset) return r;
        for (auto& e : mine) {
            if (!e.second.content.is_bitasset() || e.second.content.id != a.id) continue;
            if (!r || e.second.content.value < r->second.content.value) r = e;
        }
        return r;
    };
    return build_amm_burn(lp_coins, smallest(asset0), smallest(asset1), asset0, asset1, amount0, amount1, lp_token_burn, to);
}

Transaction Wallet::build_amm_burn(const std::vector<std::pair<OutPoint, FilledOutput>>& lp_coins, const std::optional<std::pair<OutPoint, FilledOutput>>& coin0,
                                   const std::optional<std::pair<OutPoint, FilledOutput>>& coin1, const AssetId& asset0, const AssetId& asset1, uint64_t amount0,
                                   uint64_t amount1, uint64_t lp_token_burn, const std::array<Address, 3>& to)
{
    if (asset0.kind != AssetId::BitAsset || asset1.kind != AssetId::BitAsset)
        throw WalletError("this wallet takes liquidity out of pools of two BitAssets only (the chain's rules need a coin of each spent with it)");
    if (!coin0 || !coin1)
        throw WalletError("taking liquidity out of this pool spends one coin of each of its two assets with the LP tokens: this wallet has none of " +
                          (coin0 ? asset1 : asset0).to_hex());
    uint64_t in_lp = 0;
    Transaction tx;
    std::vector<FilledOutput> spent;
    for (auto& [op, o] : lp_coins) {
        if (in_lp + o.content.value < in_lp) throw WalletError("LP token amount overflow");
        in_lp += o.content.value;
        tx.inputs.push_back(op);
        spent.push_back(o);
    }
    if (in_lp < lp_token_burn) throw WalletError("not enough funds");
    tx.inputs.push_back(coin0->first);
    spent.push_back(coin0->second);
    tx.inputs.push_back(coin1->first);
    spent.push_back(coin1->second);
    const uint64_t out0 = coin0->second.content.value + amount0, out1 = coin1->second.content.value + amount1;
    if (out0 < amount0 || out1 < amount1) throw WalletError("amount overflow");
    if (in_lp - lp_token_burn != 0) {
        Output c;
        c.address = to[0];
        c.content.kind = Content::AmmLpToken;
        c.content.value = in_lp - lp_token_burn;
        tx.outputs.push_back(c);
    }
    tx.outputs.push_back(asset_output(to[1], asset0, out0));
    tx.outputs.push_back(asset_output(to[2], asset1, out1));
    TxData d;
    d.kind = TxData::AmmBurn;
    d.amount0 = amount0;
    d.amount1 = amount1;
    d.lp_tokens = lp_token_burn;
    tx.data = d;
    // self-check: the outputs fill and pay out exactly the two coins plus the amounts (and the LP change)
    FilledTransaction ft{&tx, spent};
    auto filled = ft.filled_outputs();
    if (!filled || !ft.bitcoin_fee()) throw WalletError("this burn cannot be paid out under the current consensus rules; not sent");
    std::map<AssetId, uint64_t> want, got;
    want[asset0] += out0;
    want[asset1] += out1;
    uint64_t lp_out = 0;
    for (auto& f : *filled) {
        if (f.content.is_lp_token()) {
            lp_out += f.content.value;
            continue;
        }
        auto av = f.content.asset_value();
        if (!av) throw WalletError("this burn cannot be paid out under the current consensus rules; not sent");
        got[av->first] += av->second;
    }
    if (want != got || lp_out != in_lp - lp_token_burn) throw WalletError("this burn cannot be paid out under the current consensus rules; not sent");
    return tx;
}

// amm_swap (wallet.rs:1230-1287): the first unique spent asset must be the spent one.
// Kept fix (wallet only; consensus unchanged): a BitAsset received from a pool is paid out in one output together with
// one of the wallet's coins of it, spent after the spent asset's coins (so that asset stays the first one), as the
// Dutch-auction collect does. Their builder pays the received amount alone, which the outputs never fill: the swap
// cannot be sent (test wallet_amm_swap_with_a_coin_of_the_received_asset_is_valid).
Transaction Wallet::amm_swap(const AssetId& spend, const AssetId& receive, uint64_t amount_spend, uint64_t amount_receive)
{
    std::array<Address, 2> to{get_new_address(), get_new_address()};
    auto [in, ops] = select_asset(spend, amount_spend);
    (void)in;
    auto mine = utxos(); // leaves out coins spent by mempool transactions
    std::vector<std::pair<OutPoint, FilledOutput>> coins;
    for (auto& op : ops) {
        auto it = std::find_if(mine.begin(), mine.end(), [&](auto& p) { return p.first == op; });
        if (it == mine.end()) throw WalletError("no utxo");
        coins.push_back(*it);
    }
    std::optional<std::pair<OutPoint, FilledOutput>> receive_coin;
    if (receive.kind == AssetId::BitAsset) {
        for (auto& e : mine) {
            if (!e.second.content.is_bitasset() || e.second.content.id != receive.id) continue;
            if (!receive_coin || e.second.content.value < receive_coin->second.content.value) receive_coin = e;
        }
    }
    return build_amm_swap(coins, receive_coin, spend, receive, amount_spend, amount_receive, to);
}

Transaction Wallet::build_amm_swap(const std::vector<std::pair<OutPoint, FilledOutput>>& spend_coins, const std::optional<std::pair<OutPoint, FilledOutput>>& receive_coin,
                                   const AssetId& spend, const AssetId& receive, uint64_t amount_spend, uint64_t amount_receive, const std::array<Address, 2>& to)
{
    auto value_of = [](const FilledOutput& o) -> uint64_t { return o.content.is_bitcoin() ? o.bitcoin_value() : o.content.value; };
    uint64_t in = 0;
    Transaction tx;
    std::vector<FilledOutput> spent;
    for (auto& [op, o] : spend_coins) {
        if (in + value_of(o) < in) throw WalletError("amount overflow");
        in += value_of(o);
        tx.inputs.push_back(op);
        spent.push_back(o);
    }
    if (in < amount_spend) throw WalletError("not enough funds");
    if (in - amount_spend != 0) tx.outputs.push_back(asset_output(to[0], spend, in - amount_spend));
    uint64_t out_receive = amount_receive;
    if (receive.kind == AssetId::BitAsset) {
        if (!receive_coin) throw WalletError("a BitAsset received from a pool is paid out with a coin of it: this wallet has none of " + receive.to_hex());
        out_receive = receive_coin->second.content.value + amount_receive;
        if (out_receive < amount_receive) throw WalletError("amount overflow");
        tx.inputs.push_back(receive_coin->first);
        spent.push_back(receive_coin->second);
    }
    tx.outputs.push_back(asset_output(to[1], receive, out_receive));
    TxData d;
    d.kind = TxData::AmmSwap;
    d.amount_spent = amount_spend;
    d.amount_receive = amount_receive;
    d.pair_asset = receive;
    tx.data = d;
    // self-check: the outputs fill, the fee is defined, and they pay out exactly the change and the coin plus the amount
    FilledTransaction ft{&tx, spent};
    auto filled = ft.filled_outputs();
    if (!filled || !ft.bitcoin_fee()) throw WalletError("this swap cannot be paid out under the current consensus rules; not sent");
    std::map<AssetId, uint64_t> want, got;
    want[spend] += in - amount_spend;
    want[receive] += out_receive;
    for (auto& f : *filled) {
        auto av = f.content.asset_value();
        if (!av) throw WalletError("this swap cannot be paid out under the current consensus rules; not sent");
        got[av->first] += av->second;
    }
    std::erase_if(want, [](auto& e) { return e.second == 0; });
    std::erase_if(got, [](auto& e) { return e.second == 0; });
    if (want != got) throw WalletError("this swap cannot be paid out under the current consensus rules; not sent");
    return tx;
}

// dutch_auction_create (wallet.rs:1290-1331)
Transaction Wallet::dutch_auction_create(const DutchAuctionParams& p)
{
    auto [in, ops] = select_asset(p.base_asset, p.base_amount);
    Transaction tx;
    tx.inputs = ops;
    if (in - p.base_amount != 0) tx.outputs.push_back(asset_output(get_new_address(), p.base_asset, in - p.base_amount));
    Output r;
    r.address = get_new_address();
    r.content.kind = Content::DutchAuctionReceipt;
    tx.outputs.push_back(r);
    TxData d;
    d.kind = TxData::DutchAuctionCreate;
    d.params = p;
    tx.data = d;
    return tx;
}

// dutch_auction_bid (wallet.rs:1334-1398)
Transaction Wallet::dutch_auction_bid(const Hash32& id, const AssetId& base, const AssetId& quote, uint64_t bid_size, uint64_t receive_quantity)
{
    auto [in, ops] = select_asset(quote, bid_size);
    Transaction tx;
    tx.inputs = ops;
    // Kept fix (wallet only): the change in the quote asset goes FIRST, the order in which
    // consensus fills the outputs (test bid_output_order).
    if (in - bid_size != 0) tx.outputs.push_back(asset_output(get_new_address(), quote, in - bid_size));
    tx.outputs.push_back(asset_output(get_new_address(), base, receive_quantity));
    TxData d;
    d.kind = TxData::DutchAuctionBid;
    d.auction_id = id;
    d.receive_asset = base;
    d.quantity = receive_quantity;
    d.bid_size = bid_size;
    tx.data = d;
    return tx;
}

// dutch_auction_collect (wallet.rs:1401-1465): the receipt must be the first receipt input.
// Kept fix (wallet only; consensus unchanged): the collect also spends one of the wallet's
// BitAsset coins and pays it back in full, which keeps it valid under the consensus rules as
// reproduced (test wallet_dutch_auction_collect_is_valid).
Transaction Wallet::dutch_auction_collect(const Hash32& id, const AssetId& base, const AssetId& quote, uint64_t amount_base, uint64_t amount_quote)
{
    std::optional<std::pair<OutPoint, FilledOutput>> receipt;
    std::vector<std::pair<OutPoint, FilledOutput>> coins;
    for (auto& e : utxos()) { // utxos() leaves out coins spent by mempool transactions
        if (!receipt && e.second.content.is_receipt() && e.second.content.id == id) receipt = e;
        else if (e.second.content.is_bitasset()) coins.push_back(e);
    }
    if (!receipt) throw WalletError("not enough funds");
    std::array<Address, 3> to{get_new_address(), get_new_address(), get_new_address()};
    return build_dutch_auction_collect(*receipt, coins, base, quote, amount_base, amount_quote, to);
}

Transaction Wallet::build_dutch_auction_collect(const std::pair<OutPoint, FilledOutput>& receipt, std::vector<std::pair<OutPoint, FilledOutput>> coins,
                                                const AssetId& base, const AssetId& quote, uint64_t amount_base, uint64_t amount_quote,
                                                const std::array<Address, 3>& to)
{
    TxData d;
    d.kind = TxData::DutchAuctionCollect;
    d.asset_offered = base;
    d.asset_receive = quote;
    d.amount_offered_remaining = amount_base;
    d.amount_received = amount_quote;
    // Outputs follow the order in which consensus fills them: the added coin's asset first, then
    // the base and the quote of the auction. Prefer a coin of another asset, then the quote asset,
    // then the base asset.
    auto rank = [&](const FilledOutput& o) {
        AssetId a = AssetId::bitasset(o.content.id);
        return a == quote ? 1 : a == base ? 2 : 0;
    };
    std::stable_sort(coins.begin(), coins.end(), [&](auto& x, auto& y) { return rank(x.second) < rank(y.second); });
    const bool same_queue = base.kind == AssetId::BitAsset && quote.kind == AssetId::BitAsset;
    for (auto& [op, coin] : coins) {
        if (!coin.content.is_bitasset()) continue;
        AssetId a = AssetId::bitasset(coin.content.id);
        uint64_t first = coin.content.value;
        if (a == base) first += amount_base;
        else if (a == quote) first += amount_quote;
        if (first < coin.content.value) continue; // overflow
        Transaction tx;
        tx.inputs = {receipt.first, op};
        tx.outputs.push_back(asset_output(to[0], a, first));
        const bool quote_follows = a != quote && amount_quote != 0 && same_queue;
        if (a != base) {
            if (amount_base != 0 || quote_follows) tx.outputs.push_back(asset_output(to[1], base, amount_base));
        } else if (quote_follows) {
            if (amount_base != 0) continue; // this coin cannot carry both amounts
            tx.outputs.push_back(asset_output(to[1], base, 0));
        }
        if (a != quote && amount_quote != 0) tx.outputs.push_back(asset_output(to[2], quote, amount_quote));
        tx.data = d;
        // self-check: the outputs fill and pay out exactly the coin plus the auction's amounts
        FilledTransaction ft{&tx, {receipt.second, coin}};
        auto filled = ft.filled_outputs();
        if (!filled || !ft.bitcoin_fee()) continue;
        std::map<AssetId, uint64_t> want, got;
        want[a] += coin.content.value;
        want[base] += amount_base;
        want[quote] += amount_quote;
        bool ok = true;
        for (auto& f : *filled) {
            auto av = f.content.asset_value();
            if (!av) ok = false;
            else got[av->first] += av->second;
        }
        std::erase_if(want, [](auto& e) { return e.second == 0; });
        std::erase_if(got, [](auto& e) { return e.second == 0; });
        if (ok && want == got) return tx;
    }
    throw WalletError("cannot build a valid Dutch auction collect: the wallet needs a BitAsset coin to include");
}

void Wallet::check_fillable(const Transaction& tx)
{
    auto mine = utxos();
    std::vector<FilledOutput> spent;
    for (auto& op : tx.inputs) {
        auto it = std::find_if(mine.begin(), mine.end(), [&](auto& p) { return p.first == op; });
        if (it == mine.end()) return; // not all ours: leave it to the node
        spent.push_back(it->second);
    }
    if (!FilledTransaction{&tx, spent}.filled_outputs())
        throw WalletError("the outputs of this transaction cannot be filled under the current consensus rules; not sent");
}

AuthorizedTransaction Wallet::authorize(const Transaction& tx)
{
    AuthorizedTransaction at;
    at.transaction = tx;
    Bytes msg = tx_signing_message(tx);
    auto mine = utxos();
    for (auto& op : tx.inputs) {
        auto it = std::find_if(mine.begin(), mine.end(), [&](auto& p) { return p.first == op; });
        if (it == mine.end()) throw WalletError("no utxo");
        auto sk = key_for_address(it->second.address);
        at.authorizations.push_back(Authorization{sk.verifying_key(), sk.sign(msg)});
    }
    return at;
}

bt::frost::Signature Wallet::sign_arbitrary_msg(const bt::frost::Point& vk, const std::string& msg)
{
    auto idx = index_of(W_VK_TO_INDEX, vk.span());
    if (!idx) throw WalletError("Message signature verification key " + vk_to_string(vk) + " does not exist");
    auto sk = message_signing_key(need_seed(), *idx);
    return sign_with_dst(sk, 255, ByteSpan((const uint8_t*)msg.data(), msg.size()));
}

Authorization Wallet::sign_arbitrary_msg_as_addr(const Address& a, const std::string& msg)
{
    auto sk = key_for_address(a);
    return Authorization{sk.verifying_key(), sign_with_dst(sk, 255, ByteSpan((const uint8_t*)msg.data(), msg.size()))};
}

Bytes Wallet::decrypt_msg(const bt::ByteArray<32>& epk, ByteSpan ct)
{
    auto idx = index_of(W_EPK_TO_INDEX, epk.span());
    if (!idx) throw WalletError("Encryption pubkey " + epk_to_string(epk) + " does not exist");
    auto pt = bt::ecies::decrypt(encryption_secret(need_seed(), *idx), ct);
    if (!pt) throw WalletError("ECIES decryption failed");
    return *pt;
}

} // namespace bitassets
