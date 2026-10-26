// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
#include <bitassets/json.h>

#include <bt/sidechain/rollback.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <bt/parse.h>

#include <cstring>

namespace bitassets::json {

static UniValue obj() { return UniValue(UniValue::VOBJ); }
static UniValue arr() { return UniValue(UniValue::VARR); }
static UniValue tagged(const std::string& k, const UniValue& v)
{
    UniValue r = obj();
    r.pushKV(k, v);
    return r;
}

UniValue hash(const Hash32& h) { return UniValue(bt::hex_encode(h)); }
UniValue btc_hash(const Hash32& h) { return UniValue(bt::hex_encode_rev(h.span())); }
UniValue bytes_array(ByteSpan b)
{
    UniValue a = arr();
    for (uint8_t x : b) a.push_back(UniValue(int(x)));
    return a;
}
UniValue address(const Address& a) { return UniValue(a.to_base58()); }

UniValue outpoint(const OutPoint& o)
{
    if (o.kind == OutPoint::Deposit) return tagged("Deposit", UniValue(bt::hex_encode_rev(o.txid.span()) + ":" + std::to_string(o.vout)));
    UniValue inner = obj();
    inner.pushKV("txid", hash(o.txid));
    inner.pushKV("vout", UniValue(uint64_t(o.vout)));
    return tagged(o.kind == OutPoint::Regular ? "Regular" : "Coinbase", inner);
}

static UniValue withdrawal(uint64_t value, uint64_t main_fee, const bt::btc::Address& a)
{
    UniValue w = obj();
    w.pushKV("value_sats", UniValue(value));
    w.pushKV("main_fee_sats", UniValue(main_fee));
    w.pushKV("main_address", a.to_string());
    return w;
}

UniValue asset_id(const AssetId& a) { return UniValue(a.to_hex()); }

UniValue content(const Content& c)
{
    switch (c.kind) {
    case Content::AmmLpToken: return tagged("AmmLpToken", UniValue(c.value));
    case Content::Bitcoin: return tagged("BitcoinSats", UniValue(c.value));
    case Content::BitAsset: return tagged("BitAsset", UniValue(c.value));
    case Content::BitAssetControl: return UniValue("BitAssetControl");
    case Content::BitAssetReservation: return UniValue("BitAssetReservation");
    case Content::DutchAuctionReceipt: return UniValue("DutchAuctionReceipt");
    case Content::Withdrawal: return tagged("Withdrawal", withdrawal(c.value, c.main_fee, c.main_address));
    }
    return UniValue();
}

UniValue filled_content(const FilledContent& c)
{
    switch (c.kind) {
    case FilledContent::AmmLpToken: {
        UniValue x = obj();
        x.pushKV("asset0", asset_id(c.asset0));
        x.pushKV("asset1", asset_id(c.asset1));
        x.pushKV("amount", UniValue(c.value));
        return tagged("AmmLpToken", x);
    }
    case FilledContent::Bitcoin: return tagged("BitcoinSats", UniValue(c.value));
    case FilledContent::BitcoinWithdrawal: return tagged("BitcoinWithdrawal", withdrawal(c.value, c.main_fee, c.main_address));
    case FilledContent::BitAsset: {
        UniValue p = arr();
        p.push_back(hash(c.id));
        p.push_back(UniValue(c.value));
        return tagged("BitAsset", p);
    }
    case FilledContent::BitAssetControl: return tagged("BitAssetControl", hash(c.id));
    case FilledContent::BitAssetReservation: {
        UniValue p = arr();
        p.push_back(hash(c.id));
        p.push_back(UniValue("0x" + bt::hex_encode(c.commitment))); // const_hex::serde: 0x-prefixed
        return tagged("BitAssetReservation", p);
    }
    case FilledContent::DutchAuctionReceipt: return tagged("DutchAuctionReceipt", hash(c.id));
    }
    return UniValue();
}

UniValue output(const Output& o)
{
    UniValue r = obj();
    r.pushKV("address", address(o.address));
    r.pushKV("content", content(o.content));
    r.pushKV("memo", bt::hex_encode(o.memo));
    return r;
}

UniValue filled_output(const FilledOutput& o)
{
    UniValue r = obj();
    r.pushKV("address", address(o.address));
    r.pushKV("content", filled_content(o.content));
    r.pushKV("memo", bt::hex_encode(o.memo));
    return r;
}

UniValue inpoint(const InPoint& i)
{
    UniValue x = obj();
    if (i.kind == InPoint::Regular) {
        x.pushKV("txid", hash(i.id));
        x.pushKV("vin", UniValue(uint64_t(i.vin)));
        return tagged("Regular", x);
    }
    x.pushKV("m6id", btc_hash(i.id));
    return tagged("Withdrawal", x);
}

UniValue spent_output(const SpentOutput& s)
{
    UniValue r = obj();
    r.pushKV("output", filled_output(s.output));
    r.pushKV("inpoint", inpoint(s.inpoint));
    return r;
}

UniValue pointed(const OutPoint& op, const UniValue& o)
{
    UniValue r = obj();
    r.pushKV("outpoint", outpoint(op));
    r.pushKV("output", o);
    return r;
}

template <class T, class F>
static UniValue opt(const std::optional<T>& v, F f)
{
    return v ? f(*v) : UniValue();
}

UniValue bitasset_data(const BitAssetData& d)
{
    UniValue r = obj();
    r.pushKV("commitment", opt(d.commitment, [](const Hash32& h) { return bytes_array(h.span()); }));
    r.pushKV("socket_addr_v4", opt(d.socket_addr_v4, [](const SocketV4& s) { return UniValue(s.to_string()); }));
    r.pushKV("socket_addr_v6", opt(d.socket_addr_v6, [](const SocketV6& s) { return UniValue(s.to_string()); }));
    r.pushKV("encryption_pubkey", opt(d.encryption_pubkey, [](const bt::ByteArray<32>& k) { return UniValue(epk_to_string(k)); }));
    r.pushKV("signing_pubkey", opt(d.signing_pubkey, [](const bt::frost::Point& k) { return UniValue(vk_to_string(k)); }));
    return r;
}

template <class T, class F>
static UniValue update(const Update<T>& u, F f)
{
    if (u.kind == Update<T>::Delete) return UniValue("Delete");
    if (u.kind == Update<T>::Retain) return UniValue("Retain");
    return tagged("Set", f(u.value));
}

UniValue updates(const BitAssetDataUpdates& u)
{
    UniValue r = obj();
    r.pushKV("commitment", update(u.commitment, [](const Hash32& h) { return bytes_array(h.span()); }));
    r.pushKV("socket_addr_v4", update(u.socket_addr_v4, [](const SocketV4& s) { return UniValue(s.to_string()); }));
    r.pushKV("socket_addr_v6", update(u.socket_addr_v6, [](const SocketV6& s) { return UniValue(s.to_string()); }));
    r.pushKV("encryption_pubkey", update(u.encryption_pubkey, [](const bt::ByteArray<32>& k) { return UniValue(epk_to_string(k)); }));
    r.pushKV("signing_pubkey", update(u.signing_pubkey, [](const bt::frost::Point& k) { return UniValue(vk_to_string(k)); }));
    return r;
}

UniValue auction_params(const DutchAuctionParams& p)
{
    UniValue r = obj();
    r.pushKV("start_block", UniValue(uint64_t(p.start_block)));
    r.pushKV("duration", UniValue(uint64_t(p.duration)));
    r.pushKV("base_asset", asset_id(p.base_asset));
    r.pushKV("base_amount", UniValue(p.base_amount));
    r.pushKV("quote_asset", asset_id(p.quote_asset));
    r.pushKV("initial_price", UniValue(p.initial_price));
    r.pushKV("final_price", UniValue(p.final_price));
    return r;
}

UniValue signature(const bt::frost::Signature& s)
{
    Bytes b(s.R.begin(), s.R.end());
    b.insert(b.end(), s.z.begin(), s.z.end());
    return UniValue("0x" + bt::hex_encode(b)); // const_hex: 0x-prefixed
}

UniValue txdata(const TxData& d)
{
    UniValue x = obj();
    switch (d.kind) {
    case TxData::AmmBurn:
        x.pushKV("amount0", UniValue(d.amount0));
        x.pushKV("amount1", UniValue(d.amount1));
        x.pushKV("lp_token_burn", UniValue(d.lp_tokens));
        return tagged("AmmBurn", x);
    case TxData::AmmMint:
        x.pushKV("amount0", UniValue(d.amount0));
        x.pushKV("amount1", UniValue(d.amount1));
        x.pushKV("lp_token_mint", UniValue(d.lp_tokens));
        return tagged("AmmMint", x);
    case TxData::AmmSwap:
        x.pushKV("amount_spent", UniValue(d.amount_spent));
        x.pushKV("amount_receive", UniValue(d.amount_receive));
        x.pushKV("pair_asset", asset_id(d.pair_asset));
        return tagged("AmmSwap", x);
    case TxData::BitAssetReservation:
        x.pushKV("commitment", hash(d.commitment));
        return tagged("BitAssetReservation", x);
    case TxData::BitAssetRegistration:
        x.pushKV("name_hash", hash(d.name_hash));
        x.pushKV("revealed_nonce", hash(d.revealed_nonce));
        x.pushKV("bitasset_data", bitasset_data(d.bitasset_data));
        x.pushKV("initial_supply", UniValue(d.initial_supply));
        return tagged("BitAssetRegistration", x);
    case TxData::BitAssetMint: return tagged("BitAssetMint", UniValue(d.mint_amount));
    case TxData::BitAssetUpdate: return tagged("BitAssetUpdate", updates(d.updates));
    case TxData::DutchAuctionCreate: return tagged("DutchAuctionCreate", auction_params(d.params));
    case TxData::DutchAuctionBid:
        x.pushKV("auction_id", hash(d.auction_id));
        x.pushKV("receive_asset", asset_id(d.receive_asset));
        x.pushKV("quantity", UniValue(d.quantity));
        x.pushKV("bid_size", UniValue(d.bid_size));
        return tagged("DutchAuctionBid", x);
    case TxData::DutchAuctionCollect:
        x.pushKV("asset_offered", asset_id(d.asset_offered));
        x.pushKV("asset_receive", asset_id(d.asset_receive));
        x.pushKV("amount_offered_remaining", UniValue(d.amount_offered_remaining));
        x.pushKV("amount_received", UniValue(d.amount_received));
        return tagged("DutchAuctionCollect", x);
    }
    return UniValue();
}

UniValue transaction(const Transaction& t)
{
    UniValue r = obj();
    UniValue ins = arr();
    for (auto& op : t.inputs) ins.push_back(outpoint(op));
    r.pushKV("inputs", ins);
    UniValue outs = arr();
    for (auto& o : t.outputs) outs.push_back(output(o));
    r.pushKV("outputs", outs);
    r.pushKV("memo", bt::hex_encode(t.memo));
    r.pushKV("data", t.data ? txdata(*t.data) : UniValue());
    return r;
}

UniValue authorization(const Authorization& a)
{
    UniValue r = obj();
    r.pushKV("verifying_key", vk_to_string(a.verifying_key));
    r.pushKV("signature", signature(a.signature));
    return r;
}

UniValue authorized_transaction(const AuthorizedTransaction& t)
{
    UniValue r = obj();
    r.pushKV("transaction", transaction(t.transaction));
    UniValue a = arr();
    for (auto& x : t.authorizations) a.push_back(authorization(x));
    r.pushKV("authorizations", a);
    return r;
}

UniValue coinbase(const Coinbase& c)
{
    UniValue r = obj();
    r.pushKV("memo", bt::hex_encode(c.memo));
    UniValue o = arr();
    for (auto& x : c.outputs) o.push_back(output(x));
    r.pushKV("outputs", o);
    return r;
}

UniValue body(const Body& b)
{
    UniValue r = obj();
    r.pushKV("coinbase", coinbase(b.coinbase));
    UniValue txs = arr();
    for (auto& t : b.transactions) txs.push_back(transaction(t));
    r.pushKV("transactions", txs);
    UniValue a = arr();
    for (auto& x : b.authorizations) a.push_back(authorization(x));
    r.pushKV("authorizations", a);
    return r;
}

UniValue header(const Header& h)
{
    UniValue r = obj();
    r.pushKV("merkle_root", hash(h.merkle_root));
    r.pushKV("prev_side_hash", h.prev_side_hash ? hash(*h.prev_side_hash) : UniValue());
    r.pushKV("prev_main_hash", btc_hash(h.prev_main_hash));
    return r;
}

UniValue block(const Header& h, const Body& b, uint32_t height)
{
    UniValue r = obj();
    r.pushKV("header", header(h));
    r.pushKV("body", body(b));
    r.pushKV("height", UniValue(uint64_t(height)));
    return r;
}

UniValue bitcoin_tx(const std::vector<bt::btc::TxOut>& outs)
{
    UniValue r = obj();
    r.pushKV("version", 2);
    r.pushKV("lock_time", 0);
    r.pushKV("input", arr());
    UniValue o = arr();
    for (auto& x : outs) {
        UniValue t = obj();
        t.pushKV("value", UniValue(x.value));
        t.pushKV("script_pubkey", bt::hex_encode(x.script_pubkey));
        o.push_back(t);
    }
    r.pushKV("output", o);
    return r;
}

UniValue withdrawal_bundle(const WithdrawalBundle& w)
{
    UniValue r = obj();
    UniValue su = arr();
    for (auto& [op, o] : w.spend_utxos) {
        UniValue p = arr();
        p.push_back(outpoint(op));
        p.push_back(filled_output(o));
        su.push_back(p);
    }
    r.pushKV("spend_utxos", su);
    r.pushKV("tx", bitcoin_tx(w.outputs));
    return r;
}

UniValue pool_state(const PoolState& p)
{
    UniValue r = obj();
    r.pushKV("reserve0", UniValue(p.reserve0));
    r.pushKV("reserve1", UniValue(p.reserve1));
    r.pushKV("outstanding_lp_tokens", UniValue(p.outstanding_lp_tokens));
    r.pushKV("creation_txid", hash(p.creation_txid));
    return r;
}

template <class T>
static UniValue history(const TxHistory<T>& h)
{
    UniValue a = arr(); // NonEmpty serializes as a sequence
    for (auto& e : h.v) {
        UniValue x = obj();
        x.pushKV("data", UniValue(uint64_t(e.data)));
        x.pushKV("txid", hash(e.txid));
        x.pushKV("height", UniValue(uint64_t(e.height)));
        a.push_back(x);
    }
    return a;
}

UniValue auction_state(const AuctionState& s)
{
    UniValue r = obj();
    r.pushKV("start_block", UniValue(uint64_t(s.start_block)));
    r.pushKV("most_recent_bid_block", history(s.most_recent_bid_block));
    r.pushKV("duration", UniValue(uint64_t(s.duration)));
    r.pushKV("base_asset", asset_id(s.base_asset));
    r.pushKV("initial_base_amount", UniValue(s.initial_base_amount));
    r.pushKV("base_amount_remaining", history(s.base_amount_remaining));
    r.pushKV("quote_asset", asset_id(s.quote_asset));
    r.pushKV("quote_amount", history(s.quote_amount));
    r.pushKV("initial_price", UniValue(s.initial_price));
    r.pushKV("price_after_most_recent_bid", history(s.price_after_most_recent_bid));
    r.pushKV("initial_end_price", UniValue(s.initial_end_price));
    r.pushKV("end_price_after_most_recent_bid", history(s.end_price_after_most_recent_bid));
    return r;
}

UniValue fraction(uint64_t n, uint64_t d)
{
    uint64_t a = n, b = d;
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    if (a > 1) {
        n /= a;
        d /= a;
    }
    UniValue nd = arr();
    nd.push_back(UniValue(n));
    nd.push_back(UniValue(d));
    UniValue v = arr();
    v.push_back(UniValue("Plus"));
    v.push_back(nd);
    return tagged("Rational", v);
}

// ------------------------------------------------------------------ parsing
std::string serde_unexpected(const UniValue& v)
{
    if (v.isNull()) return "null";
    if (v.isBool()) return std::string("boolean `") + (v.get_bool() ? "true" : "false") + "`";
    if (v.isArray()) return "sequence";
    if (v.isObject()) return "map";
    if (v.isStr()) {
        // Rust's Debug of a str.
        std::string r = "string \"";
        for (unsigned char ch : v.get_str()) {
            if (ch == '"') r += "\\\"";
            else if (ch == '\\') r += "\\\\";
            else if (ch == '\n') r += "\\n";
            else if (ch == '\t') r += "\\t";
            else if (ch == '\r') r += "\\r";
            else if (ch < 0x20 || ch == 0x7f) {
                char b[16];
                std::snprintf(b, sizeof(b), "\\u{%x}", ch);
                r += b;
            } else r += char(ch);
        }
        return r + "\"";
    }
    const std::string& s = v.getValStr();
    if (s.find_first_of(".eE") != std::string::npos) {
        // serde_json reads it as an f64; Rust's Display of an f64: the shortest form, no ".0" when integral.
        double d = std::strtod(s.c_str(), nullptr);
        char b[64];
        for (int prec = 1; prec <= 17; prec++) {
            std::snprintf(b, sizeof(b), "%.*g", prec, d);
            if (std::strtod(b, nullptr) == d) break;
        }
        std::string f = b;
        if (f.find('e') != std::string::npos && d == std::floor(d)) {
            std::snprintf(b, sizeof(b), "%.0f", d);
            f = b;
        }
        return "floating point `" + f + "`";
    }
    return "integer `" + s + "`";
}

static uint64_t parse_uint(const UniValue& v, uint64_t max, const char* type)
{
    if (!v.isNum()) throw Error("invalid type: " + serde_unexpected(v) + ", expected " + type, &v);
    const std::string& s = v.getValStr();
    if (s.find_first_of(".eE") != std::string::npos) throw Error("invalid type: " + serde_unexpected(v) + ", expected " + type, &v);
    auto x = s.empty() || s[0] == '-' ? std::nullopt : bt::parse_u64(s);
    if (!x || *x > max) throw Error("invalid value: " + serde_unexpected(v) + ", expected " + type, &v);
    return *x;
}

uint64_t parse_u64(const UniValue& v) { return parse_uint(v, UINT64_MAX, "u64"); }
uint32_t parse_u32(const UniValue& v) { return uint32_t(parse_uint(v, UINT32_MAX, "u32")); }

void require_struct(const UniValue& v, const char* name, std::initializer_list<const char*> fields)
{
    if (!v.isObject()) throw Error("invalid type: " + serde_unexpected(v) + ", expected struct " + name, &v);
    for (const char* f : fields)
        if (!v.exists(f)) throw Error(std::string("missing field `") + f + "`", &v);
}

Bytes parse_hex(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a hex string", &v);
    auto b = bt::hex_decode(v.get_str());
    if (!b) throw Error("invalid hex", &v);
    return *b;
}

// const-hex's decode_to_array, which the 32-byte hashes' serde uses: an optional 0x prefix, then an odd length,
// the length, the first character that is not hex, in its words.
static std::string rust_char_debug(unsigned char b)
{
    switch (b) {
    case 0: return "'\\0'";
    case '\t': return "'\\t'";
    case '\n': return "'\\n'";
    case '\r': return "'\\r'";
    case '\'': return "'\\''";
    case '\\': return "'\\\\'";
    }
    char buf[16];
    if (b < 0x20 || b == 0x7f) {
        std::snprintf(buf, sizeof(buf), "'\\u{%x}'", b);
        return buf;
    }
    if (b < 0x80) return std::string("'") + char(b) + "'";
    return std::string("'") + char(0xc0 | (b >> 6)) + char(0x80 | (b & 0x3f)) + "'";
}

Hash32 parse_hash(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a hex string", &v);
    std::string s = v.get_str();
    if (s.rfind("0x", 0) == 0) s = s.substr(2);
    if (s.size() % 2) throw Error("odd number of digits", &v);
    if (s.size() != 64) throw Error("invalid string length", &v);
    for (size_t i = 0; i < s.size(); i++)
        if (!std::isxdigit((unsigned char)s[i])) throw Error("invalid character " + rust_char_debug((unsigned char)s[i]) + " at position " + std::to_string(i), &v);
    auto h = bt::hex_to_array<32>(s);
    if (!h) throw Error("invalid hash", &v);
    return *h;
}
Hash32 parse_btc_hash(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a hex string", &v);
    auto h = bt::hash_from_rev_hex(v.get_str());
    if (!h) throw Error("invalid hash", &v);
    return *h;
}
Address parse_address(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected an address string", &v);
    // AddressParseError: "bs58 error", or "wrong address length {n} != 20".
    auto d = bt::base58_decode(v.get_str());
    if (!d) throw Error("bs58 error", &v);
    if (d->size() != 20) throw Error("wrong address length " + std::to_string(d->size()) + " != 20", &v);
    auto a = Address::from_base58(v.get_str());
    if (!a) throw Error("invalid address", &v);
    return *a;
}
bt::btc::Address parse_btc_address(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a bitcoin address", &v);
    auto a = bt::btc::Address::parse(v.get_str());
    // rust-bitcoin 0.32 falls back to base58 when bech32 does not decode: "base58 error".
    if (!a) throw Error(bt::base58check_decode(v.get_str()) ? "invalid bitcoin address" : "base58 error", &v);
    return *a;
}

static Hash32 parse_byte_array32(const UniValue& v)
{
    if (!v.isArray() || v.size() != 32) throw Error("expected an array of 32 bytes", &v);
    Hash32 h;
    for (size_t i = 0; i < 32; i++) {
        uint64_t b = parse_u64(v[i]);
        if (b > 255) throw Error("byte out of range", &v);
        h[i] = uint8_t(b);
    }
    return h;
}

OutPoint parse_outpoint(const UniValue& v)
{
    if (!v.isObject() || v.size() != 1) throw Error("invalid OutPoint", &v);
    const std::string& k = v.getKeys()[0];
    const UniValue& x = v.getValues()[0];
    OutPoint o;
    if (k == "Deposit") {
        if (!x.isStr()) throw Error("invalid deposit outpoint", &v);
        auto s = x.get_str();
        auto c = s.rfind(':');
        if (c == std::string::npos) throw Error("invalid deposit outpoint", &v);
        o.kind = OutPoint::Deposit;
        o.txid = parse_btc_hash(UniValue(s.substr(0, c)));
        auto vout = bt::parse_u64(s.substr(c + 1));
        if (!vout || *vout > UINT32_MAX) throw Error("invalid deposit outpoint", &v);
        o.vout = uint32_t(*vout);
        return o;
    }
    if (k != "Regular" && k != "Coinbase") throw Error("invalid OutPoint variant", &v);
    o.kind = k == "Regular" ? OutPoint::Regular : OutPoint::Coinbase;
    o.txid = parse_hash(x["txid"]);
    o.vout = parse_u32(x["vout"]);
    return o;
}

// const_hex::decode / decode_to_array (1.19.1): odd length (before the 0x prefix is stripped),
// then the length of an array, then the first non-hex character, in its words.
static std::optional<Bytes> const_hex_decode(const std::string& in, std::optional<size_t> n, std::string& err)
{
    if (in.size() % 2) {
        err = "odd number of digits";
        return std::nullopt;
    }
    std::string s = in.rfind("0x", 0) == 0 ? in.substr(2) : in;
    if (n && s.size() != 2 * *n) {
        err = "invalid string length";
        return std::nullopt;
    }
    for (size_t i = 0; i < s.size(); i++)
        if (!std::isxdigit((unsigned char)s[i])) {
            err = "invalid character " + rust_char_debug((unsigned char)s[i]) + " at position " + std::to_string(i);
            return std::nullopt;
        }
    return bt::hex_decode(s);
}

AssetId parse_asset_id(const UniValue& v)
{
    if (!v.isStr()) throw Error("invalid type: " + serde_unexpected(v) + ", expected a string", &v);
    std::string err;
    auto b = const_hex_decode(v.get_str(), std::nullopt, err);
    if (!b) throw Error(err, &v);
    // borsh::from_slice, with borsh's io::Error texts
    if (b->empty()) throw Error("Unexpected length of input", &v);
    if ((*b)[0] > 2) throw Error("Unexpected variant tag: " + std::to_string((*b)[0]), &v);
    size_t need = (*b)[0] == 0 ? 1 : 33;
    if (b->size() < need) throw Error("Unexpected length of input", &v);
    if (b->size() > need) throw Error("Not all bytes read", &v);
    return *AssetId::from_borsh(*b);
}

static Content parse_content(const UniValue& c)
{
    Content r;
    if (c.isStr()) {
        if (c.get_str() == "BitAssetControl") r.kind = Content::BitAssetControl;
        else if (c.get_str() == "BitAssetReservation") r.kind = Content::BitAssetReservation;
        else if (c.get_str() == "DutchAuctionReceipt") r.kind = Content::DutchAuctionReceipt;
        else throw Error("invalid content variant", &c);
        return r;
    }
    if (!c.isObject() || c.size() != 1) throw Error("invalid content", &c);
    const std::string& k = c.getKeys()[0];
    const UniValue& x = c.getValues()[0];
    if (k == "BitcoinSats") {
        r.kind = Content::Bitcoin;
        r.value = parse_u64(x);
    } else if (k == "AmmLpToken") {
        r.kind = Content::AmmLpToken;
        r.value = parse_u64(x);
    } else if (k == "BitAsset") {
        r.kind = Content::BitAsset;
        r.value = parse_u64(x);
    } else if (k == "Withdrawal") {
        r.kind = Content::Withdrawal;
        r.value = parse_u64(x["value_sats"]);
        r.main_fee = parse_u64(x["main_fee_sats"]);
        r.main_address = parse_btc_address(x["main_address"]);
    } else {
        throw Error("invalid content variant", &c);
    }
    return r;
}

Output parse_output(const UniValue& v)
{
    if (!v.isObject()) throw Error("invalid output", &v);
    Output o;
    o.address = parse_address(v["address"]);
    o.content = parse_content(v["content"]);
    o.memo = parse_hex(v["memo"]);
    return o;
}

bt::frost::Point parse_vk(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a verifying key", &v);
    auto k = vk_from_string(v.get_str());
    if (!k) throw Error("invalid verifying key", &v);
    return *k;
}

bt::ByteArray<32> parse_epk(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected an encryption pubkey", &v);
    auto k = epk_from_string(v.get_str());
    if (!k) throw Error("parsing failed", &v); // their EncryptionPubKey's FromStr error
    return *k;
}

static SocketV4 parse_v4(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a socket address", &v);
    auto s = SocketV4::parse(v.get_str());
    if (!s) throw Error("invalid socket address", &v);
    return *s;
}
static SocketV6 parse_v6(const UniValue& v)
{
    if (!v.isStr()) throw Error("expected a socket address", &v);
    auto s = SocketV6::parse(v.get_str());
    if (!s) throw Error("invalid socket address", &v);
    return *s;
}

BitAssetData parse_bitasset_data(const UniValue& v)
{
    BitAssetData d;
    if (v.isNull()) return d;
    if (!v.isObject()) throw Error("invalid bitasset data", &v);
    auto f = [&](const char* k) -> const UniValue* {
        const UniValue& x = v[k];
        return x.isNull() ? nullptr : &x;
    };
    if (auto x = f("commitment")) d.commitment = parse_byte_array32(*x);
    if (auto x = f("socket_addr_v4")) d.socket_addr_v4 = parse_v4(*x);
    if (auto x = f("socket_addr_v6")) d.socket_addr_v6 = parse_v6(*x);
    if (auto x = f("encryption_pubkey")) d.encryption_pubkey = parse_epk(*x);
    if (auto x = f("signing_pubkey")) d.signing_pubkey = parse_vk(*x);
    return d;
}

DutchAuctionParams parse_auction_params(const UniValue& v)
{
    require_struct(v, "DutchAuctionParams", {"start_block", "duration", "base_asset", "base_amount", "quote_asset", "initial_price", "final_price"});
    DutchAuctionParams p;
    p.start_block = parse_u32(v["start_block"]);
    p.duration = parse_u32(v["duration"]);
    p.base_asset = parse_asset_id(v["base_asset"]);
    p.base_amount = parse_u64(v["base_amount"]);
    p.quote_asset = parse_asset_id(v["quote_asset"]);
    p.initial_price = parse_u64(v["initial_price"]);
    p.final_price = parse_u64(v["final_price"]);
    return p;
}

template <class T, class F>
static Update<T> parse_update(const UniValue& v, F f)
{
    Update<T> u;
    if (v.isStr()) {
        if (v.get_str() == "Delete") u.kind = Update<T>::Delete;
        else if (v.get_str() == "Retain") u.kind = Update<T>::Retain;
        else throw Error("invalid update", &v);
        return u;
    }
    if (!v.isObject() || v.size() != 1 || v.getKeys()[0] != "Set") throw Error("invalid update", &v);
    u.kind = Update<T>::Set;
    u.value = f(v.getValues()[0]);
    return u;
}

bt::frost::Signature parse_signature(const UniValue& v)
{
    // const_hex::serde into [u8; 64] (an optional 0x prefix); a frost decoding error is reported
    // as InvalidStringLength too (authorization.rs Signature::from_hex)
    if (!v.isStr()) throw Error("invalid type: " + serde_unexpected(v) + ", expected a string", &v);
    std::string err;
    auto b = const_hex_decode(v.get_str(), 64, err);
    if (!b) throw Error(err, &v);
    bt::frost::Signature s;
    std::memcpy(s.R.data(), b->data(), 32);
    std::memcpy(s.z.data(), b->data() + 32, 32);
    if (!bt::frost::valid_element(s.R) || !bt::frost::canonical_scalar(s.z)) throw Error("invalid string length", &v);
    return s;
}

TxData parse_txdata(const UniValue& v)
{
    if (!v.isObject() || v.size() != 1) throw Error("invalid transaction data", &v);
    const std::string& k = v.getKeys()[0];
    const UniValue& x = v.getValues()[0];
    TxData d;
    if (k == "AmmBurn" || k == "AmmMint") {
        d.kind = k == "AmmBurn" ? TxData::AmmBurn : TxData::AmmMint;
        d.amount0 = parse_u64(x["amount0"]);
        d.amount1 = parse_u64(x["amount1"]);
        d.lp_tokens = parse_u64(x[k == "AmmBurn" ? "lp_token_burn" : "lp_token_mint"]);
    } else if (k == "AmmSwap") {
        d.kind = TxData::AmmSwap;
        d.amount_spent = parse_u64(x["amount_spent"]);
        d.amount_receive = parse_u64(x["amount_receive"]);
        d.pair_asset = parse_asset_id(x["pair_asset"]);
    } else if (k == "BitAssetReservation") {
        d.kind = TxData::BitAssetReservation;
        d.commitment = parse_hash(x["commitment"]);
    } else if (k == "BitAssetRegistration") {
        d.kind = TxData::BitAssetRegistration;
        d.name_hash = parse_hash(x["name_hash"]);
        d.revealed_nonce = parse_hash(x["revealed_nonce"]);
        d.bitasset_data = parse_bitasset_data(x["bitasset_data"]);
        d.initial_supply = parse_u64(x["initial_supply"]);
    } else if (k == "BitAssetMint") {
        d.kind = TxData::BitAssetMint;
        d.mint_amount = parse_u64(x);
    } else if (k == "BitAssetUpdate") {
        d.kind = TxData::BitAssetUpdate;
        d.updates.commitment = parse_update<Hash32>(x["commitment"], parse_byte_array32);
        d.updates.socket_addr_v4 = parse_update<SocketV4>(x["socket_addr_v4"], parse_v4);
        d.updates.socket_addr_v6 = parse_update<SocketV6>(x["socket_addr_v6"], parse_v6);
        d.updates.encryption_pubkey = parse_update<bt::ByteArray<32>>(x["encryption_pubkey"], parse_epk);
        d.updates.signing_pubkey = parse_update<bt::frost::Point>(x["signing_pubkey"], parse_vk);
    } else if (k == "DutchAuctionCreate") {
        d.kind = TxData::DutchAuctionCreate;
        d.params = parse_auction_params(x);
    } else if (k == "DutchAuctionBid") {
        d.kind = TxData::DutchAuctionBid;
        d.auction_id = parse_hash(x["auction_id"]);
        d.receive_asset = parse_asset_id(x["receive_asset"]);
        d.quantity = parse_u64(x["quantity"]);
        d.bid_size = parse_u64(x["bid_size"]);
    } else if (k == "DutchAuctionCollect") {
        d.kind = TxData::DutchAuctionCollect;
        d.asset_offered = parse_asset_id(x["asset_offered"]);
        d.asset_receive = parse_asset_id(x["asset_receive"]);
        d.amount_offered_remaining = parse_u64(x["amount_offered_remaining"]);
        d.amount_received = parse_u64(x["amount_received"]);
    } else {
        throw Error("invalid transaction data variant", &v);
    }
    return d;
}

Transaction parse_transaction(const UniValue& v)
{
    require_struct(v, "Transaction", {"inputs", "outputs", "memo"});
    Transaction t;
    for (auto& in : v["inputs"].getValues()) t.inputs.push_back(parse_outpoint(in));
    for (auto& o : v["outputs"].getValues()) t.outputs.push_back(parse_output(o));
    t.memo = parse_hex(v["memo"]);
    if (!v["data"].isNull()) t.data = parse_txdata(v["data"]);
    return t;
}

static Authorization parse_authorization(const UniValue& v)
{
    if (!v.isObject()) throw Error("invalid authorization", &v);
    return Authorization{parse_vk(v["verifying_key"]), parse_signature(v["signature"])};
}

AuthorizedTransaction parse_authorized_transaction(const UniValue& v)
{
    require_struct(v, "Authorized", {"transaction", "authorizations"});
    AuthorizedTransaction t;
    t.transaction = parse_transaction(v["transaction"]);
    for (auto& a : v["authorizations"].getValues()) t.authorizations.push_back(parse_authorization(a));
    return t;
}

Header parse_header(const UniValue& v)
{
    require_struct(v, "Header", {"merkle_root", "prev_main_hash"});
    Header h;
    h.merkle_root = parse_hash(v["merkle_root"]);
    if (!v["prev_side_hash"].isNull()) h.prev_side_hash = parse_hash(v["prev_side_hash"]);
    h.prev_main_hash = parse_btc_hash(v["prev_main_hash"]);
    return h;
}

Body parse_body(const UniValue& v)
{
    require_struct(v, "Body", {"coinbase", "transactions", "authorizations"});
    Body b;
    const UniValue& cb = v["coinbase"];
    b.coinbase.memo = parse_hex(cb["memo"]);
    for (auto& o : cb["outputs"].getValues()) b.coinbase.outputs.push_back(parse_output(o));
    for (auto& t : v["transactions"].getValues()) b.transactions.push_back(parse_transaction(t));
    for (auto& a : v["authorizations"].getValues()) b.authorizations.push_back(parse_authorization(a));
    return b;
}

} // namespace bitassets::json
