//! Differential oracle for backbone-bitassets: random BitAssets transactions through the fork's own
//! FilledTransaction::{filled_outputs, bitcoin_fee}, Body::compute_merkle_root, AMM PoolState and Dutch
//! auction bid arithmetic. Prints one JSON line per case; the C++ side recomputes and compares.
use plain_bitassets::{
    state::{AmmPoolState, DutchAuctionState},
    types::{
        Address, AssetId, BitAssetId, Body, Coinbase, DutchAuctionParams, FilledOutput, FilledOutputContent,
        FilledTransaction, OutPoint, Output, OutputContent, Transaction, TxData, Txid, WithdrawalOutputContent,
        BitcoinOutputContent,
    },
};
use std::panic::{catch_unwind, AssertUnwindSafe};

struct Rng(u64, Vec<u64>);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
    fn below(&mut self, n: u64) -> u64 { self.next() % n }
    fn amount(&mut self) -> u64 {
        if !self.1.is_empty() && self.below(10) < 6 {
            let i = self.below(self.1.len() as u64) as usize;
            let v = self.1[i];
            return match self.below(6) { 0 => v.wrapping_add(1), 1 => v.wrapping_sub(1), 2 => v / 2, _ => v };
        }
        let v = self.fresh();
        self.1.push(v);
        v
    }
    fn fresh(&mut self) -> u64 {
        match self.below(8) {
            0 => 0,
            1 => self.below(4),
            2 => u64::MAX - self.below(4),
            3 => 1 << self.below(64),
            _ => self.below(2_000),
        }
    }
}

fn bitasset(r: &mut Rng) -> BitAssetId { BitAssetId([1 + r.below(3) as u8; 32]) }
fn asset(r: &mut Rng) -> AssetId {
    match r.below(4) {
        0 => AssetId::Bitcoin,
        1 | 2 => AssetId::BitAsset(bitasset(r)),
        _ => AssetId::BitAssetControl(bitasset(r)),
    }
}
fn addr(r: &mut Rng) -> Address { Address([r.below(3) as u8; 20]) }

fn main_address() -> bitcoin::Address<bitcoin::address::NetworkUnchecked> {
    "bc1qw508d6qejxtdg4y5r3zarvary0c5xw7kv8f3t4".parse().unwrap()
}

fn filled(r: &mut Rng) -> FilledOutput {
    let content = match r.below(9) {
        0 | 1 => FilledOutputContent::Bitcoin(BitcoinOutputContent(bitcoin::Amount::from_sat(r.amount()))),
        2 | 3 => FilledOutputContent::BitAsset(bitasset(r), r.amount()),
        4 => FilledOutputContent::BitAssetControl(bitasset(r)),
        5 => FilledOutputContent::AmmLpToken { asset0: asset(r), asset1: asset(r), amount: r.amount() },
        6 => FilledOutputContent::BitAssetReservation(Txid([r.below(3) as u8; 32]), [r.below(3) as u8; 32]),
        7 => FilledOutputContent::DutchAuctionReceipt(plain_bitassets::types::DutchAuctionId(Txid([r.below(3) as u8; 32]))),
        _ => FilledOutputContent::BitcoinWithdrawal(WithdrawalOutputContent {
            value: bitcoin::Amount::from_sat(r.amount()), main_fee: bitcoin::Amount::from_sat(r.amount()), main_address: main_address() }),
    };
    Output { address: addr(r), content, memo: if r.below(4) == 0 { vec![r.below(256) as u8; r.below(4) as usize] } else { vec![] } }
}

fn output(r: &mut Rng) -> Output {
    let content = match r.below(9) {
        0 | 1 => OutputContent::Bitcoin(BitcoinOutputContent(bitcoin::Amount::from_sat(r.amount()))),
        2 | 3 => OutputContent::BitAsset(r.amount()),
        4 => OutputContent::BitAssetControl,
        5 => OutputContent::AmmLpToken(r.amount()),
        6 => OutputContent::BitAssetReservation,
        7 => OutputContent::DutchAuctionReceipt,
        _ => OutputContent::Withdrawal(WithdrawalOutputContent {
            value: bitcoin::Amount::from_sat(r.amount()), main_fee: bitcoin::Amount::from_sat(r.amount()), main_address: main_address() }),
    };
    Output { address: addr(r), content, memo: vec![] }
}

fn txdata(r: &mut Rng) -> Option<TxData> {
    Some(match r.below(12) {
        0 => TxData::AmmBurn { amount0: r.amount(), amount1: r.amount(), lp_token_burn: r.amount() },
        1 => TxData::AmmMint { amount0: r.amount(), amount1: r.amount(), lp_token_mint: r.amount() },
        2 => TxData::AmmSwap { amount_spent: r.amount(), amount_receive: r.amount(), pair_asset: asset(r) },
        3 => TxData::BitAssetReservation { commitment: [r.below(3) as u8; 32] },
        4 => TxData::BitAssetRegistration {
            name_hash: [1 + r.below(3) as u8; 32], revealed_nonce: [r.below(3) as u8; 32],
            bitasset_data: Box::default(), initial_supply: r.amount() },
        5 => TxData::BitAssetMint(r.amount()),
        6 => TxData::DutchAuctionCreate(DutchAuctionParams {
            start_block: r.below(10) as u32, duration: r.below(5) as u32, base_asset: asset(r), base_amount: r.amount(),
            quote_asset: asset(r), initial_price: r.amount(), final_price: r.amount() }),
        7 => TxData::DutchAuctionBid { auction_id: plain_bitassets::types::DutchAuctionId(Txid([r.below(3) as u8; 32])),
            receive_asset: asset(r), quantity: r.amount(), bid_size: r.amount() },
        8 => TxData::DutchAuctionCollect { asset_offered: asset(r), asset_receive: asset(r),
            amount_offered_remaining: r.amount(), amount_received: r.amount() },
        _ => return None,
    })
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    let seed: u64 = args.get(1).map(|s| s.parse().unwrap()).unwrap_or(1);
    let n: usize = args.get(2).map(|s| s.parse().unwrap()).unwrap_or(1000);
    let mut r = Rng(seed.wrapping_mul(0x9E3779B97F4A7C15) | 1, Vec::new());
    std::panic::set_hook(Box::new(|_| {}));
    for _ in 0..n {
        r.1.clear();
        let n_in = r.below(5) as usize;
        let n_out = r.below(6) as usize;
        let spent: Vec<FilledOutput> = (0..n_in).map(|_| filled(&mut r)).collect();
        let inputs: Vec<OutPoint> = (0..n_in).map(|i| OutPoint::Regular { txid: Txid([i as u8; 32]), vout: i as u32 }).collect();
        let outputs: Vec<Output> = (0..n_out).map(|_| output(&mut r)).collect();
        let data = txdata(&mut r);
        let outputs: Vec<Output> = outputs.into_iter().map(|o| { let _ = &o; output(&mut r) }).collect();
        let tx = Transaction { inputs: inputs.into(), outputs: outputs.into(), memo: vec![], data };
        let ft = FilledTransaction { transaction: tx, spent_utxos: spent };
        let case = const_hex::encode(bincode::serialize(&ft).unwrap());
        let filled = catch_unwind(AssertUnwindSafe(|| ft.filled_outputs()));
        let filled_j = match filled {
            Ok(Some(f)) => serde_json::Value::String(const_hex::encode(bincode::serialize(&f).unwrap())),
            Ok(None) => serde_json::Value::Null,
            Err(_) => serde_json::Value::String("panic".into()),
        };
        let fee = ft.bitcoin_fee().ok().map(|a| a.to_sat());
        let root = catch_unwind(AssertUnwindSafe(|| Body::compute_merkle_root(&Coinbase::default(), std::slice::from_ref(&ft))));
        let root_j = match root {
            Ok(Ok(m)) => serde_json::Value::String(m.to_string()),
            Ok(Err(_)) => serde_json::Value::Null,
            Err(_) => serde_json::Value::String("panic".into()),
        };
        println!("{}", serde_json::json!({"kind": "tx", "case": case, "filled": filled_j, "fee": fee, "root": root_j}));
    }
    // AMM pool arithmetic
    for _ in 0..n {
        let pool = serde_json::json!({"reserve0": r.amount(), "reserve1": r.amount(), "outstanding_lp_tokens": r.amount(),
            "creation_txid": "00".repeat(32)});
        let ps: AmmPoolState = serde_json::from_value(pool.clone()).unwrap();
        let (a, b) = (r.amount(), r.amount());
        let run = |f: &dyn Fn() -> Result<AmmPoolState, plain_bitassets::state::error::Amm>| -> serde_json::Value {
            match catch_unwind(AssertUnwindSafe(f)) {
                Ok(Ok(p)) => serde_json::to_value(p).unwrap(),
                Ok(Err(e)) => serde_json::Value::String(format!("err: {e}")),
                Err(_) => serde_json::Value::String("panic".into()),
            }
        };
        let mint = run(&|| ps.mint(a, b));
        let burn = run(&|| ps.burn(a));
        let s01 = run(&|| ps.swap_asset0_for_asset1(a));
        let s10 = run(&|| ps.swap_asset1_for_asset0(a));
        println!("{}", serde_json::json!({"kind": "amm", "pool": pool, "a": a, "b": b, "mint": mint, "burn": burn, "s01": s01, "s10": s10}));
    }
    // Dutch auction bids
    for _ in 0..n {
        let start = r.below(20) as u32;
        let duration = 1 + r.below(10) as u32;
        let last = start + r.below(duration as u64 + 2) as u32;
        let rem = r.amount();
        let p = r.amount();
        let e = if r.below(4) == 0 { r.amount() } else { p / (1 + r.below(5)) };
        let st = serde_json::json!({"start_block": start,
            "most_recent_bid_block": [{"data": last, "txid": "00".repeat(32), "height": 0}],
            "duration": duration, "base_asset": "00", "initial_base_amount": rem,
            "base_amount_remaining": [{"data": rem, "txid": "00".repeat(32), "height": 0}],
            "quote_asset": "00", "quote_amount": [{"data": r.amount(), "txid": "00".repeat(32), "height": 0}],
            "initial_price": p, "price_after_most_recent_bid": [{"data": p, "txid": "00".repeat(32), "height": 0}],
            "initial_end_price": e, "end_price_after_most_recent_bid": [{"data": e, "txid": "00".repeat(32), "height": 0}]});
        let das: DutchAuctionState = serde_json::from_value(st.clone()).unwrap();
        let bid = r.amount();
        let height = start.saturating_sub(1) + r.below(duration as u64 + 4) as u32;
        let res = match catch_unwind(AssertUnwindSafe(|| das.bid(Txid([7; 32]), bid, height))) {
            Ok(Ok(s)) => serde_json::to_value(s).unwrap(),
            Ok(Err(e)) => serde_json::Value::String(format!("err: {e}")),
            Err(_) => serde_json::Value::String("panic".into()),
        };
        println!("{}", serde_json::json!({"kind": "bid", "state": st, "bid": bid, "height": height, "result": res}));
    }
}
