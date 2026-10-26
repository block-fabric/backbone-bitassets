// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// `bitassets-cli`: the subcommands and output of the plain-bitassets fork's
// plain_bitassets_app_cli (cli/lib.rs, shipped as `bitassets-cli`) over the node's JSON-RPC.
// JSON results print like serde_json::to_string_pretty; scalars like Rust's Display.
#include <bitassets/chain.h>

#include <bt/crypto.h>
#include <bt/jsonrpc.h>
#include <bt/parse.h>

#include <optional>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace {

enum class Out { Pretty, Display, Empty };

struct Spec {
    std::string rpc;
    std::vector<std::string> positional;                    // positional CLI args, in order
    std::vector<std::pair<std::string, std::string>> flags; // --flag -> rpc param name
    Out out = Out::Pretty;
    std::vector<std::string> json_args; // args/flags whose value is JSON (numbers, objects)
};

std::map<std::string, Spec> specs()
{
    std::map<std::string, Spec> m;
    auto add = [&](const std::string& cmd, Spec s) { m[cmd] = std::move(s); };
    add("amm-burn", {"amm_burn", {}, {{"--asset0", "asset0"}, {"--asset1", "asset1"}, {"--lp-token-amount", "lp_token_amount"}}, Out::Display, {"lp_token_amount"}});
    add("amm-mint", {"amm_mint", {}, {{"--asset0", "asset0"}, {"--asset1", "asset1"}, {"--amount0", "amount0"}, {"--amount1", "amount1"}}, Out::Display, {"amount0", "amount1"}});
    add("amm-swap", {"amm_swap", {}, {{"--asset-spend", "asset_spend"}, {"--asset-receive", "asset_receive"}, {"--amount-spend", "amount_spend"}}, Out::Display, {"amount_spend"}});
    add("bitasset-data", {"bitasset_data", {"bitasset_id"}, {}, Out::Pretty, {}});
    add("bitassets", {"bitassets", {}, {}, Out::Pretty, {}});
    add("bitcoin-balance", {"bitcoin_balance", {}, {}, Out::Pretty, {}});
    add("connect-block", {"connect_block", {"block", "main_block_hash"}, {}, Out::Display, {"block"}});
    add("connect-peer", {"connect_peer", {"addr"}, {}, Out::Empty, {}});
    add("create-deposit", {"create_deposit", {"address"}, {{"--value-sats", "value_sats"}, {"--fee-sats", "fee_sats"}}, Out::Display, {"value_sats", "fee_sats"}});
    add("decrypt-msg", {"decrypt_msg", {}, {{"--encryption-pubkey", "encryption_pubkey"}, {"--msg", "ciphertext"}}, Out::Display, {}});
    add("dutch-auction-bid", {"dutch_auction_bid", {}, {{"--auction-id", "dutch_auction_id"}, {"--bid-size", "bid_size"}}, Out::Display, {"bid_size"}});
    add("dutch-auctions", {"dutch_auctions", {}, {}, Out::Pretty, {}});
    add("encrypt-msg", {"encrypt_msg", {}, {{"--encryption-pubkey", "encryption_pubkey"}, {"--msg", "msg"}}, Out::Display, {}});
    add("forget-peer", {"forget_peer", {"addr"}, {}, Out::Empty, {}});
    add("format-deposit-address", {"format_deposit_address", {"address"}, {}, Out::Display, {}});
    add("generate-mnemonic", {"generate_mnemonic", {}, {}, Out::Display, {}});
    add("get-amm-pool-state", {"get_amm_pool_state", {}, {{"--asset0", "asset0"}, {"--asset1", "asset1"}}, Out::Pretty, {}});
    add("get-amm-price", {"get_amm_price", {}, {{"--base", "base"}, {"--quote", "quote"}}, Out::Pretty, {}});
    add("get-best-mainchain-block-hash", {"get_best_mainchain_block_hash", {}, {}, Out::Pretty, {}});
    add("get-best-sidechain-block-hash", {"get_best_sidechain_block_hash", {}, {}, Out::Pretty, {}});
    add("get-block", {"get_block", {"block_hash"}, {}, Out::Pretty, {}});
    add("get-blockcount", {"getblockcount", {}, {}, Out::Display, {}});
    add("get-block-hash", {"get_block_hash", {"height"}, {}, Out::Pretty, {"height"}});
    add("get-block-index", {"get_block_index", {"block_hash"}, {}, Out::Pretty, {}});
    add("get-block-template", {"get_block_template", {}, {}, Out::Pretty, {}});
    add("get-bmm-inclusions", {"get_bmm_inclusions", {"block_hash"}, {}, Out::Pretty, {}});
    add("get-new-address", {"get_new_address", {}, {}, Out::Display, {}});
    add("get-new-encryption-key", {"get_new_encryption_key", {}, {}, Out::Display, {}});
    add("get-new-verifying-key", {"get_new_verifying_key", {}, {}, Out::Display, {}});
    add("get-transaction", {"get_transaction", {"txid"}, {}, Out::Pretty, {}});
    add("get-transaction-info", {"get_transaction_info", {"txid"}, {}, Out::Pretty, {}});
    add("get-wallet-addresses", {"get_wallet_addresses", {}, {}, Out::Pretty, {}});
    add("get-wallet-utxos", {"get_wallet_utxos", {}, {}, Out::Pretty, {}});
    add("invalidate-block", {"invalidate_block", {"block_hash"}, {}, Out::Empty, {}});
    add("latest-failed-withdrawal-bundle-height", {"latest_failed_withdrawal_bundle_height", {}, {}, Out::Pretty, {}});
    add("list-mempool", {"list_mempool", {}, {}, Out::Pretty, {}});
    add("list-peers", {"list_peers", {}, {}, Out::Pretty, {}});
    add("list-utxos", {"list_utxos", {}, {}, Out::Pretty, {}});
    add("mainchain-sync-progress", {"mainchain_sync_progress", {}, {}, Out::Pretty, {}});
    add("mine", {"mine", {}, {{"--fee-sats", "fee"}}, Out::Empty, {"fee"}});
    add("my-unconfirmed-utxos", {"my_unconfirmed_utxos", {}, {}, Out::Pretty, {}});
    add("my-utxos", {"my_utxos", {}, {}, Out::Pretty, {}});
    add("openapi-schema", {"openapi_schema", {}, {}, Out::Pretty, {}});
    add("pending-withdrawal-bundle", {"pending_withdrawal_bundle", {}, {}, Out::Pretty, {}});
    add("remove-from-mempool", {"remove_from_mempool", {"txid"}, {}, Out::Empty, {}});
    add("reserve-bitasset", {"reserve_bitasset", {"plain_name"}, {}, Out::Display, {}});
    add("set-seed-from-mnemonic", {"set_seed_from_mnemonic", {"mnemonic"}, {}, Out::Empty, {}});
    add("sidechain-wealth", {"sidechain_wealth", {}, {}, Out::Display, {}});
    add("sign-arbitrary-msg", {"sign_arbitrary_msg", {}, {{"--verifying-key", "verifying_key"}, {"--msg", "msg"}}, Out::Display, {}});
    add("sign-arbitrary-msg-as-addr", {"sign_arbitrary_msg_as_addr", {}, {{"--address", "address"}, {"--msg", "msg"}}, Out::Pretty, {}});
    add("sign-transaction", {"sign_transaction", {"transaction", "broadcast"}, {}, Out::Pretty, {"transaction", "broadcast"}});
    add("submit-transaction", {"submit_transaction", {"transaction"}, {}, Out::Display, {"transaction"}});
    add("stop", {"stop", {}, {}, Out::Empty, {}});
    add("transfer", {"transfer", {"dest"}, {{"--value-sats", "value"}, {"--fee-sats", "fee"}}, Out::Display, {"value", "fee"}});
    add("transfer-many", {"transfer_many", {"dests"}, {{"--fee-sats", "fee_sats"}}, Out::Display, {"dests", "fee_sats"}});
    add("transfer-bitasset", {"transfer_bitasset", {"dest"}, {{"--asset-id", "asset_id"}, {"--amount", "amount"}, {"--fee-sats", "fee_sats"}}, Out::Display, {"amount", "fee_sats"}});
    add("verify-signature", {"verify_signature", {},
                             {{"--signature", "signature"}, {"--verifying-key", "verifying_key"}, {"--dst", "dst"}, {"--msg", "msg"}},
                             Out::Display, {}});
    add("withdraw", {"withdraw", {"mainchain_address"},
                     {{"--amount-sats", "amount_sats"}, {"--fee-sats", "fee_sats"}, {"--mainchain-fee-sats", "mainchain_fee_sats"}},
                     Out::Display, {"amount_sats", "fee_sats", "mainchain_fee_sats"}});
    return m;
}

void json_str(std::string& o, const std::string& s)
{
    o += '"';
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        default:
            if (c < 0x20) {
                char b[8];
                std::snprintf(b, sizeof(b), "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    o += '"';
}

// serde_json::to_string_pretty: two-space indent, "key": value, empty containers inline.
void pretty(std::string& o, const UniValue& v, int indent)
{
    auto pad = [&](int n) { o.append(size_t(n) * 2, ' '); };
    if (v.isObject()) {
        if (v.empty()) {
            o += "{}";
            return;
        }
        o += "{\n";
        for (size_t i = 0; i < v.size(); i++) {
            pad(indent + 1);
            json_str(o, v.getKeys()[i]);
            o += ": ";
            pretty(o, v.getValues()[i], indent + 1);
            o += i + 1 < v.size() ? ",\n" : "\n";
        }
        pad(indent);
        o += "}";
    } else if (v.isArray()) {
        if (v.empty()) {
            o += "[]";
            return;
        }
        o += "[\n";
        for (size_t i = 0; i < v.size(); i++) {
            pad(indent + 1);
            pretty(o, v[i], indent + 1);
            o += i + 1 < v.size() ? ",\n" : "\n";
        }
        pad(indent);
        o += "]";
    } else if (v.isStr()) {
        json_str(o, v.get_str());
    } else {
        o += v.write();
    }
}

bool parse_url(const std::string& url, std::string& host, uint16_t& port)
{
    std::string r = url;
    if (r.rfind("http://", 0) == 0) r = r.substr(7);
    auto slash = r.find('/');
    if (slash != std::string::npos) r = r.substr(0, slash);
    auto hp = bt::split_hostport(r, uint16_t(80));
    if (!hp) return false;
    host = hp->first;
    port = hp->second;
    return true;
}

int fail(const std::string& m)
{
    std::fprintf(stderr, "Error: %s\n", m.c_str());
    return 1;
}

UniValue rpc(const std::string& host, uint16_t port, const std::string& method, const UniValue& params, std::string& err)
{
    UniValue resp = bt::jsonrpc::call(host, port, method, params, err);
    if (!err.empty()) return UniValue();
    if (!resp["error"].isNull()) {
        err = resp["error"]["message"].getValStr();
        return UniValue();
    }
    return resp["result"];
}

} // namespace

int main(int argc, char** argv)
{
    std::string url = "http://127.0.0.1:" + std::to_string(bitassets::DEFAULT_RPC_PORT);
    std::string rpc_host = "127.0.0.1", rpc_port = std::to_string(bitassets::DEFAULT_RPC_PORT);
    bool host_port = false;
    int i = 1;
    for (; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--rpc-url" && i + 1 < argc) url = argv[++i];
        else if (a == "--rpc-host" && i + 1 < argc) {
            rpc_host = argv[++i];
            host_port = true;
        } else if (a == "--rpc-port" && i + 1 < argc) {
            rpc_port = argv[++i];
            host_port = true;
        }
        else if (a.rfind("--rpc-url=", 0) == 0) url = a.substr(10);
        else if (a == "--timeout" && i + 1 < argc) ++i;
        else if (a.rfind("--timeout=", 0) == 0) {
        } else if (a == "-v" || a == "--verbose") {
        } else break;
    }
    auto all = specs();
    if (i >= argc || std::string(argv[i]) == "--help" || std::string(argv[i]) == "help") {
        std::printf("Usage: bitassets-cli [--rpc-host HOST] [--rpc-port PORT] [--rpc-url URL] [--timeout SECS] <COMMAND>\n\nCommands:\n");
        for (auto& [k, _] : all) std::printf("  %s\n", k.c_str());
        for (const char* k : {"dutch-auction-collect", "dutch-auction-create", "get-stxos", "get-utxos", "register-bitasset"}) std::printf("  %s\n", k);
        return argc > i ? 0 : 2;
    }
    if (std::string(argv[i]) == "-V" || std::string(argv[i]) == "--version") {
        std::printf("plain_bitassets_app_cli %s\n", bitassets::VERSION);
        return 0;
    }
    if (host_port) url = "http://" + rpc_host + ":" + rpc_port;
    std::string host;
    uint16_t port;
    if (!parse_url(url, host, port)) return fail("invalid --rpc-url");
    std::string cmd = argv[i++];
    UniValue params(UniValue::VOBJ);
    std::string err;
    if (cmd == "get-stxos" || cmd == "get-utxos") {
        UniValue a(UniValue::VARR);
        for (; i < argc; i++) a.push_back(argv[i]);
        if (a.empty()) {
            std::fprintf(stderr, "error: the following required arguments were not provided: <ADDRESSES>...\n");
            return 2;
        }
        params.pushKV("addresses", a);
        UniValue r = rpc(host, port, cmd == "get-stxos" ? "get_stxos" : "get_utxos", params, err);
        if (!err.empty()) return fail(err);
        std::string o;
        pretty(o, r, 0);
        std::printf("%s\n", o.c_str());
        return 0;
    }
    if (cmd == "register-bitasset" || cmd == "dutch-auction-create" || cmd == "dutch-auction-collect") {
        // RegisterBitasset { plaintext_name, --initial-supply, #[command(flatten)] BitAssetData };
        // DutchAuctionCreate { #[command(flatten)] DutchAuctionParams }; DutchAuctionCollect { auction_id }
        static const std::map<std::string, std::string> data_fields = {
            {"--commitment", "commitment"}, {"--socket-addr-v4", "socket_addr_v4"}, {"--socket-addr-v6", "socket_addr_v6"},
            {"--encryption-pubkey", "encryption_pubkey"}, {"--signing-pubkey", "signing_pubkey"}};
        static const std::map<std::string, std::string> param_fields = {
            {"--start-block", "start_block"}, {"--duration", "duration"}, {"--base-asset", "base_asset"}, {"--base-amount", "base_amount"},
            {"--quote-asset", "quote_asset"}, {"--initial-price", "initial_price"}, {"--final-price", "final_price"}};
        UniValue data(UniValue::VOBJ);
        std::string name;
        std::optional<uint64_t> supply;
        for (; i < argc; i++) {
            std::string a = argv[i];
            if (cmd == "register-bitasset" && a == "--initial-supply" && i + 1 < argc) {
                supply = bt::parse_u64(argv[++i]);
                if (!supply) return fail("invalid --initial-supply");
                continue;
            }
            auto& fields = cmd == "register-bitasset" ? data_fields : param_fields;
            auto it = fields.find(a);
            if (it != fields.end() && i + 1 < argc) {
                std::string v = argv[++i];
                if (it->second == "commitment") {
                    auto h = bt::hex_to_array<32>(v);
                    if (!h) return fail("invalid --commitment");
                    UniValue arr(UniValue::VARR);
                    for (auto b : *h) arr.push_back(int(b));
                    data.pushKV("commitment", arr);
                } else if (cmd == "dutch-auction-create" && it->second != "base_asset" && it->second != "quote_asset") {
                    auto n = bt::parse_u64(v);
                    if (!n) return fail("invalid " + a);
                    data.pushKV(it->second, UniValue(*n));
                } else {
                    data.pushKV(it->second, v);
                }
            } else if (cmd != "dutch-auction-create" && name.empty()) {
                name = a;
            } else {
                std::fprintf(stderr, "error: unexpected argument '%s'\n", a.c_str());
                return 2;
            }
        }
        if (cmd == "register-bitasset") {
            if (name.empty() || !supply) {
                std::fprintf(stderr, "error: usage: register-bitasset <PLAINTEXT_NAME> --initial-supply <N> [BitAssetData flags]\n");
                return 2;
            }
            params.pushKV("plain_name", name);
            params.pushKV("initial_supply", UniValue(*supply));
            params.pushKV("bitasset_data", data);
            UniValue r = rpc(host, port, "register_bitasset", params, err);
            if (!err.empty()) return fail(err);
            std::printf("%s\n", r.getValStr().c_str());
        } else if (cmd == "dutch-auction-create") {
            params.pushKV("dutch_auction_params", data);
            UniValue r = rpc(host, port, "dutch_auction_create", params, err);
            if (!err.empty()) return fail(err);
            std::printf("%s\n", r.getValStr().c_str());
        } else {
            if (name.empty()) return fail("usage: dutch-auction-collect <AUCTION_ID>");
            params.pushKV("dutch_auction_id", name);
            UniValue r = rpc(host, port, "dutch_auction_collect", params, err);
            if (!err.empty()) return fail(err);
            UniValue o(UniValue::VOBJ);
            o.pushKV("amount0", r[0]);
            o.pushKV("amount1", r[1]);
            std::string out;
            pretty(out, o, 0);
            std::printf("%s\n", out.c_str());
        }
        return 0;
    }
    auto it = all.find(cmd);
    if (it == all.end()) {
        std::fprintf(stderr, "error: unrecognized subcommand '%s'\n", cmd.c_str());
        return 2;
    }
    const Spec& spec = it->second;
    size_t pos = 0;
    bool utf8 = false;
    for (; i < argc; i++) {
        std::string a = argv[i];
        if (cmd == "decrypt-msg" && a == "--utf8") {
            utf8 = true;
            continue;
        }
        std::string fname, fval;
        bool is_flag = false;
        for (auto& [flag, name] : spec.flags) {
            if (a == flag && i + 1 < argc) {
                fname = name;
                fval = argv[++i];
                is_flag = true;
            } else if (a.rfind(flag + "=", 0) == 0) {
                fname = name;
                fval = a.substr(flag.size() + 1);
                is_flag = true;
            }
            if (is_flag) break;
        }
        std::string name = fname, value = fval;
        if (!is_flag) {
            if (pos >= spec.positional.size()) {
                std::fprintf(stderr, "error: unexpected argument '%s'\n", a.c_str());
                return 2;
            }
            name = spec.positional[pos++];
            value = a;
        }
        bool is_json = std::find(spec.json_args.begin(), spec.json_args.end(), name) != spec.json_args.end();
        UniValue v;
        if (is_json) {
            if (!v.read(value)) {
                std::fprintf(stderr, "error: invalid value '%s' for '<%s>'\n", value.c_str(), name.c_str());
                return 2;
            }
        } else {
            v = UniValue(value);
        }
        params.pushKV(name, v);
    }
    if (cmd == "sign-transaction" && !params.exists("broadcast")) params.pushKV("broadcast", false);
    UniValue r = rpc(host, port, spec.rpc, params, err);
    if (!err.empty()) return fail(err);
    switch (spec.out) {
    case Out::Pretty: {
        std::string o;
        pretty(o, r, 0);
        std::printf("%s\n", o.c_str());
        break;
    }
    case Out::Display:
        if (utf8 && r.isStr()) {
            auto b = bt::hex_decode(r.get_str());
            if (!b) return fail("invalid hex");
            std::printf("%s\n", std::string(b->begin(), b->end()).c_str());
        } else {
            std::printf("%s\n", r.isStr() ? r.get_str().c_str() : r.write().c_str());
        }
        break;
    case Out::Empty: std::printf("\n"); break;
    }
    return 0;
}
