#!/usr/bin/env python3
"""Local differential test, BitAssets (eCash slot 4): their node (octobocto plain-bitassets v0.17.1,
plain_bitassets_app) and ours (backbone-bitassets `bitassets`) on one LOCAL regtest mainchain (ecash-com
bitcoind betanet branch + the real bip300301_enforcer 17eb2ce), P2P-peered only with each other.

Blocks are made by both nodes in turn (`mine`: BMM through the enforcer's WalletService, the mainchain block
through MiningService.GenerateToAddress); after every block both nodes' state is compared: raw JSON-RPC reply bytes
of the read methods (sets where theirs iterates a HashMap), plus the wallet methods (both wallets share a mnemonic).

    diffba.py [--keep]          results in $RUN/result.json, log on stdout
Ports PORT_BASE+0..27 only (default 49020-49047). Every process started here is stopped by exact PID at the end.
"""
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import threading
import time
import urllib.request

sys.path.insert(0, os.path.expanduser("~/skydoge/tools/linked-regtest"))
import connect as C  # noqa: E402  (Connect-JSON client, standard library only)

SKY = os.path.expanduser("~/skydoge")
SCR = os.environ.get("SCR", "/tmp/diffba")
RUN = os.environ.get("RUN", f"{SCR}/run-ba")
ECASHD = f"{SKY}/ext/ecash-bitcoin/build/bin/bitcoind"
ENFORCER = f"{SKY}/ext/enforcer/target/release/bip300301_enforcer"
THEIRS = f"{SKY}/ext/plain-bitassets-octobocto/target/release/plain_bitassets_app"
OURS = os.environ.get("OURS", f"{SKY}/backbone-bitassets/build/chains/bitassets/bitassets")
SLOT = 4
USER, PASS = "diff", "diff"
PB = int(os.environ.get("PORT_BASE", "49020"))  # every port is PB + 0..27
A_RPC, A_P2P, A_ZMQ = PB, PB + 1, PB + 3
E_GRPC, E_RPC = PB + 10, PB + 11
PORTS = {"theirs": (PB + 20, PB + 21, PB + 22), "ours": (PB + 25, PB + 26, PB + 27)}
MNEMONIC = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
ENF = f"127.0.0.1:{E_GRPC}"

procs = {}
mismatches, notes = [], []
checks = 0
T0 = time.time()


def log(*a):
    print(f"[{time.time() - T0:7.1f}]", *a, flush=True)


def start(name, argv):
    out = open(f"{RUN}/{name}.out", "ab")
    p = subprocess.Popen(argv, stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
    procs[name] = p
    log("started", name, "pid", p.pid)
    return p


def stop_all():
    for name, p in list(procs.items()):
        if p.poll() is None:
            try:
                os.kill(p.pid, signal.SIGTERM)  # exact PID we started
            except ProcessLookupError:
                pass
    deadline = time.time() + 30
    for name, p in procs.items():
        try:
            p.wait(max(1, deadline - time.time()))
        except subprocess.TimeoutExpired:
            os.kill(p.pid, signal.SIGKILL)
            p.wait()
    log("stopped", ", ".join(f"{n}:{p.pid}" for n, p in procs.items()))


def wait_for(cond, timeout, what, step=0.5):
    t = time.time()
    last = None
    while time.time() - t < timeout:
        try:
            v = cond()
            if v:
                return v
        except Exception as e:  # noqa: BLE001
            last = e
        time.sleep(step)
    raise RuntimeError(f"timeout: {what} ({last})")


def rpc(port, method, *params):
    body = json.dumps({"jsonrpc": "1.0", "id": 1, "method": method, "params": list(params)}).encode()
    import base64
    req = urllib.request.Request(f"http://127.0.0.1:{port}/", body, {"Authorization": "Basic " + base64.b64encode(f"{USER}:{PASS}".encode()).decode()})
    try:
        with urllib.request.urlopen(req, timeout=120) as r:
            j = json.loads(r.read())
    except urllib.error.HTTPError as e:
        j = json.loads(e.read())
    if j.get("error"):
        raise RuntimeError(f"{method}: {j['error']}")
    return j["result"]


def grpc(method, req=None, timeout=120):
    return C.call(ENF, method, req or {}, timeout=timeout)


class Node:
    def __init__(self, name):
        self.name = name
        self.p2p, self.rpc_port, self.zmq = PORTS[name]

    def raw(self, method, params=None):
        body = {"jsonrpc": "2.0", "id": 1, "method": method}
        if params is not None:
            body["params"] = params
        req = urllib.request.Request(f"http://127.0.0.1:{self.rpc_port}/", json.dumps(body).encode(), {"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=180) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()
        except Exception as e:  # noqa: BLE001  (their handler panicked, refused, ...)
            return 0, f"no reply: {type(e).__name__}: {e}".encode()

    def call(self, method, params=None):
        st, b = self.raw(method, params)
        try:
            j = json.loads(b)
        except ValueError:
            raise RuntimeError(f"{self.name} {method}: {b[:300]!r}")
        if "error" in j:
            raise RuntimeError(f"{self.name} {method}: {j['error']}")
        return j["result"]


A, B = Node("theirs"), Node("ours")


def compare(label, method, params=None, unordered=False, key=None):
    global checks
    checks += 1
    a, b = A.raw(method, params), B.raw(method, params)
    if a == b:
        return True
    try:
        ja, jb = json.loads(a[1]), json.loads(b[1])
    except Exception:  # noqa: BLE001
        ja, jb = None, None
    kind = "bytes"
    if ja is not None and ja == jb:
        kind = "json-equal, bytes differ (key order / formatting)"
    elif unordered and ja is not None and "result" in ja and "result" in jb and isinstance(ja["result"], list) and \
            sorted(json.dumps(x, sort_keys=True) for x in ja["result"]) == sorted(json.dumps(x, sort_keys=True) for x in jb["result"]):
        return True  # a set: theirs iterates a HashMap
    elif ja is not None and "error" in ja and "error" in jb and ja["error"].get("code") == jb["error"].get("code"):
        kind = "same error code, message differs"
    mismatches.append({"label": label, "method": method, "params": params, "kind": kind,
                       "theirs": [a[0], a[1][:2000].decode(errors="replace")], "ours": [b[0], b[1][:2000].decode(errors="replace")]})
    log("MISMATCH", label, method, json.dumps(params)[:200], kind, "\n   theirs:", a[1][:700], "\n   ours:  ", b[1][:700])
    return False


MINER_ADDR = None


def main_mine(n=1):
    r = grpc("MiningService/GenerateToAddress", {"blocks": n, "address": MINER_ADDR}, timeout=300)
    time.sleep(2)
    return r


def side_mine(node, fee=1000, expect_new=True):
    """`mine` on node: its BMM request, a mainchain block, its sidechain block; then both nodes at the same tip."""
    before = node.call("getblockcount")
    res = {}

    def run():
        try:
            res["r"] = node.call("mine", [fee])
        except Exception as e:  # noqa: BLE001
            res["e"] = e
    mp0 = len(rpc(A_RPC, "getrawmempool"))
    t = threading.Thread(target=run, daemon=True)
    t.start()
    try:
        wait_for(lambda: len(rpc(A_RPC, "getrawmempool")) > mp0, 60, f"{node.name}'s BMM request")
    except RuntimeError as e:
        notes.append(f"{node.name}: no BMM request seen ({e})")
    main_mine()
    t.join(120)
    if "e" in res:
        notes.append(f"{node.name} mine: {res['e']}")
        log("mine error", node.name, res["e"])
    if expect_new:
        wait_for(lambda: node.call("getblockcount") > before, 60, f"{node.name} mined a block")
    tip = node.call("get_best_sidechain_block_hash")
    other = B if node is A else A
    try:
        wait_for(lambda: other.call("get_best_sidechain_block_hash") == tip, 120, f"{other.name} follows {node.name}'s block")
    except RuntimeError as e:
        mismatches.append({"label": "sync", "method": "get_best_sidechain_block_hash", "kind": "divergence",
                           "theirs": A.call("get_best_sidechain_block_hash"), "ours": B.call("get_best_sidechain_block_hash"), "detail": str(e)})
        log("DIVERGENCE", e)
    compare_state(f"after {node.name}'s block {before + 1}")
    return tip


known_txids, known_addrs, known_assets, known_pairs, known_auctions = set(), set(), set(), set(), set()


def asset_hex(kind, h):
    return {"bitcoin": "00", "bitasset": "01" + h, "control": "02" + h}[kind]


def compare_state(tag):
    n = A.call("getblockcount")
    for m in ("getblockcount", "get_best_sidechain_block_hash", "get_best_mainchain_block_hash", "sidechain_wealth",
              "pending_withdrawal_bundle", "latest_failed_withdrawal_bundle_height", "bitassets", "dutch_auctions"):
        compare(tag, m)
    compare(tag, "list_utxos", unordered=True)
    for h in range(0, n + 2):
        compare(tag, "get_block_hash", [h])
    for h in range(max(0, n - 2), n + 1):
        bh = A.call("get_block_hash", [h])
        if bh is None:
            continue
        compare(tag, "get_block", [bh])
        compare(tag, "get_block_index", [bh])
        compare(tag, "get_bmm_inclusions", [bh])
        blk = A.call("get_block", [bh])
        for tx in blk["body"]["transactions"]:
            pass
        for t in A.call("get_block_index", [bh])["txs"]:
            known_txids.add(t["txid"])
    for txid in sorted(known_txids):
        compare(tag, "get_transaction", [txid])
        compare(tag, "get_authorized_transaction", [txid])
        compare(tag, "get_transaction_info", [txid])
    for seq, bid, _ in A.call("bitassets"):
        known_assets.add(bid)
        compare(tag, "bitasset_data", [bid])
    for a0, a1 in sorted(known_pairs):
        compare(tag, "get_amm_pool_state", [a0, a1])
        compare(tag, "get_amm_price", [a0, a1])
        compare(tag, "get_amm_price", [a1, a0])
    addrs = sorted(set(A.call("get_wallet_addresses")) | set(B.call("get_wallet_addresses")) | known_addrs)
    compare(tag, "get_utxos", [addrs], unordered=True)
    compare(tag, "get_stxos", [addrs], unordered=True)
    compare(tag, "list_mempool", unordered=True)
    compare(tag, "bitcoin_balance")
    compare(tag, "mainchain_sync_progress")


def errors():
    tag = "errors"
    unknown = "11" * 32
    cases = [("get_block_hash", [999999]), ("get_bmm_inclusions", [unknown]), ("get_transaction", [unknown]),
             ("get_block", ["zz"]), ("get_block", []), ("get_block_hash", ["x"]), ("getblockcount", [1]), ("no_such_method", None),
             ("get_utxos", [["notanaddress"]]), ("withdraw", ["bc1qnotvalid", 1000, 10, 10]), ("transfer", ["x", 1, 1]),
             ("create_deposit", ["x", 1000, 100]), ("format_deposit_address", ["x"]), ("submit_transaction", [{}]),
             ("sign_transaction", [{}]), ("connect_block", [{}, unknown]), ("set_seed_from_mnemonic", ["not a mnemonic"]),
             ("get_block_index", [unknown]), ("bitasset_data", [unknown]), ("get_authorized_transaction", [unknown]),
             ("get_transaction_info", [unknown]), ("rebroadcast_transaction", [unknown]), ("decrypt_msg", ["x", "y"]),
             ("get_amm_pool_state", ["00", "01" + unknown]), ("get_amm_price", ["00", "01" + unknown]),
             ("get_amm_pool_state", ["zz", "00"]), ("dutch_auction_bid", [unknown, 5]), ("register_bitasset", ["never-reserved", 5, None]),
             ("verify_signature", ["00", "x", "transaction", "m"])]
    for m, p in cases:
        compare(tag, m, p)
    compare(tag, "get_block_hash", {"height": 0})


def start_main():
    if os.path.exists(RUN):
        shutil.rmtree(RUN)
    for d in ("a", "e", "theirs", "ours"):
        os.makedirs(f"{RUN}/{d}")
    start("ecashd", [ECASHD, "-regtest", f"-datadir={RUN}/a", f"-rpcport={A_RPC}", f"-port={A_P2P}", "-listen=0",
                     f"-rpcuser={USER}", f"-rpcpassword={PASS}", "-rest", "-server", "-txindex", "-listenonion=0", "-dnsseed=0", "-fixedseeds=0",
                     f"-zmqpubsequence=tcp://127.0.0.1:{A_ZMQ}", "-fallbackfee=0.0002", "-maxtipage=2000000000", "-acceptnonstdtxn", "-disablewallet"])
    wait_for(lambda: rpc(A_RPC, "getblockcount") >= 0, 60, "ecashd")
    start("enforcer", [ENFORCER, f"--data-dir={RUN}/e", f"--node-rpc-addr=127.0.0.1:{A_RPC}", f"--node-rpc-user={USER}",
                       f"--node-rpc-pass={PASS}", f"--node-zmq-addr-sequence=tcp://127.0.0.1:{A_ZMQ}", "--network-magic=eca5b134",
                       f"--serve-grpc-addr=127.0.0.1:{E_GRPC}", f"--serve-rpc-addr=127.0.0.1:{E_RPC}",
                       "--op-drivechain=nop8", "--enable-wallet", "--wallet-auto-create", "--wallet-sync-source=disabled",
                       "--enable-mempool", "--enable-block-template-server", f"--log-directory={RUN}/e/logs", "--log-level=info"])
    wait_for(lambda: grpc("ValidatorService/GetChainInfo"), 300, "enforcer gRPC")
    global MINER_ADDR
    MINER_ADDR = wait_for(lambda: grpc("WalletService/CreateNewAddress")["address"], 300, "enforcer wallet")
    main_mine(101)
    grpc("BlockProducerService/SetAckAllProposals", {"policy": "ACK_ALL_PROPOSALS_POLICY_ALL"})
    grpc("BlockProducerService/SubmitSidechainProposal", {"sidechainId": SLOT, "declaration": {"v0": {
        "title": "BitAssets", "description": "BitAssets (local regtest)", "hashId1": {"hex": "00" * 32}, "hashId2": {"hex": "00" * 20}}}})
    main_mine(8)
    sc = grpc("ValidatorService/GetSidechains")
    slots = [s.get("sidechainNumber") for s in sc.get("sidechains", [])]
    log("active slots:", slots)
    assert SLOT in slots, slots


def start_side():
    for node, binary in ((A, THEIRS), (B, OURS)):
        other = B if node is A else A
        d = f"{RUN}/{node.name}"
        args = [binary, "--headless", "--network", "regtest", "--datadir", f"{d}/data", "--log-dir", f"{d}/logs", "--log-level", "info",
                "-n", f"127.0.0.1:{node.p2p}", "--rpc-host", "127.0.0.1", "--rpc-port", str(node.rpc_port),
                "--add-peer", f"127.0.0.1:{other.p2p}", "--mainchain-grpc-host", "127.0.0.1", "--mainchain-grpc-port", str(E_GRPC),
                "-z", f"127.0.0.1:{node.zmq}"]
        if node is B and binary != THEIRS:
            args += ["--bt-no-seeds"]
        node.args = args
        node.proc = start(f"side-{node.name}", args)
    for node in (A, B):
        wait_for(lambda: node.call("getblockcount") is not None, 120, f"{node.name} RPC")
        node.call("set_seed_from_mnemonic", [MNEMONIC])
    time.sleep(8)


def restart(node, why):
    notes.append(f"{node.name} restarted: {why}")
    log("RESTART", node.name, why)
    node.proc.terminate()
    try:
        node.proc.wait(30)
    except subprocess.TimeoutExpired:
        node.proc.kill()
        node.proc.wait()
    node.proc = start(f"side-{node.name}-restart", node.args)
    wait_for(lambda: node.call("getblockcount") is not None, 120, f"{node.name} restarted")
    time.sleep(10)


def check_alive(what):
    for node in (A, B):
        if node.proc.poll() is None and "Net task error" in open(f"{RUN}/side-{node.name}.out", errors="replace").read()[-200000:] \
                and not getattr(node, "net_dead_seen", False):
            node.net_dead_seen = True
            restart(node, f"net task died on {what}")
            continue
        if node.proc.poll() is not None:
            out = open(f"{RUN}/side-{node.name}.out", errors="replace").read().strip().splitlines()[-3:]
            notes.append(f"{node.name} EXITED (rc {node.proc.returncode}) on {what}: " + " | ".join(out))
            log("CRASH", node.name, what, out)
            node.proc = start(f"side-{node.name}-restart", node.args)
            wait_for(lambda: node.call("getblockcount") is not None, 120, f"{node.name} restarted")
            time.sleep(15)


def relayed(node, txid, what):
    other = B if node is A else A
    try:
        wait_for(lambda: txid in json.dumps(other.call("list_mempool")), 30, f"{what} {txid} relayed to {other.name}")
    except RuntimeError as e:
        notes.append(f"relay: {e}")
        mismatches.append({"label": "relay", "method": "PushTransaction", "kind": "not relayed", "from": node.name, "detail": str(e)})
        log("NOT RELAYED", e)


def retry(fn, what, tries=20):
    last = None
    for _ in range(tries):
        try:
            return fn()
        except RuntimeError as e:
            last = e
            time.sleep(1)
    notes.append(f"{what}: {last}")
    log("FAILED", what, last)
    return None

def create_pool_by_hand(node, id_a, id_b, amount_a, amount_b):
    """AmmMint into a fresh pool: one coin of each asset in, the two changes (spent-asset order),
    then the LP tokens = isqrt(amount0 * amount1)."""
    import math
    coins = {}
    for u in node.call("get_wallet_utxos"):
        c = u["output"]["content"]
        if isinstance(c, dict) and "BitAsset" in c:
            bid, v = c["BitAsset"]
            if bid in (id_a, id_b) and v > (amount_a if bid == id_a else amount_b) and bid not in coins:
                coins[bid] = (u["outpoint"], v)
    if len(coins) != 2:
        notes.append(f"pool by hand: coins missing {coins}")
        return None
    order = [id_a, id_b]  # inputs in this order: the spent-asset totals follow it
    amt = {id_a: amount_a, id_b: amount_b}
    lo, hi = sorted([id_a, id_b])
    lp = math.isqrt(amt[lo] * amt[hi])
    addr = node.call("get_new_address")
    tx = {"inputs": [coins[i][0] for i in order],
          "outputs": [{"address": addr, "content": {"BitAsset": coins[i][1] - amt[i]}, "memo": ""} for i in order]
          + [{"address": addr, "content": {"AmmLpToken": lp}, "memo": ""}],
          "memo": "", "data": {"AmmMint": {"amount0": amt[lo], "amount1": amt[hi], "lp_token_mint": lp}}}
    try:
        at = node.call("sign_transaction", [tx, True])
    except RuntimeError as e:
        notes.append(f"pool by hand: {e}")
        log("FAILED pool by hand", e)
        return None
    txid = None
    for t in node.call("list_mempool"):
        if t["tx"]["data"] and "AmmMint" in t["tx"]["data"]:
            txid = t.get("txid")
    return txid


def scenario():
    for _ in range(3):
        compare("wallet", "get_new_address")
    compare("wallet", "get_new_encryption_key")
    compare("wallet", "get_new_verifying_key")
    compare_state("start")
    errors()
    log("blocks by each in turn")
    for node in (A, B, A, B):
        side_mine(node)
    log("deposits from each node")
    for node in (A, B):
        addr = node.call("get_new_address")
        (B if node is A else A).call("get_new_address")
        known_addrs.add(addr)
        compare("deposit", "format_deposit_address", [addr])
        node.call("create_deposit", [addr, 100_000_000, 10_000])
        main_mine()
        side_mine(B if node is A else A)
    side_mine(A)
    log("transfers: made on one node, relayed, mined by the other")
    for node in (A, B):
        other = B if node is A else A
        dest = other.call("get_new_address"); node.call("get_new_address")
        known_addrs.add(dest)
        txid = node.call("transfer", [dest, 5_000_000, 1_000, "c0ffee"])
        known_txids.add(txid)
        relayed(node, txid, "transfer")
        side_mine(other)
    d1 = A.call("get_new_address"); B.call("get_new_address")
    d2 = A.call("get_new_address"); B.call("get_new_address")
    txid = B.call("transfer_many", [{d1: 1_000_000, d2: 2_000_000}, 1_000])
    relayed(B, txid, "transfer_many")
    side_mine(A)
    log("BitAssets: reserve, register (with data), transfer")
    ids = {}
    for node, name in ((A, "alpha"), (B, "beta")):
        other = B if node is A else A
        txid = node.call("reserve_bitasset", [name])
        relayed(node, txid, "reservation")
        side_mine(other)
        data = {"commitment": [0xaa] * 32, "socket_addr_v4": "1.2.3.4:80", "socket_addr_v6": "[::ffff:1.2.3.4]:443",
                "encryption_pubkey": None, "signing_pubkey": None}
        txid = retry(lambda: node.call("register_bitasset", [name, 1_000_000, data]), f"register_bitasset {name} on {node.name}")
        if txid:
            relayed(node, txid, "registration")
            side_mine(other)
        bid = __import__("hashlib").blake2b  # placeholder, replaced below
        ids[name] = [b for s, b, d in A.call("bitassets")][-1] if A.call("bitassets") else None
    if ids.get("alpha") is None:
        # their wallet could not register its own reservation (see the control run): register
        # "gamma" on ours and hand theirs part of the supply, so AMM and auctions still run
        log("alpha missing: registering gamma on ours and sending theirs 600000")
        txid = B.call("reserve_bitasset", ["gamma"])
        relayed(B, txid, "reservation gamma")
        side_mine(A)
        data = {"commitment": None, "socket_addr_v4": None, "socket_addr_v6": None, "encryption_pubkey": None, "signing_pubkey": None}
        txid = retry(lambda: B.call("register_bitasset", ["gamma", 1_000_000, data]), "register_bitasset gamma on ours")
        if txid:
            relayed(B, txid, "registration gamma")
            side_mine(A)
            gid = [b for s, b, d in A.call("bitassets")][-1]
            dest = A.call("get_new_address"); B.call("get_new_address")
            txid = retry(lambda: B.call("transfer_bitasset", [dest, gid, 600_000, 1000, None]), "transfer gamma to theirs")
            if txid:
                relayed(B, txid, "gamma transfer")
                side_mine(A)
                ids["alpha"] = gid
    log("ids", ids)
    alpha, beta = ids.get("alpha"), ids.get("beta")
    if alpha and beta:
        dest = B.call("get_new_address"); A.call("get_new_address")
        txid = retry(lambda: A.call("transfer_bitasset", [dest, alpha, 1000, 1000, None]), "transfer_bitasset")
        if txid:
            relayed(A, txid, "bitasset transfer")
            side_mine(B)
        log("AMM: mint on theirs, swap on ours, burn on theirs")
        a0, a1 = sorted([asset_hex("bitasset", alpha), asset_hex("bitasset", beta)])
        known_pairs.add((a0, a1))
        # theirs holds alpha; it needs beta too: ours sends some
        dest = A.call("get_new_address"); B.call("get_new_address")
        txid = retry(lambda: B.call("transfer_bitasset", [dest, beta, 200_000, 1000, None]), "transfer beta to theirs")
        if txid:
            relayed(B, txid, "beta transfer")
            side_mine(A)
        # Neither RPC can open a pool (amm_mint reads the pool state first: MissingPoolState), so
        # the first mint is built by hand and signed + broadcast by their wallet.
        txid = create_pool_by_hand(A, alpha, beta, 100_000, 100_000)
        if txid:
            relayed(A, txid, "pool-creating mint (hand-built, signed by theirs)")
            side_mine(B)
            log("pool", A.call("get_amm_pool_state", [a0, a1]))
        txid = retry(lambda: A.call("amm_mint", [a0, a1, 100_000, 100_000]), "amm_mint")
        if txid:
            relayed(A, txid, "amm_mint")
            side_mine(B)
        r = retry(lambda: B.call("amm_swap", [asset_hex("bitasset", beta), asset_hex("bitasset", alpha), 1000]), "amm_swap on ours")
        log("amm_swap ours ->", r)
        side_mine(A)
        txid = retry(lambda: A.call("amm_burn", [a0, a1, 1000]), "amm_burn")
        if txid:
            relayed(A, txid, "amm_burn")
            side_mine(B)
        log("Dutch auction: create on ours, bid on theirs, collect on ours")
        h = B.call("getblockcount")
        params = {"start_block": h + 2, "duration": 6, "base_asset": asset_hex("bitasset", beta), "base_amount": 10_000,
                  "quote_asset": asset_hex("bitasset", alpha), "initial_price": 5_000, "final_price": 1_000}
        txid = retry(lambda: B.call("dutch_auction_create", [params]), "dutch_auction_create")
        if txid:
            relayed(B, txid, "auction create")
            side_mine(A)
            side_mine(B)
            before = {t.get("txid") for t in A.call("list_mempool")}
            r = retry(lambda: A.call("dutch_auction_bid", [txid, 2_000]), "dutch_auction_bid on theirs")
            log("bid on theirs ->", r)
            side_mine(B)
            # their wallet's bid with change cannot be filled: drop it from both mempools
            for node in (A, B):
                for t in node.call("list_mempool"):
                    if t["tx"]["data"] and "DutchAuctionBid" in t["tx"]["data"]:
                        tid = t.get("txid")
                        log("their bid still in", node.name, "mempool:", tid)
                        try:
                            node.call("remove_from_mempool", [tid])
                        except RuntimeError as e:
                            notes.append(f"remove_from_mempool {node.name}: {e}")
            r = retry(lambda: B.call("dutch_auction_bid", [txid, 2_000]), "dutch_auction_bid on ours")
            log("bid on ours ->", r)
            side_mine(A)
            for i in range(6):
                side_mine(A if i % 2 else B)
            r = retry(lambda: B.call("dutch_auction_collect", [txid]), "dutch_auction_collect on ours")
            log("collect ->", r)
            ctx = None
            for t in B.call("list_mempool"):
                if t["tx"]["data"] and "DutchAuctionCollect" in t["tx"]["data"]:
                    ctx = t.get("txid")
            if ctx:
                relayed(B, ctx, "collect")
                side_mine(A)  # their node mines our collect
                for node in (A, B):
                    info = node.call("get_transaction_info", [ctx])
                    log("collect on", node.name, "->", "in a block" if info and info.get("txin") else "NOT in a block")
                    if not (info and info.get("txin")):
                        notes.append(f"collect {ctx} not in a block on {node.name}")
            else:
                notes.append("collect: no collect transaction in our mempool")
                side_mine(A)
    log("upstream mempool behaviour check (details held for coordinated disclosure)")
    zi = A.call("reserve_bitasset", ["gamma"])
    at = A.call("get_authorized_transaction", [zi])
    relayed(A, zi, "reservation")
    side_mine(B)
    try:
        A.call("submit_transaction", [at])
        log("resubmission accepted by theirs")
        side_mine(A)
    except RuntimeError as e:
        notes.append(f"resubmission on theirs: {e}")
    log("withdrawal: bundle, Submitted, voted, Succeeded")
    main_addr = grpc("WalletService/CreateNewAddress")["address"]
    txid = A.call("withdraw", [main_addr, 10_000_000, 5_000, 1_000])
    relayed(A, txid, "withdrawal")
    for i in range(14):
        side_mine(B if i % 2 == 0 else A)
        if A.call("pending_withdrawal_bundle") is None and i > 2:
            break
    log("a bundle nobody votes for: Failed")
    grpc("BlockProducerService/SetWithdrawalBundlePolicy", {"policy": "WITHDRAWAL_BUNDLE_POLICY_NONE"})
    txid = B.call("withdraw", [main_addr, 7_000_000, 5_000, 1_000])
    relayed(B, txid, "withdrawal")
    for i in range(16):
        side_mine(A if i % 2 == 0 else B)
        if A.call("latest_failed_withdrawal_bundle_height") is not None:
            break
    grpc("BlockProducerService/SetWithdrawalBundlePolicy", {"policy": "WITHDRAWAL_BUNDLE_POLICY_KNOWN"})
    for i in range(4):
        side_mine(B if i % 2 == 0 else A)
    log("sidechain reorg: invalidate_block of the tip on both, then blocks on the other branch")
    tip = A.call("get_best_sidechain_block_hash")
    for node in (A, B):
        try:
            node.call("invalidate_block", [tip])
        except RuntimeError as e:
            notes.append(f"invalidate_block on {node.name}: {e}")
    time.sleep(5)
    check_alive("invalidate_block")
    compare_state("after invalidate_block")
    try:
        side_mine(B)
        side_mine(A)
    except Exception as e:  # noqa: BLE001
        notes.append(f"mining after invalidate_block failed: {str(e)[:400]}")
        log("MINING AFTER INVALIDATE FAILED", str(e)[:400])
        check_alive("mining after invalidate_block")
    log("mainchain reorg over a sidechain tip (with a deposit in the dropped blocks)")
    addr = A.call("get_new_address"); B.call("get_new_address")
    A.call("create_deposit", [addr, 3_000_000, 10_000])
    side_mine(A)
    side_mine(B)
    h = rpc(A_RPC, "getblockcount")
    drop = rpc(A_RPC, "getblockhash", h - 1)
    rpc(A_RPC, "invalidateblock", drop)
    hashes = [rpc(A_RPC, "generateblock", MINER_ADDR, [])["hash"] for _ in range(3)]
    wait_for(lambda: grpc("ValidatorService/GetChainTip")["blockHeaderInfo"]["blockHash"]["hex"] == hashes[-1], 120, "enforcer on the new branch")
    for wait in (15, 45):
        time.sleep(wait)
        check_alive("the mainchain reorg")
        compare_state(f"after mainchain reorg (+{wait}s)")
    log("after the reorg: more blocks")
    try:
        side_mine(A)
        side_mine(B)
    except Exception as e:  # noqa: BLE001
        notes.append(f"mining after the mainchain reorg failed: {str(e)[:600]}")
        log("MINING AFTER REORG FAILED", str(e)[:600])
    errors()
    open(f"{RUN}/openapi-theirs.json", "wb").write(A.raw("openapi_schema")[1])


def main():
    try:
        start_main()
        start_side()
        scenario()
    except BaseException:  # noqa: BLE001  (also Ctrl-C / SIGTERM: the finally stops every process)
        import traceback
        traceback.print_exc(file=sys.stdout)
        notes.append("aborted: " + traceback.format_exc()[-800:])
    finally:
        try:
            for node in (A, B):
                log(node.name, "height", node.call("getblockcount"))
        except Exception:  # noqa: BLE001
            pass
        if "--keep" not in sys.argv:
            stop_all()
        json.dump({"checks": checks, "mismatches": mismatches, "notes": notes}, open(f"{RUN}/result.json", "w"), indent=1)
        log(f"DONE checks={checks} mismatches={len(mismatches)} notes={len(notes)}")


if __name__ == "__main__":
    signal.signal(signal.SIGTERM, lambda *a: (_ for _ in ()).throw(KeyboardInterrupt()))
    main()
