// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// BitAssets' app layer (doc/app.md): what our app of BitAssets and the unified wallet need of this node, on the app
// RPC (bt/appui), beside the LayerTwo JSON-RPC, which it does not touch. It reads and acts through the same Node,
// Wallet and App the LayerTwo methods use: the LayerTwo reads are served as they are (the same handlers, the same
// JSON), every spend is a confirmed method (asked in the app's trusted dialog, bound to a one-time token, run by the
// LayerTwo handler), and the wallet's history, the explorer, the peg, the assets, the pools, the auctions, the link to
// the mainchain and the backup phrase are host methods of the pages.
#pragma once

#include <bitassets/app.h>

#include <bt/appui/dropin.h>
#include <bt/appui/host.h>
#include <bt/appui/rpcserver.h>
#include <bt/jsonrpc.h>

#include <memory>
#include <string>

namespace bitassets {

class AppLayer {
public:
    /** `network`: the LayerTwo name of the network (betanet, regtest...). */
    AppLayer(App& app, std::string network, std::string datadir, bt::appui::AppOptions options);
    ~AppLayer();
    AppLayer(const AppLayer&) = delete;
    AppLayer& operator=(const AppLayer&) = delete;

    bool start(std::string& err);
    void stop();
    uint16_t port() const;

    /** The LayerTwo methods the pages read (the same handlers and JSON as the LayerTwo server's). */
    static const std::vector<std::string>& l2l_reads();
    /** Every method of the app RPC, host method and confirmed method: the pack's manifest is checked against it. */
    std::vector<std::string> app_methods() const;
    UniValue manifest() const;

    /** The host, for tests (webuihost without HTTP). */
    bt::appui::RemoteHost& host() { return *host_; }
    bt::appui::RpcServer& server() { return *server_; }

    /** The wallet's history (dropin.history): one row a transaction touching the wallet, newest first. */
    UniValue history(size_t limit);

private:
    struct Impl;
    std::unique_ptr<Impl> m_;
    std::unique_ptr<bt::appui::RpcServer> server_;
    std::unique_ptr<bt::appui::RemoteHost> host_;
};

/** Why a BitAsset name cannot be reserved or registered through our app ("" when it can): 1 to 255 bytes of valid
 *  UTF-8, no control character, no mark that turns or hides text. LayerTwo's rules take any text. */
std::string asset_name_problem(const std::string& name);

/** The price of an auction for its whole remaining base at a block height (the bid's own formula, without bidding), or
 *  nothing when it has not started, has ended, or its state is one a bid would refuse. */
std::optional<uint64_t> auction_price_at(const AuctionState& a, uint32_t height);

} // namespace bitassets
