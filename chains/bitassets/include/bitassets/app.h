// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// BitAssets application: node + wallet, block templates, BMM mining, deposits (fork app/app.rs)
// and the JSON-RPC surface (rpc-api/lib.rs, app/rpc_server.rs): the 66 methods their node
// serves, with serde_json shapes.
#pragma once

#include <bitassets/wallet.h>

#include <bt/jsonrpc.h>

namespace bitassets {

struct BlockTemplate {
    uint64_t bribe = 0;
    Header header;
    uint32_t height = 0;
    Body body;
    uint64_t fees = 0;
};

class App {
public:
    App(Node& node, Wallet& wallet) : node(node), wallet(wallet) {}
    Node& node;
    Wallet& wallet;

    static constexpr uint64_t EMPTY_BLOCK_BMM_BRIBE = 1000; // app.rs:423
    BlockTemplate build_block_template(std::optional<uint64_t> fee);
    void mine(std::optional<uint64_t> fee);
    bool connect_block(const Header& h, const Body& b, const Hash32& main_block_hash);
    Hash32 deposit(const Address& a, uint64_t value, uint64_t fee);
    std::function<void()> on_stop;

    // state queries (node/mod.rs), under the node lock
    std::vector<std::tuple<uint32_t, Hash32, BitAssetData>> bitassets();
    std::optional<BitAssetData> bitasset_data(const Hash32& id);
    std::optional<PoolState> pool(const AssetId& a0, const AssetId& a1);
    std::vector<std::pair<Hash32, AuctionState>> auctions();
    std::optional<AuctionState> auction(const Hash32& id);
};

/** Register the RPC methods (rpc_server.rs run_server): the public server gets the node API,
 *  the private one the private node API and the wallet; one server gets all. */
void register_rpc(bt::jsonrpc::Server& s, App& app, bool public_api, bool private_api);

} // namespace bitassets
