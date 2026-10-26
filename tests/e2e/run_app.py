#!/usr/bin/env python3
# Copyright (c) 2026 The Backbone developers
# Distributed under the MIT software license.
"""End-to-end run of bitassets-app on a local eCash regtest (doc/app.md "Tests"): slot 4, app.js in its page; the runner
is core/tests/e2e/dropin_app.py. Besides deposit, payments both ways, receipt and a withdrawal paid on eCash: two
assets issued (reserved, registered), one sent to node B, a pool opened, a swap, a Dutch auction started by the app,
node B bids on it (here, through its LayerTwo RPC), the app collects it.

    run_app.py --bin build-app/chains/bitassets --backboned <backboned> --tmp <dir> --shots <dir> [--base-port 49500]
"""

import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "core" / "tests" / "e2e"))
from dropin_app import Chain, host_port_flags, main  # noqa: E402
from dropin_regtest import wait_for  # noqa: E402

READS = [("getblockcount", []), ("get_best_sidechain_block_hash", []), ("get_best_mainchain_block_hash", []), ("list_utxos", []),
         ("pending_withdrawal_bundle", []), ("latest_failed_withdrawal_bundle_height", []), ("sidechain_wealth", []), ("bitcoin_balance", []),
         ("get_wallet_addresses", []), ("get_wallet_utxos", []), ("my_utxos", []), ("my_unconfirmed_utxos", []), ("openapi_schema", []),
         ("list_peers", []), ("bitassets", []), ("dutch_auctions", []), ("list_mempool", []), ("mainchain_sync_progress", [])]


def on_signal(msg, run):
    """SIGNAL auction-created <id> <start>: node B (no app) bids 1,000 of the quote asset on it once it is open."""
    words = msg.split()
    if words[0] != "auction-created":
        return []
    auction, start = words[1], int(words[2])
    # B's next block (getblockcount: the tip's height + 1 on BitAssets) is at least the auction's first one.
    wait_for(lambda: run.b.getblockcount() >= start, "the auction open on node B", 300, 1)
    got = wait_for(lambda: run.b.dutch_auction_bid(auction, 1000), "node B's bid", 120, 2)
    print(f"  node B bids 1,000 for {got} of the auction {auction[:16]}…", flush=True)
    return []


def more_reads(a, addrs):
    reads = [("bitasset_data", [x[1]]) for x in a.bitassets()[:3]]
    ids = sorted("01" + x[1] for x in a.bitassets()[:2])
    if len(ids) == 2:
        reads += [("get_amm_pool_state", ids), ("get_amm_price", ids)]
    tip = a.get_best_sidechain_block_hash()
    reads += [("get_block_index", [tip])]
    return reads


if __name__ == "__main__":
    sys.exit(main(Chain(stem="bitassets", title="BitAssets", slot=4, script=HERE / "app.js", reads=READS, url_flag="--bt-mainchain-grpc-url",
                        node_args=["--bt-no-zmq"], addresses=host_port_flags, on_signal=on_signal, more_reads=more_reads,
                        peer_available=lambda b: b.bitcoin_balance()["available_sats"],
                        peer_pay=lambda b, to, sats: b.transfer(to, sats, 1000))))
