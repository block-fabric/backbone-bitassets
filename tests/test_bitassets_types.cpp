#include "test.h"

#include <bitassets/state.h>
#include <bitassets/types.h>

#include <bt/bip32.h>

using namespace bitassets;

TEST(bitassets_empty_block_merkle_root)
{
    // every empty betanet BitAssets block has merkle root 812c9708...bdbb (study §BitAssets 1)
    auto r = compute_merkle_root(Coinbase{}, {});
    REQUIRE(r.has_value());
    CHECK_EQ(bt::hex_encode(*r), std::string("812c97082cde521a6128a777b45d9221259e8e3e33d6f84b7b496ef43ca6bdbb"));
}

TEST(bitassets_asset_id_order_and_forms)
{
    Hash32 a{}, b{};
    a[0] = 1;
    b[0] = 2;
    CHECK(AssetId::bitcoin() < AssetId::bitasset(b));
    CHECK(AssetId::bitasset(b) < AssetId::control(a));
    CHECK(AssetId::bitasset(a) < AssetId::bitasset(b));
    CHECK_EQ(AssetId::bitcoin().to_hex(), std::string("00"));
    CHECK_EQ(AssetId::bitasset(a).borsh_bytes().size(), size_t(33));
    CHECK(AssetId::from_borsh(AssetId::control(a).borsh_bytes()) == std::optional<AssetId>(AssetId::control(a)));
    CHECK(!AssetId::from_borsh(bt::Bytes{0, 0})); // trailing byte
    CHECK(!AssetId::from_borsh(bt::Bytes{3}));
}

TEST(bip32_secp256k1_vector1)
{
    // BIP-32 test vector 1: m/0'/1 (chain m/0H/1)
    auto seed = *bt::hex_decode("000102030405060708090a0b0c0d0e0f");
    auto m = bt::bip32::master(seed);
    REQUIRE(m.has_value());
    CHECK_EQ(bt::hex_encode(m->secret), std::string("e8f32e723decf4051aefac8e2c93c9c5b214313817cdb01a1494b917c8436b35"));
    auto c = bt::bip32::derive_path(*m, {0 | bt::bip32::HARDENED, 1});
    REQUIRE(c.has_value());
    CHECK_EQ(bt::hex_encode(c->secret), std::string("3c6cb8d0f6a264c91ea8b5030fadaa8e538b020f0a387421a12de9319dc93368"));
    CHECK_EQ(bt::hex_encode(c->chaincode), std::string("2a7857631386ba23dacac34180dd1983734e444fdbf774041578e9b6adb37c19"));
}

TEST(isqrt_u128_extremes)
{
    // num::integer::sqrt for u128 at the edges (a previous version looped forever near 2^128)
    const u128 m = ~u128(0);
    CHECK(uint64_t(isqrt_u128(m)) == UINT64_MAX);
    CHECK(uint64_t(isqrt_u128(u128(UINT64_MAX) * u128(UINT64_MAX - 1))) == UINT64_MAX - 1);
    CHECK(uint64_t(isqrt_u128(u128(UINT64_MAX) * u128(UINT64_MAX))) == UINT64_MAX);
    CHECK(uint64_t(isqrt_u128(u128(1) << 64)) == (uint64_t(1) << 32));
    for (uint64_t v : {0ull, 1ull, 2ull, 3ull, 4ull, 15ull, 16ull, 17ull, 1000000ull}) {
        uint64_t r = uint64_t(isqrt_u128(v));
        CHECK(u128(r) * r <= v && u128(r + 1) * (r + 1) > v);
    }
}

TEST(bid_output_order)
{
    // The regtest bid of their wallet: spends 1000 + 399000 of the quote asset (039b…), bids 2000,
    // receives 4000 of the base asset (c607…). Their order (base first) cannot be filled; quote
    // change first (our wallet) can.
    Hash32 quote = *bt::hex_to_array<32>("039b3fa6c7a5987c410ffe6d58ab194dfc98840263841bc7c949bdd4497fd576");
    Hash32 base = *bt::hex_to_array<32>("c607f0e66519ff41d34c1c8e2e312228c3cc358c0a5b75cef4b22cf8ed3875db");
    auto spent = [&](uint64_t v) {
        FilledOutput o;
        o.content.kind = FilledContent::BitAsset;
        o.content.id = quote;
        o.content.value = v;
        return o;
    };
    auto out = [&](uint64_t v) {
        Output o;
        o.content.kind = Content::BitAsset;
        o.content.value = v;
        return o;
    };
    Transaction tx;
    tx.inputs.resize(2);
    tx.inputs[0].kind = OutPoint::Regular;
    tx.inputs[1].kind = OutPoint::Regular;
    tx.inputs[1].vout = 1;
    TxData d;
    d.kind = TxData::DutchAuctionBid;
    d.auction_id = base;
    d.receive_asset = AssetId::bitasset(base);
    d.quantity = 4000;
    d.bid_size = 2000;
    tx.data = d;
    tx.outputs = {out(4000), out(398000)}; // theirs
    FilledTransaction theirs{&tx, {spent(1000), spent(399000)}};
    CHECK(!theirs.filled_outputs().has_value());
    Transaction tx2 = tx;
    tx2.outputs = {out(398000), out(4000)}; // ours
    FilledTransaction ours{&tx2, {spent(1000), spent(399000)}};
    auto f = ours.filled_outputs();
    REQUIRE(f.has_value());
    CHECK(f->at(0).content.id == quote && f->at(0).content.value == 398000);
    CHECK(f->at(1).content.id == base && f->at(1).content.value == 4000);
}

TEST(bitasset_swap_cannot_deliver)
{
    // Their consensus, reproduced: a BitAsset-for-BitAsset swap with a receive output cannot be
    // filled (regtest: rejected by their node and ours alike).
    Hash32 beta = *bt::hex_to_array<32>("c607f0e66519ff41d34c1c8e2e312228c3cc358c0a5b75cef4b22cf8ed3875db");
    Hash32 gamma = *bt::hex_to_array<32>("039b3fa6c7a5987c410ffe6d58ab194dfc98840263841bc7c949bdd4497fd576");
    FilledOutput in;
    in.content.kind = FilledContent::BitAsset;
    in.content.id = beta;
    in.content.value = 700000;
    auto out = [](uint64_t v) {
        Output o;
        o.content.kind = Content::BitAsset;
        o.content.value = v;
        return o;
    };
    Transaction tx;
    tx.inputs.resize(1);
    TxData d;
    d.kind = TxData::AmmSwap;
    d.amount_spent = 1000;
    d.amount_receive = 989;
    d.pair_asset = AssetId::bitasset(gamma);
    tx.data = d;
    tx.outputs = {out(699000), out(989)};
    CHECK(!(FilledTransaction{&tx, {in}}.filled_outputs().has_value()));
    tx.outputs = {out(989), out(699000)};
    CHECK(!(FilledTransaction{&tx, {in}}.filled_outputs().has_value()));
    tx.outputs = {out(699000)}; // without the receive output it fills (and the 989 are lost)
    CHECK((FilledTransaction{&tx, {in}}.filled_outputs().has_value()));
}
