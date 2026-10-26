# bitassets-app: our app on the BitAssets drop-in

`bitassets-app` is BitAssets with our sidechain app: the sidechain template's window, trusted dialog and interface kit,
with pages for BitAssets' own data model (UTXOs, assets reserved then registered, AMM pools, Dutch auctions,
withdrawal bundles, eCash as the mainchain). The node is `bitassets`, next to the app, unchanged: the app starts it as
its child and talks to its app RPC. Consensus, P2P and the LayerTwo JSON-RPC (plain_bitassets 0.17.1, 66 methods)
are the drop-in's own and stay byte for byte what they are; the app layer is off unless the node is started with
`--bt-app-rpc-port`.

The design shared by every drop-in app (why a thin client, the app RPC, the privileges, one backup phrase by BIP-85,
the pack, security, tests) is [core/doc/app.md](../core/doc/app.md). This page is BitAssets' part.

## Build and run

```
cmake -S . -B build-app -DBT_BUILD_APP=ON -DBT_KIT_DIR=<sidechain-template repo>   # GTK 3 + WebKitGTK 4.1, npm
cmake --build build-app -j4
build-app/chains/bitassets/bitassets-app              # betanet, the enforcer at http://localhost:50051
build-app/chains/bitassets/bitassets-app --network regtest --mainchain-grpc-url http://127.0.0.1:50051 --node-arg=--bt-no-zmq
cmake --build build-app --target bitassets-app-pack   # build-app/pack/bitassets: the pack for the unified wallet
```

The manifest (`app/manifest.json`) gives the app the node's own ways: LayerTwo's data folder
(`<data_dir>/plain_bitassets`, `datadirName`), its RPC address as `--rpc-host`/`--rpc-port` and the private pair
(`addressFlags: host-port`), the mainchain interface as `--bt-mainchain-grpc-url`. The node's ZMQ publisher keeps
LayerTwo's default (127.0.0.1:28004); `--node-arg=--bt-no-zmq` turns it off (a second node on the same machine needs
that or its own `--zmq-addr`).

## The pages

The drop-in pages (`core/app/ui`: Home, Overview, Send, Receive, Activity, Drivechains, the explorer, the node,
Settings → Node and → Wallet), with plain-bitassets' names for their spends (`transfer`, `withdraw`), and BitAssets'
own (`app/chain`):

- **My assets**: what the wallet holds of each asset (a crown where it holds the control coin), its pool shares, the
  auctions it runs (collect once ended), names reserved and waiting to be registered; **Send an asset**
  (`transfer_bitasset [dest, asset_id, amount, fee]`).
- **Issue an asset**: is the name free (`bitassets.resolve`)? **Reserve** it (`reserve_bitasset [name]`: only a
  commitment goes on the chain), then once in a block **register** it (`register_bitasset [name, supply, data?]`: its
  whole supply and its control coin to this wallet, optional IPv4/IPv6 records). Our app asks for an ECX coin before
  reserving: LayerTwo's wallet reserves without an input when it has none.
- **Browse assets**: every asset (by its id: the chain keeps the hash of a name, not the name; the names the wallet
  knows, or looked up, in words to a reader of the wallet only), its supply, records, pools and auctions; a name looked
  up.
- **Trade**: **Swap** (`amm_swap [asset_spend, asset_receive, amount_spend]`, after the node's quote by the chain's own
  arithmetic, the price impact), **Pools**, **Liquidity** (`amm_mint [asset0, asset1, amount0, amount1]` opens or adds,
  `amm_burn [asset0, asset1, lp_tokens]` takes out).
- **Auctions**: every auction with its price now for all that is left (the chain's formula), **Bid**
  (`dutch_auction_bid [id, bid_size]`), **Collect** (`dutch_auction_collect [id]`); **Sell by auction**
  (`dutch_auction_create [params]`, its price curve drawn).
- Home's **My assets** card; the activity's rows of assets (`asset-issue`, `asset-send`, `swap`, `pool-add`,
  `auction-bid`, ...) with what they moved (`HistoryRow.assets`), the explorer's outputs with their asset.

Asset names and records are text anyone chose: shown as data (`SafeText`), checked by core's `display_text_problem`
before a trusted dialog shows them (the page's twin, `dropin/text.ts`), every CSV cell through the kit's `toCsv`.
Amounts of an asset are whole numbers (BitAssets have no decimals) as exact text; a page sends at most 2^53 − 1.

**What the app trades.** Pools and auctions of two BitAssets. The chain's rules as reproduced cannot pay ECX out of a
pool or an auction, so the app offers none with ECX (a pool or an auction with ECX made elsewhere is shown, read only).
Two kept wallet fixes (consensus unchanged, as the Dutch-auction collect): a swap pays the BitAsset received together
with one coin of it the wallet spends (so the wallet needs some of it first), and taking liquidity out spends one coin
of each asset of the pool with the LP tokens; the pages say so before asking. Each describer builds the wallet's
transaction first without taking addresses (`Wallet::PreviewScope`) and runs the chain's own checks of the next block
on it (`validate_filled`, `template_check`, a store transaction never committed): what the chain would refuse is
refused before any question.

**Deposits** go to the bare address of the wallet (Drivechains shows it, and says so): BitAssets credits the plain
base58 address (and its own `s4_…` form); Backbone's deposit writes the text it is given, and a deposit to anything
else is lost.

## What the pages use of the node (app/manifest.json)

- **Reads** (the LayerTwo handlers, the same JSON): `bitasset_data`, `bitassets`, `bitcoin_balance`, `dutch_auctions`,
  `format_deposit_address`, `get_amm_pool_state`, `get_amm_price`, `get_best_mainchain_block_hash`,
  `get_best_sidechain_block_hash`, `get_block`, `get_block_hash`, `get_block_index`, `get_bmm_inclusions`,
  `get_stxos`, `get_transaction`, `get_transaction_info`, `get_utxos`, `get_wallet_addresses`, `get_wallet_utxos`,
  `getblockcount`, `latest_failed_withdrawal_bundle_height`, `list_mempool`, `list_peers`, `list_utxos`,
  `mainchain_sync_progress`, `my_unconfirmed_utxos`, `my_utxos`, `openapi_schema`, `pending_withdrawal_bundle`,
  `sidechain_wealth`; and `getmainchaininfo`, `getbalances`, `getblockchaininfo`, `getnetworkinfo`, `uptime`.
- **Confirmed** (only through `rpc.confirmed`): `transfer`, `transfer_many`, `withdraw`, `create_deposit`, `mine`,
  `reserve_bitasset`, `register_bitasset`, `transfer_bitasset`, `amm_mint`, `amm_burn`, `amm_swap`,
  `dutch_auction_create`, `dutch_auction_bid`, `dutch_auction_collect`.
- **Host**: the shared `dropin.*`; `bitassets.assets` (exercises `bitassets`), `bitassets.asset` (`bitasset_data`,
  `get_amm_pool_state`, `dutch_auctions`), `bitassets.resolve` (`bitasset_data`), `bitassets.wallet`
  (`get_wallet_utxos`, `bitassets`), `bitassets.pools` and `bitassets.quote` (`get_amm_pool_state`),
  `bitassets.auctions` and `bitassets.bidQuote` (`dutch_auctions`). What the wallet holds, its control coins, its
  receipts and the names it knows: only to a caller of `get_wallet_addresses`.

## One backup phrase

The wallet is plain-bitassets' (BIP-39, rust-bitcoin's BIP-32 master), so the unified wallet's BIP-85 phrase works as
for Thunder (`getwalletphraseinfo`, `setwalletphrase`, derivation `plain_bitassets`). Vectors (master "abandon ×11
about"):

| stem, network | index | phrase | fingerprint |
|---|---|---|---|
| bitassets, betanet | 967615207 | enforce vibrant burst island hire blanket run address walnut sword target item | 6b4de6e79e7faeaf |
| bitassets, regtest | 915557307 | skirt direct any common pass truly ball run skate afraid oil vibrant | 8bc6b4cf47ae2f1e |

## Tests

- `tests/test_bitassets_appui.cpp`: every LayerTwo method's reply byte-identical with and without the app layer (66
  methods, bad and good parameters), the app RPC's reads equal to the LayerTwo results, the phrase flows and the
  vectors above (the wallet then has the addresses LayerTwo's `set_seed_from_mnemonic` gives), ownership only for a
  wallet reader, every describer's sum refused near UINT64_MAX and above 21,000,000 ECX; on a chain built in the test
  (blocks through the chain's own prevalidate and connect): two assets reserved and registered (each asked with what
  it does), one sent, a pool opened, a swap, liquidity taken out, an auction refused for the chain's rules, created,
  bid on, collected, each described as it runs and connected; the history's kinds; the swap and burn builders alone;
  the manifest (every host method with its privileges).
- `app/chain/ba.test.ts` (vitest, with the shared `dropin.test.ts` and `text.test.ts`): issue, swap, send an asset,
  sell by auction through `rpc.confirmed` with LayerTwo's parameters, nothing outside the manifest, the amounts' and
  prices' arithmetic, names; `npx svelte-check --fail-on-warnings`. The assembled interface's `node_modules` is a link
  to the template's: vitest then needs `server.fs.allow` to name it (see backbone-zside's doc/app.md).
- `tests/e2e/run_app.py` (a build with `-DBT_APP_TEST_HOOKS=ON`; xvfb-run): a local eCash regtest (backboned
  `-regtest -ecashregtest=betanet -grpc`, slot 4 proposed and acked), bitassets-app runs node A, a headless BitAssets
  node B is the other side: the app starts, the phrase is made in the trusted dialog, 10 ECX deposited (to the bare
  address), 1.5 ECX sent, 0.25 ECX received, `gold` and `silver` reserved and registered, 50,000 silver sent to node
  B, a pool of both opened, 1,000 gold swapped for silver, an auction of 10,000 gold for silver started, node B bids
  1,000 silver on it, the app collects it, 2 ECX withdrawn and paid on eCash; then A's LayerTwo replies byte-identical
  without and with the app layer. Screenshots of the main pages.

```
python3 tests/e2e/run_app.py --bin build-app/chains/bitassets --backboned <backboned> --tmp <dir> --shots <dir> --base-port 49500
```

## Platforms

Linux (GTK 3, WebKitGTK 4.1): built, unit-tested, run end to end. Windows (mingw-w64, WebView2): `bitassets.exe` and
`bitassets-app.exe` cross-compile (`-DCMAKE_TOOLCHAIN_FILE=core/cmake/mingw64.cmake -DBT_BUILD_APP=ON
-DWEBVIEW2_INCLUDE_DIR=<WebView2 headers>`, the mingw64 depends prefix); not yet run on Windows.
