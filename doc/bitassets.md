# BitAssets (eCash slot 4): drop-in client

`bitassets` replaces the octobocto fork of plain-bitassets (`plain_bitassets_app`) v0.17.1
(16f2f69, submodule 7958ceff) on eCash betanet. Consensus, P2P, mainchain link, CLI flags and
JSON-RPC follow that tag: it is the code every betanet BitAssets block was made under.

## Status

| Milestone | State | Evidence |
|---|---|---|
| Codecs and hashes | done | All 1,080 betanet blocks (0..1079), fetched from the seed `204.168.254.113:4004` over QUIC with `bitassets-fetch-blocks`. Each one decodes and re-encodes byte-exact. Ids and merkle roots match the seed index for heights 1..1079 (the index's height-0 row is a stale pre-0.17 leftover). Empty-body root is `812c9708…bdbb`. |
| State machine | done | Full replay with every check on (`tests/test_bitassets_replay.cpp`): 1,080 BMM commitments verified (genesis BMM at mainchain 968,864), 18 signatures, 17 tx inclusions (14 distinct), 8 reservations, 2 registrations, 7 transfers, 6 deposits. Tip `195ef084…`: 27 UTXOs, 18 STXOs, 2 BitAssets (`e6cc6c14…` seq 0 supply 21,000,000; `94e441d4…` seq 1 supply 1), 3 open reservations, no pools or auctions, wealth 133,074,500 sat. The pending bundle's m6id equals the enforcer's Submitted `0fe864bd…` (mainchain 971,770). Rollback runs newest-first down to height 259, where theirs stops too (known upstream consensus behaviour reproduced; details held for coordinated disclosure), then reconnects to an identical state. |
| Their own code as oracle | done | `tools/oracle` links their `plain_bitassets` crate and generates random cases for `FilledTransaction::filled_outputs`, `bitcoin_fee`, `Body::compute_merkle_root`, AMM `mint/burn/swap*` and `DutchAuctionState::bid`. Results are compared byte for byte, error text for error text and panic for panic: 240,000 cases (80k tx, 80k AMM, 80k bids, 17,924 panics in theirs) give 0 differences. 4,500 cases are checked in (`tests/data/oracle`). |
| Live sync | done | From an empty datadir through the public enforcer (`https://enforcer.beta.ecash.ninja`) and the public seed peer to tip 1079 `195ef084…`, with wealth 133,074,500 and 27 UTXOs, the same as the replay. |
| Regtest pair with their binary | done | `tools/diffba.py` (doc/differential-tests.md), last run (2026-10-22, after the collect fix): 5,523 RPC comparisons over 54 blocks. Blocks made by each node are accepted by the other: deposits, transfers, transfer_many, reservations, registrations with data, BitAsset transfers, a pool-creating AMM mint (hand-built, signed by their wallet), an AMM mint, a Dutch auction create, bid and collect (our collect mined by their node), withdrawals (Succeeded and Failed bundles), an upstream mempool behaviour check and a mainchain reorg. The 61 remaining differences are of the kinds that also appear in the theirs-vs-theirs control run (their wallet balance lag; the state after their net task dies on `invalidate_block`), except one mempool difference caused by a kept fix. |

## The "halt" at ~1079

Nothing in their code halts the chain. BMM stopped at mainchain 970,889: the last commitment,
`7aadc874…`, is for a block 1080 whose body never reached any peer. A later deposit (seq 6 at
973,892) is waiting. The upgrade path is simply that anyone BMMs a new block on top of
`195ef084…`. That block connects the pending bundle `0fe864bd…` as Known (the enforcer reports
it Submitted) and the deposit. Our node does exactly what theirs does here
(`betanet_replay_all_checks`).

## Consensus behaviour reproduced (not fixed: fixing any of it would split from their nodes)

Our consensus code follows theirs exactly, including several upstream consensus behaviours that we
consider bugs. They are reproduced on purpose and checked against their code (rustc, the oracle and
the regtest pair). Known upstream consensus behaviour reproduced; details held for coordinated
disclosure with LayerTwo Labs.

* There is no 1000-transaction merkle switch (BitNames has one; BitAssets does not).
* Side tips: a mainchain reorg does not disconnect sidechain blocks whose BMM block left the
  chain (`REPRODUCE_SIDE_TIPS_HALT=false` keeps their behaviour; see open questions).

## Kept fixes (node, wallet and RPC only; consensus-neutral)

* Mempool, relay and templates apply extra policy checks so that they only carry transactions
  that can be connected. Among them, a transaction that spends one outpoint twice is refused; a
  block that carries one is invalid by consensus for both clients
  (`block_with_a_tx_spending_one_outpoint_twice_is_invalid`). Further details held for
  coordinated disclosure.
* `template_check` runs the connect-only checks (output filling, AMM, auctions, BitAssets) on
  template transactions, and the mempool drops a transaction once a template finds that it can
  never connect.
* Wallet: a Dutch-auction bid puts the quote change BEFORE the base output, the order in which
  consensus fills the outputs, so a bid with change is valid (test `bid_output_order`). Our bid
  with change was mined and accepted by their node.
* Wallet: a Dutch-auction collect also spends one of the wallet's BitAsset coins and pays it back
  in full, which keeps the collect valid under the consensus rules as reproduced (test
  `wallet_dutch_auction_collect_is_valid`; on the regtest pair our collect is mined by their
  node). The wallet prefers a coin of another asset, then one of the quote asset, then one of the
  base asset, checks that the outputs fill and pay out exactly the coin and the auction's
  amounts, and refuses to send otherwise.
* Wallet: an AMM swap that receives a BitAsset also spends one of the wallet's coins of it and pays it out with the
  amount received (after the spent asset's change), and taking liquidity out spends one coin of each BitAsset of the
  pool with the LP tokens and pays each back with its amount; under the consensus rules as reproduced, their builders'
  swaps and burns cannot be filled or are refused (tests `wallet_amm_swap_with_a_coin_of_the_received_asset_is_valid`,
  `wallet_amm_burn_with_a_coin_of_each_asset_is_valid`, and `appui_assets_pools_auctions_on_a_chain`, which connects
  them with every check). Both builders check that the outputs fill and pay out exactly the coins plus the amounts.
* Wallet: transactions the wallet builds are checked for fillability before they are sent
  (`check_fillable`).
* Wallet: a reservation spends one coin, the correct control coin is selected, `amm_mint` sorts
  assets and amounts, and `amm_swap`'s receive amount is computed as old − new.
* Our wallet is a view of the node's UTXO set. Theirs is a separate database that keeps coins a
  reorg removed and lags the tip. Both leave out coins spent by a mempool transaction.
* P2P hardening (core `node.h`): bans are enforced on inbound connections and on every request.
  Unknown connections are closed. GetHeaders is bounded by height(end)+1 and rate-limited per IP
  (IPv6 per /64, IPv4-mapped addresses per IPv4), and the limit survives reconnects. Body
  fetching is bounded, one chain per peer, and gives up on a round that makes no progress. An
  unchanged peer tip is retried every 5 s, because the first attempt can come before our
  mainchain sync has reached it. The node stays alive after a failed reorg.

## Our own bug found by the oracle (fixed)

`isqrt_u128` looped forever near 2^128, because `(r+1)^2` overflowed. That happened for
example on an AMM mint into an empty pool with reserves near u64::MAX. A block carrying such a
mint would have hung our node, while theirs (num::integer::sqrt) connects it. It is fixed, with
`isqrt_u128_extremes` and the oracle cases as tests.

## Their own behaviours seen on regtest (reproduced theirs-against-theirs; not consensus)

* After `invalidate_block` of the tip, their net task dies ("archive error: no height info for
  block hash …"): an in-flight request refers to the deleted block. The process keeps running
  with no P2P until it is restarted. It happens with two of their nodes as well.
* Their wallet cannot register its own reservation (`could not find bitasset reservation for
  …`), on both nodes of a theirs-vs-theirs pair. Ours registers it.
* Their wallet balance lags the tip and keeps coins a reorg removed.

## Consensus-changing proposals (documented, NOT coded)

Proposals for consensus changes are held, with the details above, for coordinated disclosure.
Each one needs a flag day agreed with LayerTwo Labs and the other node operators.

## Drop-in surface

* **CLI**: their flags: `--add-peer`, `-d/--datadir`, `--file-log-level`, `--headless`,
  `--log-dir`, `--log-level`, `--mainchain-grpc-host/--mainchain-grpc-port`,
  `--mnemonic-seed-phrase-path`, `-n/--net-addr` (0.0.0.0:4004), `--network`,
  `--network-magic`, `--rpc-host/--rpc-port` (127.0.0.1:6004),
  `--private-rpc-host/--private-rpc-port`, `--server-name`, `--wallet-dir`, `-z/--zmq-addr`
  (127.0.0.1:28004). Ours start with `--bt-`: `--bt-mainchain-grpc-url`, `--bt-no-seeds`,
  `--bt-no-zmq`, `--bt-rebuild-side-tips`, `--bt-rpc-allow-external`,
  `--bt-rpc-allowed-host/-origin`.
* **JSON-RPC**: 66 methods with serde shapes and error texts, including const-hex and borsh
  parse errors. They are openapi_schema, bitasset_data, bitassets, connect_block,
  dutch_auctions, get_amm_pool_state, get_amm_price, get_block, get_block_hash,
  get_block_index, get_bmm_inclusions, get_best_mainchain_block_hash,
  get_best_sidechain_block_hash, get_stxos, get_transaction, get_transaction_info, get_utxos,
  getblockcount, latest_failed_withdrawal_bundle_height, list_mempool, list_peers, list_utxos,
  mainchain_sync_progress, pending_withdrawal_bundle, sidechain_wealth,
  get_authorized_transaction (mempool only, as theirs), broadcast_transaction,
  rebroadcast_transaction, submit_transaction, connect_peer, forget_peer, invalidate_block,
  remove_from_mempool, stop, amm_burn, amm_mint, amm_swap, bitcoin_balance, create_deposit,
  decrypt_msg, dutch_auction_bid/create/collect, encrypt_msg, format_deposit_address,
  generate_mnemonic, get_block_template, get_new_address, get_new_encryption_key,
  get_new_verifying_key, get_wallet_addresses, get_wallet_utxos, my_utxos,
  my_unconfirmed_utxos, mine, register_bitasset, reserve_bitasset, set_seed_from_mnemonic,
  sign_arbitrary_msg, sign_arbitrary_msg_as_addr, sign_transaction, transfer, transfer_many,
  transfer_bitasset, verify_signature, withdraw. `bitassets-cli` mirrors their CLI.
* **P2P**: magic `cc 06 58 | net` (betanet 04, regtest 00), port 4004, protocol version 0.17.1.
* **Wallet**: rust-bitcoin BIP32 secp256k1 master from the BIP-39 seed. Transaction keys are
  m/0'/i, x25519 keys m/1'/i, message keys m/2'/i. The reservation nonce is
  keyed_hash(scalar, name_hash). Our keys and addresses are identical to theirs for the same
  mnemonic (checked on regtest).

## Open questions

* Side tips halt switch: should a mainchain reorg that drops a BMM block disconnect the
  sidechain block? Theirs does not. The switch stays off, as in thunder and bitnames.
* BitNames should move onto the core `Node<C>`, `PegState` and `SideTips` templates (it still
  has its own copy with the pre-review P2P patterns). So should Thunder.
* What happens to block 1080 (`7aadc874…`) if its body ever surfaces: it is valid by BMM, so it
  would connect.
