// Copyright (c) 2026 The Backbone developers
// Distributed under the MIT software license.
//
// BitAssets' traits for the core's archive, P2P, peg and node templates, and its network
// constants (fork lib/net/peer/message.rs:19-37,316-321, lib/net/mod.rs:150-195, app/cli.rs).
#pragma once

#include <bitassets/state.h>
#include <bitassets/types.h>

#include <bt/sidechain/archive.h>
#include <bt/sidechain/p2p.h>

namespace bitassets {

using bt::sidechain::BmmResult;
using bt::sidechain::Network;
using bt::sidechain::TipRef;
using bt::sidechain::U256;

/** "BTAST" Baudot prefix; Betanet = cc065804, Regtest 00, Signet 01, Forknet 02. */
inline std::array<uint8_t, 4> magic_bytes(Network n) { return bt::sidechain::magic_bytes({0xcc, 0x06, 0x58, 0x00}, n); }
constexpr uint16_t DEFAULT_PORT = 4000 + THIS_SIDECHAIN;     // P2P
constexpr uint16_t DEFAULT_RPC_PORT = 6000 + THIS_SIDECHAIN; // JSON-RPC
constexpr uint16_t DEFAULT_ZMQ_PORT = 28000 + THIS_SIDECHAIN;
/** fork lib/net/mod.rs: seed peers per network. */
std::vector<std::string> seed_peers(Network n);
inline const char* VERSION = "0.17.1";

struct Chain {
    using Header = bitassets::Header;
    using Body = bitassets::Body;
    using Transaction = bitassets::Transaction;
    using AuthorizedTransaction = bitassets::AuthorizedTransaction;
    using FilledOutput = bitassets::FilledOutput;
    using Address = bitassets::Address;
    using State = bitassets::State;
    static constexpr size_t MIN_HEADER_BYTES = 32 + 1 + 8 + 32;
    static constexpr uint8_t SLOT = THIS_SIDECHAIN;
    static constexpr uint16_t DEFAULT_PORT = bitassets::DEFAULT_PORT;
    static constexpr const char* STORE_FILE = "backbone-bitassets.sqlite";
    static constexpr size_t MAX_BODY_SIZE = Body::MAX_SIZE;
    static Hash32 hash(const Header& h) { return h.hash(); }
    static void encode_header(Writer& w, const Header& h) { bincode(w, h); }
    static Header decode_header(Reader& r) { return de_header(r); }
    static void encode_body(Writer& w, const Body& b) { bincode(w, b); }
    static Body decode_body(Reader& r) { return de_body(r); }
    static void encode_authorized_tx(Writer& w, const AuthorizedTransaction& t) { bincode(w, t); }
    static AuthorizedTransaction decode_authorized_tx(Reader& r) { return de_authorized_transaction(r); }
    static std::vector<Hash32> txids(const Body& b)
    {
        std::vector<Hash32> r;
        for (auto& t : b.transactions) r.push_back(t.txid());
        return r;
    }
    static std::array<uint8_t, 4> magic(Network n) { return magic_bytes(n); }
    static std::vector<std::string> seed_peers(Network n) { return bitassets::seed_peers(n); }
    static bt::sidechain::Version p2p_version() { return bt::sidechain::Version{0, 17, 1}; }
    static size_t body_size(const Body& b) { return borsh_bytes(b).size(); }
};

using Archive = bt::sidechain::Archive<Chain>;
using P2P = bt::sidechain::P2P<Chain>;

} // namespace bitassets
