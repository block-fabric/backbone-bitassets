// BitAssets' app layer (doc/app.md): the LayerTwo JSON-RPC is the same with the app layer running (same methods, same
// bytes for every reply); the app RPC's reads are the LayerTwo handlers' own JSON; the backup phrase's flows (made, its
// words only in a question, restored, refused over another) and the unified wallet's BIP-85 phrase of BitAssets; wallet
// ownership only for a wallet reader; every spend refused before any question when its amounts wrap or pass the money
// supply; on a chain built here: assets issued, sent, a pool opened, a swap (the wallet's fix: a coin of the received
// asset), liquidity taken out, an auction created, bid on and collected, each described as it runs; the manifest of
// app/manifest.json is the node's.
#include "test.h"

#include <bitassets/app.h>
#include <bitassets/appui.h>
#include <bitassets/json.h>

#include <bt/appui/auth.h>
#include <bt/appui/phrase.h>
#include <bt/frost.h>
#include <bt/hd.h>

#include <filesystem>
#include <fstream>
#include <memory>

using namespace bitassets;

namespace {
const char* ABANDON = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";

struct Harness {
    std::string dir;
    std::unique_ptr<Node> node;
    std::unique_ptr<Wallet> wallet;
    std::unique_ptr<App> app;
    explicit Harness(const std::string& name)
    {
        bt::frost::init();
        dir = (std::filesystem::temp_directory_path() / ("bt-bitassets-appui-" + name + "-" + bt::appui::random_hex(6))).string();
        std::filesystem::create_directories(dir);
        bt::sidechain::NodeConfig cfg;
        cfg.datadir = dir;
        cfg.network = Network::Regtest;
        cfg.p2p = false;
        cfg.use_seeds = false;
        cfg.zmq_addr.reset();
        cfg.mainchain_grpc_url = "http://127.0.0.1:1";
        node = std::make_unique<Node>(cfg);
        std::string err;
        CHECK(node->start(err));
        wallet = std::make_unique<Wallet>(dir + "/backbone-wallet.sqlite", *node);
        app = std::make_unique<App>(*node, *wallet);
    }
    ~Harness()
    {
        app.reset();
        wallet.reset();
        if (node) node->stop();
        node.reset();
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }

    /** A block of these transactions, connected to the node's state directly (no mainchain here, so no BMM): the chain's
     *  own checks (prevalidate, connect) as for any block. */
    void connect(const std::vector<Transaction>& txs)
    {
        std::vector<AuthorizedTransaction> ats;
        for (const Transaction& tx : txs) ats.push_back(wallet->authorize(tx));
        std::lock_guard<std::recursive_mutex> l(node->lock());
        bt::kv::Txn t(node->store());
        Body b{Body::from_authorized(ats, Coinbase{})};
        Header h;
        h.prev_side_hash = node->state().tip(t);
        std::vector<FilledTransaction> filled;
        for (auto& tx : b.transactions) filled.push_back(node->state().fill(t, tx));
        h.merkle_root = *compute_merkle_root(b.coinbase, filled);
        h.prev_main_hash.data()[0] = uint8_t(node->state().height(t).value_or(0) + 1);
        auto pre{node->state().prevalidate(t, h, b)};
        node->state().connect_prevalidated(t, h, b, pre);
        // In the archive too (the explorer's and the history's reads: blocks, inclusions of transactions).
        node->archive().put_header(t, h);
        node->archive().put_body(t, h.hash(), b);
        t.commit();
    }

    std::map<Hash32, uint64_t> held()
    {
        std::map<Hash32, uint64_t> r;
        for (auto& [op, o] : wallet->utxos())
            if (o.content.is_bitasset()) r[o.content.id] += o.content.value;
        return r;
    }
};

std::string reply(bt::jsonrpc::Server& s, const std::string& method, const std::string& params)
{
    return s.handle(R"({"jsonrpc":"2.0","id":7,"method":")" + method + R"(","params":)" + params + "}");
}

UniValue host_call(AppLayer& layer, const std::string& user, const std::string& method, const std::string& params, const std::string& token = "")
{
    UniValue req{UniValue::VOBJ};
    req.pushKV("kind", "host");
    req.pushKV("method", method);
    UniValue p;
    CHECK(p.read(params));
    req.pushKV("params", p);
    return layer.host().run(req, token, "", UniValue{}, user);
}

/** The question of a spend for a caller of every spend (the cookie), or the error it is refused with. */
std::pair<UniValue, std::string> describe(AppLayer& layer, const std::string& method, const std::string& params)
{
    UniValue p;
    CHECK(p.read(params));
    try {
        return {layer.host().describe(method, p, "__cookie__"), ""};
    } catch (const std::exception& e) {
        return {UniValue{}, e.what()};
    }
}

std::string row(const UniValue& question, const std::string& label)
{
    const UniValue& q{question.exists("question") ? question["question"] : question};
    for (const UniValue& r : q["rows"].getValues()) {
        if (r["label"].get_str() == label) return r["value"].get_str();
    }
    return "(none)";
}

bt::appui::AppOptions options()
{
    bt::appui::AppOptions o;
    o.enabled = true;
    o.port = 0;
    // The app's user, a reader of the chain only, the cookie unrestricted.
    o.whitelist = {"app:webuihost,describeconfirmation,webuievents,getblockchaininfo,getbalances,get_wallet_addresses,dropin.checkAddress,dropin.block,dropin.seed.status,dropin.seed.prepare,dropin.seed.create,dropin.seed.restore,dropin.seed.show,transfer,bitcoin_balance,get_block,bitassets,bitasset_data,get_wallet_utxos,dutch_auctions,get_amm_pool_state,bitassets.assets,bitassets.wallet,bitassets.resolve,reserve_bitasset,register_bitasset",
                   "chainonly:webuihost,getblockchaininfo,get_block,dropin.checkAddress,bitassets,bitasset_data,dutch_auctions,get_amm_pool_state"};
    o.whitelist_default = false;
    return o;
}

std::string hexid(const Hash32& h) { return bt::hex_encode(h); }
std::string asset_hex(const Hash32& h) { return json::asset_id(AssetId::bitasset(h)).get_str(); }
} // namespace

TEST(appui_l2l_rpc_byte_identical_with_and_without_the_app_layer)
{
    Harness h("l2l");
    // Without the app layer: the LayerTwo server as main.cpp makes it.
    bt::jsonrpc::Server before;
    register_rpc(before, *h.app, true, true);
    // With it running (its own server, its pollers, its store) beside the same node and wallet.
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    bt::jsonrpc::Server after;
    register_rpc(after, *h.app, true, true);
    CHECK(before.methods() == after.methods());
    CHECK_EQ(before.methods().size(), size_t{66});
    // Every method, with no parameters, with wrong ones, and (the reads) with right ones: the same bytes.
    const std::vector<std::string> bad{"[]", "[1,2,3,4,5]", R"(["x"])", R"({"nope":1})", "[null]", R"(["00", "01", 1, 2])"};
    for (const std::string& m : before.methods()) {
        // What changes the node or the wallet runs once only (on `after`, after the comparison of its errors).
        static const std::set<std::string> changes{"stop", "mine", "get_new_address", "generate_mnemonic", "set_seed_from_mnemonic", "create_deposit",
                                                   "transfer", "transfer_many", "withdraw", "connect_peer", "forget_peer", "invalidate_block",
                                                   "submit_transaction", "connect_block", "get_block_template", "sign_transaction", "reserve_bitasset",
                                                   "register_bitasset", "get_new_encryption_key", "get_new_verifying_key", "broadcast_transaction",
                                                   "rebroadcast_transaction", "encrypt_msg", "sign_arbitrary_msg", "sign_arbitrary_msg_as_addr",
                                                   "transfer_bitasset", "amm_mint", "amm_burn", "amm_swap", "dutch_auction_create", "dutch_auction_bid",
                                                   "dutch_auction_collect", "remove_from_mempool"};
        for (const std::string& p : bad) {
            if (changes.contains(m) && (p == "[]" || p == "[null]")) continue;
            // A fresh phrase or a ciphertext of fresh randomness each call: the same form, not the same bytes.
            if (m == "generate_mnemonic" || m == "encrypt_msg") {
                UniValue a, b;
                CHECK(a.read(reply(before, m, p)));
                CHECK(b.read(reply(after, m, p)));
                CHECK(a.getKeys() == b.getKeys());
                continue;
            }
            const std::string a{reply(before, m, p)}, b{reply(after, m, p)};
            if (a != b) std::fprintf(stderr, "  %s %s:\n    %s\n    %s\n", m.c_str(), p.c_str(), a.c_str(), b.c_str());
            CHECK(a == b);
        }
    }
    const std::string zero{"0000000000000000000000000000000000000000000000000000000000000000"};
    const std::vector<std::pair<std::string, std::string>> reads{
        {"getblockcount", "[]"}, {"get_best_sidechain_block_hash", "[]"}, {"get_best_mainchain_block_hash", "[]"}, {"list_utxos", "[]"}, {"list_peers", "[]"},
        {"pending_withdrawal_bundle", "[]"}, {"sidechain_wealth", "[]"}, {"bitcoin_balance", "[]"}, {"get_wallet_addresses", "[]"}, {"get_wallet_utxos", "[]"},
        {"my_utxos", "[]"}, {"my_unconfirmed_utxos", "[]"}, {"openapi_schema", "[]"}, {"latest_failed_withdrawal_bundle_height", "[]"}, {"get_block_hash", "[0]"},
        {"format_deposit_address", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR"])"}, {"get_utxos", R"([["4XrMw8bmnhchFspSyP4XRUuB5vaR"]])"},
        {"get_stxos", R"([["4XrMw8bmnhchFspSyP4XRUuB5vaR"]])"}, {"get_transaction", "[\"" + zero + "\"]"}, {"get_transaction_info", "[\"" + zero + "\"]"},
        {"bitassets", "[]"}, {"bitasset_data", "[\"" + zero + "\"]"}, {"dutch_auctions", "[]"}, {"get_amm_pool_state", R"(["00", "01)" + zero + "\"]"},
        {"get_amm_price", R"(["00", "01)" + zero + "\"]"}, {"list_mempool", "[]"}, {"mainchain_sync_progress", "[]"}, {"get_bmm_inclusions", "[\"" + zero + "\"]"},
        {"get_block", "[\"" + zero + "\"]"}, {"get_block_index", "[\"" + zero + "\"]"}};
    for (const auto& [m, p] : reads) CHECK(reply(before, m, p) == reply(after, m, p));
    // The app RPC's own read of a LayerTwo method answers its result, the same JSON (an error of LayerTwo's, an error).
    for (const auto& [m, p] : reads) {
        UniValue r;
        CHECK(r.read(reply(before, m, p)));
        bt::appui::Call c;
        c.method = m;
        CHECK(c.params.read(p));
        c.user = "__cookie__";
        if (r.exists("error")) {
            bool refused{false};
            try {
                layer.server().dispatch(c);
            } catch (const bt::appui::RpcError&) {
                refused = true;
            }
            CHECK(refused);
            continue;
        }
        CHECK_EQ(layer.server().dispatch(c).write(), r.find_value("result").write());
    }
    // Every page read of the manifest is a LayerTwo read of the node.
    for (const std::string& m : AppLayer::l2l_reads()) CHECK(std::find(before.methods().begin(), before.methods().end(), m) != before.methods().end());
    layer.stop();
}

TEST(appui_phrase_made_in_a_question_restored_refused_over_another)
{
    Harness h("seed");
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    // Made: an id for the page, the words only in the question, the wallet set on the yes.
    const UniValue prep{host_call(layer, "app", "dropin.seed.prepare", "{}")["result"]};
    const std::string id{prep["id"].get_str()};
    CHECK(id.size() == 32);
    CHECK(!prep.exists("phrase") && !prep.exists("words_list"));
    const std::string create{"{\"id\":\"" + id + "\"}"};
    const UniValue q{host_call(layer, "app", "dropin.seed.create", create)["confirm"]};
    CHECK_EQ(q["kind"].get_str(), std::string{"phrase.create"});
    CHECK(q["details"].get_str().find(" 1. ") != std::string::npos);
    CHECK(!h.wallet->has_seed());
    const UniValue done{host_call(layer, "app", "dropin.seed.create", create, q["token"].get_str())["result"]};
    CHECK(h.wallet->has_seed());
    const std::string fp{done["fingerprint"].get_str()};
    CHECK_EQ(host_call(layer, "app", "dropin.seed.status", "{}")["result"]["fingerprint"].get_str(), fp);
    CHECK(host_call(layer, "app", "dropin.seed.status", "{}")["result"]["phraseKept"].isTrue());
    CHECK_EQ(host_call(layer, "app", "dropin.seed.status", "{}")["result"]["from"].get_str(), std::string{"app"});
    // Shown again: a question, never a result with words.
    const UniValue shown{host_call(layer, "app", "dropin.seed.show", "{}")};
    CHECK(shown.exists("confirm") && !shown.exists("result"));
    // Another phrase over it: refused before any question.
    bool refused{false};
    try {
        host_call(layer, "app", "dropin.seed.restore", std::string{R"({"phrase":")"} + ABANDON + "\"}");
    } catch (const bt::appui::RpcError&) {
        refused = true;
    }
    CHECK(refused);
    // The unified wallet's setup (its own user, the cookie): the same refusal over another phrase.
    bt::appui::Call c;
    c.method = "setwalletphrase";
    c.params = UniValue{UniValue::VARR};
    c.params.push_back(ABANDON);
    c.user = "__cookie__";
    refused = false;
    try {
        layer.server().dispatch(c);
    } catch (const bt::appui::RpcError&) {
        refused = true;
    }
    CHECK(refused);
    // The app's user may not call it at all.
    c.user = "app";
    refused = false;
    try {
        layer.server().dispatch(c);
    } catch (const bt::appui::RpcError&) {
        refused = true;
    }
    CHECK(refused);
    layer.stop();
}

TEST(appui_restore_and_unified_phrase_bitassets_vectors)
{
    // The phrases of BitAssets from the master "abandon ×11 about" (BIP-85, sidechain/dropin/v1), as an independent
    // implementation computes them (Python, hardened-only BIP-32, checked against BIP-85's own vector).
    auto master{bt::hd::parse_mnemonic(ABANDON)};
    const auto seed{bt::hd::mnemonic_to_seed(*master)};
    CHECK_EQ(bt::appui::dropin_phrase_index("bitassets", "betanet"), uint32_t{967615207});
    CHECK_EQ(bt::appui::dropin_phrase_index("bitassets", "regtest"), uint32_t{915557307});
    const std::string betanet{bt::appui::dropin_phrase(seed.span(), "bitassets", "betanet")};
    const std::string regtest{bt::appui::dropin_phrase(seed.span(), "bitassets", "regtest")};
    CHECK_EQ(betanet, std::string{"enforce vibrant burst island hire blanket run address walnut sword target item"});
    CHECK_EQ(regtest, std::string{"skirt direct any common pass truly ball run skate afraid oil vibrant"});
    CHECK_EQ(bt::appui::seed_fingerprint(bt::hd::mnemonic_to_seed(*bt::hd::parse_mnemonic(betanet)).span()), std::string{"6b4de6e79e7faeaf"});
    CHECK_EQ(bt::appui::seed_fingerprint(bt::hd::mnemonic_to_seed(*bt::hd::parse_mnemonic(regtest)).span()), std::string{"8bc6b4cf47ae2f1e"});

    Harness h("restore");
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    bt::appui::Call c;
    c.method = "setwalletphrase";
    c.params = UniValue{UniValue::VARR};
    c.params.push_back(regtest);
    c.user = "__cookie__";
    const UniValue r{layer.server().dispatch(c)};
    CHECK_EQ(r["fingerprint"].get_str(), std::string{"8bc6b4cf47ae2f1e"});
    // The fingerprint the node gives is the one the unified wallet computes from the phrase it set.
    bt::appui::Call info;
    info.method = "getwalletphraseinfo";
    info.user = "__cookie__";
    const UniValue i{layer.server().dispatch(info)};
    CHECK_EQ(i["fingerprint"].get_str(), std::string{"8bc6b4cf47ae2f1e"});
    CHECK_EQ(i["from"].get_str(), std::string{"unified"});
    CHECK_EQ(i["derivation"].get_str(), std::string{"plain_bitassets"});
    CHECK(i["phraseKept"].isTrue());
    // The same phrase again: accepted (the same seed).
    CHECK(layer.server().dispatch(c)["fingerprint"].get_str() == "8bc6b4cf47ae2f1e");
    // The wallet's keys are the phrase's: the same addresses as LayerTwo's set_seed_from_mnemonic of it.
    Harness other("restore-l2l");
    other.wallet->set_seed_from_mnemonic(regtest);
    CHECK(h.wallet->get_new_address() == other.wallet->get_new_address());
    // A bad phrase: tiny-bip39's words.
    bt::appui::Call bad{c};
    bad.params = UniValue{UniValue::VARR};
    bad.params.push_back("abandon abandon");
    bool refused{false};
    try {
        layer.server().dispatch(bad);
    } catch (const bt::appui::RpcError& e) {
        refused = std::string{e.what()}.find("word") != std::string::npos || std::string{e.what()}.find("invalid") != std::string::npos;
    }
    CHECK(refused);
    layer.stop();
}

TEST(appui_wallet_ownership_only_for_a_wallet_reader)
{
    Harness h("mine");
    h.wallet->set_seed_from_mnemonic(ABANDON);
    const std::string own{h.wallet->get_new_address().to_base58()};
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    const std::string p{"{\"address\":\"" + own + "\"}"};
    const UniValue reader{host_call(layer, "app", "dropin.checkAddress", p)["result"]};
    CHECK(reader["valid"].isTrue());
    CHECK(reader["mine"].isTrue());
    const UniValue chain_only{host_call(layer, "chainonly", "dropin.checkAddress", p)["result"]};
    CHECK(chain_only["valid"].isTrue());
    CHECK(!chain_only.exists("mine"));
    CHECK(!chain_only.exists("error"));
    // The wallet's holdings: not for a reader of the chain only (refused), the assets without what the wallet holds.
    bool refused{false};
    try {
        host_call(layer, "chainonly", "bitassets.wallet", "{}");
    } catch (const bt::appui::RpcError&) {
        refused = true;
    }
    CHECK(refused);
    // A spend the wallet cannot pay: refused before any question.
    const auto [q, why] = describe(layer, "transfer", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR", 1000, 10])");
    CHECK(why.find("Not enough") != std::string::npos);
    // A reservation without a coin to spend: refused (their wallet would make one anyone can replay).
    const auto [q2, why2] = describe(layer, "reserve_bitasset", R"(["gold"])");
    CHECK(why2.find("ECX coins") != std::string::npos);
    // A name that hides or turns text, or a line break in the trusted dialog: refused.
    CHECK(describe(layer, "reserve_bitasset", "[\"a\\nb\"]").second.find("U+000A") != std::string::npos);
    CHECK(describe(layer, "reserve_bitasset", "[\"a\\u202eb\"]").second.find("U+202E") != std::string::npos);
    CHECK(describe(layer, "reserve_bitasset", "[\"a\\u2028b\"]").second.find("U+2028") != std::string::npos);
    CHECK(describe(layer, "reserve_bitasset", "[\"a\\ue000b\"]").second.find("U+E000") != std::string::npos);
    CHECK(!asset_name_problem("gold").size());
    layer.stop();
}

TEST(appui_spends_refused_when_their_sum_wraps_or_passes_max_money)
{
    Harness h("overflow");
    h.wallet->set_seed_from_mnemonic(ABANDON);
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    const auto overflow{[&layer](const std::string& method, const std::string& params) {
        const auto [q, why] = describe(layer, method, params);
        const bool ok{why.find("amount overflow") != std::string::npos};
        if (!ok) std::fprintf(stderr, "  %s %s: %s\n", method.c_str(), params.c_str(), why.c_str());
        return ok;
    }};
    // 2^63 + 2^63 + (2^63 + 1) wraps past the amount and still looks larger than it: refused before any question.
    CHECK(overflow("withdraw", R"(["bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt", 9223372036854775808, 9223372036854775808, 9223372036854775809])"));
    CHECK(overflow("withdraw", R"(["bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt", 18446744073709551606, 5, 20])"));
    CHECK(overflow("transfer", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR", 18446744073709551615, 1])"));
    CHECK(overflow("transfer", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR", 2100000000000001, 0])"));
    CHECK(overflow("transfer_many", R"([{"4XrMw8bmnhchFspSyP4XRUuB5vaR": 9223372036854775808, "4TXCaVb28BDdZbpF93Gg4GDMhxd9": 9223372036854775808}, 1000])"));
    CHECK(overflow("create_deposit", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR", 18446744073709551615, 1000])"));
    CHECK(overflow("mine", "[2100000000000001]"));
    // A number past u64: refused as a parameter.
    CHECK(describe(layer, "transfer", R"(["4XrMw8bmnhchFspSyP4XRUuB5vaR", 18446744073709551616, 1])").second.find("too large") != std::string::npos);
    // The wallet itself (LayerTwo's withdraw, without the app): each addition checked too.
    bt::jsonrpc::Server l2l;
    register_rpc(l2l, *h.app, true, true);
    const std::string r{reply(l2l, "withdraw", R"(["bcrt1qqypqxpq9qcrsszg2pvxq6rs0zqg3yyc5phstwt", 9223372036854775808, 9223372036854775808, 9223372036854775809])")};
    CHECK(r.find("overflow") != std::string::npos);
    layer.stop();
}

TEST(appui_assets_pools_auctions_on_a_chain)
{
    Harness h("assets");
    h.wallet->set_seed_from_mnemonic(ABANDON);
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    // ---- issue: two names reserved (their reservations in a block), then registered with their supply
    const Hash32 gold{name_hash("gold")}, silver{name_hash("silver")};
    // Registering before the reservation is in a block: refused with the next step.
    CHECK(describe(layer, "register_bitasset", R"(["gold", 1000000])").second.find("Reserve this name first") != std::string::npos);
    h.connect({h.wallet->reserve_bitasset("gold"), h.wallet->reserve_bitasset("silver")});
    {
        const auto [q, why] = describe(layer, "register_bitasset", R"(["gold", 1000000, {"socket_addr_v4": "203.0.113.7:443"}])");
        CHECK_EQ(why, std::string{});
        CHECK_EQ(row(q, "Supply"), std::string{"1,000,000 units"});
        CHECK_EQ(row(q, "IPv4 address"), std::string{"203.0.113.7:443"});
        CHECK_EQ(row(q, "Asset id"), hexid(gold));
    }
    // The wallet's next step is said by bitassets.wallet: both names reserved, in a block, not registered.
    CHECK_EQ(host_call(layer, "app", "bitassets.wallet", "{}")["result"]["pending"].size(), size_t{2});
    h.connect({h.wallet->register_bitasset("gold", BitAssetData{}, 1'000'000), h.wallet->register_bitasset("silver", BitAssetData{}, 1'000'000)});
    CHECK_EQ(h.held()[gold], uint64_t{1'000'000});
    CHECK(describe(layer, "reserve_bitasset", R"(["gold"])").second.find("registered already") != std::string::npos);
    // The assets: what the wallet holds and their names in words only to a reader of the wallet.
    {
        const UniValue mine{host_call(layer, "app", "bitassets.assets", "{}")["result"]};
        const UniValue chain{host_call(layer, "chainonly", "bitassets.assets", "{}")["result"]};
        CHECK_EQ(mine.size(), size_t{2});
        CHECK_EQ(chain.size(), size_t{2});
        for (size_t i{0}; i < mine.size(); ++i) {
            CHECK(mine[i].exists("name") && mine[i].exists("held") && mine[i].exists("control"));
            CHECK(!chain[i].exists("name") && !chain[i].exists("held") && !chain[i].exists("control"));
            CHECK_EQ(chain[i]["supply"].get_str(), std::string{"1000000"});
        }
    }
    // ---- an asset sent: asked with the asset and its amount; more than held: refused before asking
    const std::string other{"4XrMw8bmnhchFspSyP4XRUuB5vaR"};
    {
        const auto [q, why] = describe(layer, "transfer_bitasset", "[\"" + other + "\", \"" + hexid(gold) + "\", 2500, 0]");
        CHECK_EQ(why, std::string{});
        CHECK_EQ(row(q, "Amount"), std::string{"2,500"});
        CHECK_EQ(row(q, "Asset"), std::string{"gold"});
        CHECK(describe(layer, "transfer_bitasset", "[\"" + other + "\", \"" + hexid(gold) + "\", 2000000, 0]").second.find("Not enough gold") != std::string::npos);
        CHECK(describe(layer, "transfer_bitasset", "[\"" + other + "\", \"" + hexid(gold) + "\", 18446744073709551615, 0]").second.find("Not enough gold") != std::string::npos);
        CHECK(describe(layer, "transfer_bitasset", "[\"" + other + "\", \"" + hexid(gold) + "\", 1, 18446744073709551615]").second.find("amount overflow") != std::string::npos);
        h.connect({h.wallet->create_bitasset_transfer(*Address::from_base58(other), gold, 2'500, 0, {})});
        CHECK_EQ(h.held()[gold], uint64_t{997'500});
    }
    // ---- a pool: ECX and control coins are not traded through the app; one of two BitAssets opened, its LP tokens
    const std::string g{asset_hex(gold)}, s{asset_hex(silver)};
    CHECK(describe(layer, "amm_mint", "[\"00\", \"" + g + "\", 1000, 1000]").second.find("BitAssets") != std::string::npos);
    CHECK(describe(layer, "amm_mint", "[\"" + g + "\", \"" + g + "\", 1000, 1000]").second.find("two different") != std::string::npos);
    CHECK(describe(layer, "amm_mint", "[\"" + g + "\", \"" + s + "\", 18446744073709551615, 18446744073709551615]").second.find("Not enough") != std::string::npos);
    const AssetId A0{std::min(AssetId::bitasset(gold), AssetId::bitasset(silver))}, A1{std::max(AssetId::bitasset(gold), AssetId::bitasset(silver))};
    uint64_t lp{0};
    {
        const auto [q, why] = describe(layer, "amm_mint", "[\"" + g + "\", \"" + s + "\", 100000, 400000]");
        CHECK_EQ(why, std::string{});
        CHECK_EQ(row(q, "LP tokens"), std::string{"200,000"});
        CHECK_EQ(q["accept"].get_str(), std::string{"Open the pool"});
        const bool gold_first{A0 == AssetId::bitasset(gold)};
        lp = 200'000;
        h.connect({h.wallet->amm_mint(A0, A1, gold_first ? 100'000 : 400'000, gold_first ? 400'000 : 100'000, lp)});
        CHECK(h.app->pool(A0, A1).has_value());
    }
    // ---- a swap of gold for silver: the quote and the question say the same; the wallet's swap (with one of its silver
    //      coins, the kept fix) is valid on the chain and moves exactly what it said
    {
        const UniValue quote{host_call(layer, "chainonly", "bitassets.quote", "{\"kind\":\"swap\",\"spend\":\"" + g + "\",\"receive\":\"" + s + "\",\"amount\":1000}")["result"]};
        const std::string receive{quote["receive"].get_str()};
        CHECK(std::stoull(receive) > 3'000 && std::stoull(receive) < 4'000);
        const auto [q, why] = describe(layer, "amm_swap", "[\"" + g + "\", \"" + s + "\", 1000]");
        CHECK_EQ(why, std::string{});
        const std::string grouped{receive.size() > 3 ? receive.substr(0, receive.size() - 3) + "," + receive.substr(receive.size() - 3) : receive};
        CHECK_EQ(row(q, "You get"), grouped + " silver");
        const auto before{h.held()};
        h.connect({h.wallet->amm_swap(AssetId::bitasset(gold), AssetId::bitasset(silver), 1'000, std::stoull(receive))});
        const auto after{h.held()};
        CHECK_EQ(after.at(gold), before.at(gold) - 1'000);
        CHECK_EQ(after.at(silver), before.at(silver) + std::stoull(receive));
        CHECK(describe(layer, "amm_swap", "[\"00\", \"" + s + "\", 1000]").second.find("BitAssets") != std::string::npos);
    }
    // ---- half the liquidity taken out
    {
        const auto [q, why] = describe(layer, "amm_burn", "[\"" + g + "\", \"" + s + "\", 100000]");
        CHECK_EQ(why, std::string{});
        CHECK_EQ(row(q, "LP tokens burned"), std::string{"100,000"});
        auto ps{h.app->pool(A0, A1)};
        std::string e;
        auto np{ps->burn(100'000, e)};
        const Transaction burn{h.wallet->amm_burn(A0, A1, ps->reserve0 - np->reserve0, ps->reserve1 - np->reserve1, 100'000)};
        h.connect({burn});
        CHECK_EQ(h.app->pool(A0, A1)->outstanding_lp_tokens, uint64_t{100'000});
    }
    // ---- an auction of gold priced in silver: the chain's rules said before asking; created, bid on, collected
    const uint32_t next{*h.node->height() + 1};
    auto params = [&](uint32_t start, uint32_t duration, uint64_t initial, uint64_t final_) {
        return "[{\"start_block\":" + std::to_string(start) + ",\"duration\":" + std::to_string(duration) + ",\"base_asset\":\"" + g + "\",\"base_amount\":10000,\"quote_asset\":\"" + s +
               "\",\"initial_price\":" + std::to_string(initial) + ",\"final_price\":" + std::to_string(final_) + "}]";
    };
    CHECK(describe(layer, "dutch_auction_create", params(next, 4, 5000, 1000)).second.find("at the earliest") != std::string::npos);
    CHECK(describe(layer, "dutch_auction_create", params(next + 1, 4, 1000, 5000)).second.find("final price") != std::string::npos);
    CHECK(describe(layer, "dutch_auction_create", params(next + 1, 1, 5000, 1000)).second.find("one price") != std::string::npos);
    CHECK(describe(layer, "dutch_auction_create", params(next + 1, 0, 5000, 5000)).second.find("at least one block") != std::string::npos);
    {
        const auto [q, why] = describe(layer, "dutch_auction_create", params(next + 2, 4, 5000, 1000));
        CHECK_EQ(why, std::string{});
        CHECK_EQ(row(q, "Sell"), std::string{"10,000 gold"});
    }
    DutchAuctionParams d{next + 2, 4, AssetId::bitasset(gold), 10'000, AssetId::bitasset(silver), 5'000, 1'000};
    const Transaction create{h.wallet->dutch_auction_create(d)};
    const Hash32 auction{create.txid()};
    h.connect({create});
    // Before it starts: no bid.
    CHECK(describe(layer, "dutch_auction_bid", "[\"" + hexid(auction) + "\", 1000]").second.find("not started") != std::string::npos);
    h.connect({});
    {
        const UniValue bq{host_call(layer, "chainonly", "bitassets.bidQuote", "{\"id\":\"" + hexid(auction) + "\",\"bid\":1000}")["result"]};
        const auto [q, why] = describe(layer, "dutch_auction_bid", "[\"" + hexid(auction) + "\", 1000]");
        CHECK_EQ(why, std::string{});
        CHECK(row(q, "You get").find(" gold") != std::string::npos);
        const uint64_t qty{std::stoull(bq["quantity"].get_str())};
        CHECK(qty > 0);
        auto st{h.app->auction(auction)};
        h.connect({h.wallet->dutch_auction_bid(auction, st->base_asset, st->quote_asset, 1'000, qty)});
    }
    // Not ended yet: the collect is refused with when it can be done.
    CHECK(describe(layer, "dutch_auction_collect", "[\"" + hexid(auction) + "\"]").second.find("not ended") != std::string::npos);
    for (int i{0}; i < 4; ++i) h.connect({});
    {
        const UniValue auctions{host_call(layer, "app", "bitassets.auctions", "{}")["result"]};
        CHECK_EQ(auctions.size(), size_t{1});
        CHECK(auctions[0]["mine"].isTrue());
        CHECK(!host_call(layer, "chainonly", "bitassets.auctions", "{}")["result"][0].exists("mine"));
        const auto [q, why] = describe(layer, "dutch_auction_collect", "[\"" + hexid(auction) + "\"]");
        CHECK_EQ(why, std::string{});
        CHECK_EQ(row(q, "Paid to you"), std::string{"1,000 silver"});
        auto st{h.app->auction(auction)};
        const auto before{h.held()};
        h.connect({h.wallet->dutch_auction_collect(auction, st->base_asset, st->quote_asset, st->base_amount_remaining.latest().data, st->quote_amount.latest().data)});
        const auto after{h.held()};
        CHECK_EQ(after.at(silver), before.at(silver) + 1'000);
    }
    // The wallet's history says what each row was, with the assets it moved.
    {
        const UniValue hist{layer.history(100)};
        std::set<std::string> kinds;
        for (const UniValue& r : hist.getValues()) kinds.insert(r["kind"].get_str());
        for (const char* k : {"asset-reserve", "asset-issue", "asset-send", "pool-add", "swap", "pool-remove", "auction-create", "auction-bid", "auction-collect"}) {
            if (!kinds.contains(k)) std::fprintf(stderr, "  no %s row in the history\n", k);
            CHECK(kinds.contains(k));
        }
    }
    layer.stop();
}

TEST(wallet_amm_swap_with_a_coin_of_the_received_asset_is_valid)
{
    // Wallet::build_amm_swap alone: without a coin of the received BitAsset it refuses (the outputs could not be filled);
    // with one, the received amount is paid out with it, after the spent asset's change.
    const Hash32 x{name_hash("x")}, y{name_hash("y")};
    auto coin = [](uint8_t n, const Hash32& id, uint64_t v) {
        FilledOutput o;
        o.content.kind = FilledContent::BitAsset;
        o.content.id = id;
        o.content.value = v;
        Hash32 txid{};
        txid.data()[0] = n;
        return std::make_pair(OutPoint{OutPoint::Regular, txid, 0}, o);
    };
    const std::array<Address, 2> to{};
    bool refused{false};
    try {
        Wallet::build_amm_swap({coin(1, x, 5'000)}, std::nullopt, AssetId::bitasset(x), AssetId::bitasset(y), 1'000, 900, to);
    } catch (const WalletError&) {
        refused = true;
    }
    CHECK(refused);
    const Transaction tx{Wallet::build_amm_swap({coin(1, x, 5'000)}, coin(2, y, 7), AssetId::bitasset(x), AssetId::bitasset(y), 1'000, 900, to)};
    CHECK_EQ(tx.inputs.size(), size_t{2});
    CHECK_EQ(tx.outputs.size(), size_t{2});
    CHECK_EQ(tx.outputs[0].content.value, uint64_t{4'000});
    CHECK_EQ(tx.outputs[1].content.value, uint64_t{907});
    // No change of the spent asset: the received output alone.
    const Transaction exact{Wallet::build_amm_swap({coin(1, x, 1'000)}, coin(2, y, 7), AssetId::bitasset(x), AssetId::bitasset(y), 1'000, 900, to)};
    CHECK_EQ(exact.outputs.size(), size_t{1});
    // An amount that would wrap with the coin: refused.
    refused = false;
    try {
        Wallet::build_amm_swap({coin(1, x, 1'000)}, coin(2, y, 7), AssetId::bitasset(x), AssetId::bitasset(y), 1'000, UINT64_MAX - 3, to);
    } catch (const WalletError&) {
        refused = true;
    }
    CHECK(refused);
}

TEST(wallet_amm_burn_with_a_coin_of_each_asset_is_valid)
{
    // Wallet::build_amm_burn alone: without a coin of each BitAsset of the pool it refuses (the chain refuses a burn that
    // spends coins of fewer than two BitAssets); with them, each is paid back with the amount taken out, after the LP change.
    const Hash32 x{name_hash("x")}, y{name_hash("y")};
    const AssetId X{AssetId::bitasset(x)}, Y{AssetId::bitasset(y)};
    const AssetId a0{std::min(X, Y)}, a1{std::max(X, Y)};
    auto coin = [](uint8_t n, const Hash32& id, uint64_t v) {
        FilledOutput o;
        o.content.kind = FilledContent::BitAsset;
        o.content.id = id;
        o.content.value = v;
        Hash32 txid{};
        txid.data()[0] = n;
        return std::make_pair(OutPoint{OutPoint::Regular, txid, 0}, o);
    };
    FilledOutput lpo;
    lpo.content.kind = FilledContent::AmmLpToken;
    lpo.content.asset0 = a0;
    lpo.content.asset1 = a1;
    lpo.content.value = 1'000;
    Hash32 lptx{};
    lptx.data()[0] = 9;
    const std::vector<std::pair<OutPoint, FilledOutput>> lp{{OutPoint{OutPoint::Regular, lptx, 0}, lpo}};
    const std::array<Address, 3> to{};
    bool refused{false};
    try {
        Wallet::build_amm_burn(lp, std::nullopt, coin(2, a1.id, 5), a0, a1, 100, 200, 400, to);
    } catch (const WalletError&) {
        refused = true;
    }
    CHECK(refused);
    const Transaction tx{Wallet::build_amm_burn(lp, coin(1, a0.id, 3), coin(2, a1.id, 5), a0, a1, 100, 200, 400, to)};
    CHECK_EQ(tx.inputs.size(), size_t{3});
    REQUIRE(tx.outputs.size() == 3);
    CHECK_EQ(tx.outputs[0].content.value, uint64_t{600});
    CHECK_EQ(tx.outputs[1].content.value, uint64_t{103});
    CHECK_EQ(tx.outputs[2].content.value, uint64_t{205});
    // ECX in a pool: refused (only pools of two BitAssets).
    refused = false;
    try {
        Wallet::build_amm_burn(lp, coin(1, a0.id, 3), coin(2, a1.id, 5), AssetId::bitcoin(), a1, 100, 200, 400, to);
    } catch (const WalletError&) {
        refused = true;
    }
    CHECK(refused);
}

TEST(appui_manifest_is_the_nodes)
{
    Harness h("manifest");
    AppLayer layer(*h.app, "regtest", h.dir, options());
    std::string err;
    CHECK(layer.start(err));
    std::ifstream f(std::string{BT_SOURCE_DIR} + "/app/manifest.json");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    UniValue file;
    CHECK(file.read(text));
    const UniValue node{layer.manifest()};
    for (const char* group : {"reads", "confirmed", "host", "protocol"}) {
        std::set<std::string> a, b;
        for (const UniValue& v : file[group].getValues()) a.insert(v.get_str());
        for (const UniValue& v : node[group].getValues()) b.insert(v.get_str());
        if (a != b) std::fprintf(stderr, "  manifest %s differs\n", group);
        CHECK(a == b);
    }
    // Every read and protocol method is served by the app RPC; every host method of the manifest has its privileges
    // entry (deny by default: a user whose whitelist names none of them may not run it).
    for (const UniValue& v : file["reads"].getValues()) CHECK(layer.server().has(v.get_str()));
    for (const UniValue& v : file["protocol"].getValues()) CHECK(layer.server().has(v.get_str()));
    // (dropin.checkAddress exercises nothing: whether an address is the wallet's is said only to a wallet reader.)
    for (const UniValue& v : file["host"].getValues()) {
        if (v.get_str() != "dropin.checkAddress") CHECK(!layer.host().host_method_allowed(v.get_str(), [](const std::string&) { return false; }));
    }
    // Every confirmed method is described by the node (refused here for its parameters, never unknown).
    for (const UniValue& v : file["confirmed"].getValues()) {
        const auto [q, why] = describe(layer, v.get_str(), "[]");
        CHECK(why.find("not found") == std::string::npos && why.find("Unknown") == std::string::npos);
    }
    CHECK_EQ(file["program"].isNull() ? std::string{"bitassets"} : file["program"].get_str(), std::string{"bitassets"});
    CHECK_EQ(file["datadirName"].get_str(), std::string{"plain_bitassets"});
    CHECK_EQ(file["addressFlags"].get_str(), std::string{"host-port"});
    layer.stop();
}
