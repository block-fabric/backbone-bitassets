# Differential tests against plain-bitassets v0.17.1

Everything here runs LOCALLY (never on servers). Their code comes from
`~/skydoge/ext/plain-bitassets-octobocto` (16f2f69, pinned in `~/skydoge/ext/PINNED.txt`).

## 1. Oracle: their library as the reference (`tools/oracle`)

```
cd tools/oracle
cp ../../../ext/plain-bitassets-octobocto/Cargo.lock .   # their pinned dependency versions
CARGO_TARGET_DIR=../../../ext/plain-bitassets-octobocto/target cargo build --release -j4
ba_oracle <seed> <n> > cases.jsonl                      # n tx cases, n AMM cases, n auction bids
BT_TEST_DATA=<dir containing oracle/cases.jsonl> build/tests/bt_tests oracle
```

The oracle produces random filled transactions. Amounts are drawn from a pool that includes
0, u64::MAX−k, powers of two and nearby values, so that sums cancel and wrap. Each one goes
through their `filled_outputs` (bincode of the result, `null`, or `panic`), `bitcoin_fee` and
`Body::compute_merkle_root`. AMM pools go through `mint`, `burn`, `swap_asset0_for_asset1` and
`swap_asset1_for_asset0`, and Dutch auctions through `bid` (serde JSON of the new state, the
error text, or `panic`). `test_bitassets_oracle.cpp` recomputes every case with our code and
requires the same result.

Result: 240,000 cases from seeds 2..5, with 17,924 panics in theirs, gave 0 differences. 4,500
cases (seed 1) are checked in. The oracle found one hang in our `isqrt_u128` (fixed).

## 2. Regtest pair (`tools/diffba.py`)

One local eCash regtest (`ecashd`, `-listen=0`), one enforcer (`--network-magic=eca5b134`,
`--wallet-sync-source=disabled`), their `plain_bitassets_app` and our `bitassets`, peered with
each other. Both use the same mnemonic. Ports: `PORT_BASE`+0..27, default 49020–49047 (ecashd
49020/49021/49023, enforcer 49030/49031, theirs 49040–49042, ours 49045–49047). After every block, the harness compares the
public RPC surface of the two nodes byte for byte: tips, block hashes, blocks, UTXOs/STXOs,
BitAssets, pools, auctions, bundles, wealth, mempools and transaction info. It also compares
wallet RPCs on the shared seed (addresses, keys, deposit-address formatting) and RPC error texts
for bad inputs.

Scenario:
* blocks mined by each node in turn;
* a deposit from each node;
* transfers made on one node, relayed over P2P and mined by the other;
* reserve, register (with data) and transfer of BitAssets;
* AMM: a pool-creating mint built by hand and signed by their wallet, then mint, swap and burn;
* Dutch auction: create on ours, bid, collect on ours (mined by theirs);
* an upstream mempool behaviour check (details held for coordinated disclosure);
* withdrawal bundles that succeed and that fail;
* `invalidate_block`;
* a mainchain reorg over a sidechain tip.

The harness stops every process it started, by exact PID (also on Ctrl-C and SIGTERM).

Control run: set `OURS=<their binary>` to run the same scenario with their node on both sides.
Every remaining difference in our runs also shows up in that control run (their wallet lag, the
net-task death after `invalidate_block`, their wallet failing to register its own reservation).

Results: see `~/skydoge/audit/2026-10-22-dropin-bitassets-zside.md`.

## 3. Public betanet

* `bitassets-fetch-blocks` fetches every block from a public peer.
* `tests/test_bitassets_replay.cpp` checks re-encoding, ids, roots, the full replay with every
  check on, and rollback.
* Live sync: `bitassets --headless --bt-mainchain-grpc-url https://enforcer.beta.ecash.ninja`
  syncs from an empty datadir to the betanet tip.
