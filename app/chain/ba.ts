// BitAssets' own host methods (bitassets.*) and what the pages compute from them. Amounts of an asset are whole numbers
// (BitAssets have no decimals) the node writes as exact decimal text; the pages keep them as bigint and send them as
// JSON numbers (at most 2^53 − 1 from a page). Asset names and records are text anyone chose: shown as data.
import { request } from '../lib/bridge';
import { displayTextProblem } from './dropin/text';

/** An asset as the node names it: its LayerTwo AssetId (hex of its borsh bytes: 00 ECX, 01<id> a BitAsset, 02<id> its
 *  control coin), its 64-hex id, and its name in words when the wallet knows it (a reader of the wallet only). */
export interface AssetRef {
  assetId: string;
  kind: 'ecx' | 'asset' | 'control';
  id?: string;
  name?: string;
}

/** A BitAsset's records as the node says them (bitasset_data). */
export interface AssetData {
  commitment: number[] | null;
  socket_addr_v4: string | null;
  socket_addr_v6: string | null;
  encryption_pubkey: string | null;
  signing_pubkey: string | null;
}

export interface AssetInfo {
  id: string;
  assetId: string;
  seq: number;
  supply?: string;
  data: AssetData;
  name?: string;
  held?: string;
  control?: boolean;
}

export interface PoolView {
  asset0: AssetRef;
  asset1: AssetRef;
  reserve0: string;
  reserve1: string;
  lpTokens: string;
  creationTxid: string;
  /** Both sides BitAssets: what the app trades (an ECX side is shown, not traded through it). */
  tradable: boolean;
}

export interface AuctionView {
  id: string;
  base: AssetRef;
  quote: AssetRef;
  startBlock: number;
  duration: number;
  endBlock: number;
  initialBase: string;
  baseRemaining: string;
  quoteRaised: string;
  initialPrice: string;
  finalPrice: string;
  bids: number;
  /** The price of all that is left, in the next block (null: no bid can be made then). */
  priceNext: string | null;
  /** closing: no more bids, collected from the block after its last one. */
  state: 'upcoming' | 'open' | 'closing' | 'ended' | 'collected';
  nextHeight: number;
  tradable: boolean;
  /** This wallet holds its receipt (it runs it): only to a reader of the wallet. */
  mine?: boolean;
}

export interface AssetDetail extends AssetInfo {
  registeredAt: number;
  registrationTxid: string;
  pools: PoolView[];
  auctions: AuctionView[];
}

export interface HeldAsset extends AssetRef {
  amount: string;
  coins: number;
  control: boolean;
  supply?: string;
}

export interface WalletAssets {
  ecx: number;
  assets: HeldAsset[];
  lp: { asset0: AssetRef; asset1: AssetRef; lpTokens: string; share0?: string; share1?: string; pool?: PoolView }[];
  auctions: AuctionView[];
  reservations: { txid: string; commitment: string; name?: string }[];
  /** Reserved by this wallet (in a block), not registered yet: the next step. */
  pending: { name: string; id: string }[];
  /** Their registration sent, waiting for its block. */
  registering: { name: string; id: string }[];
}

export interface Resolved {
  name: string;
  id: string;
  assetId: string;
  registered: boolean;
  supply?: string;
  data?: AssetData;
}

export interface SwapQuote {
  pool: boolean;
  receive?: string;
  reserveSpend?: string;
  reserveReceive?: string;
  error?: string;
}

const host = <T>(method: string, params: Record<string, unknown> = {}) => request<T>('host', method, params);

export const ba = {
  assets: (limit = 1000) => host<AssetInfo[]>('bitassets.assets', { limit }),
  asset: (id: string) => host<AssetDetail>('bitassets.asset', { id }),
  resolve: (name: string) => host<Resolved>('bitassets.resolve', { name }),
  wallet: () => host<WalletAssets>('bitassets.wallet'),
  pools: () => host<PoolView[]>('bitassets.pools'),
  quoteSwap: (spend: string, receive: string, amount: bigint) => host<SwapQuote>('bitassets.quote', { kind: 'swap', spend, receive, amount: unitsNumber(amount) }),
  quoteMint: (asset0: string, asset1: string, amount0: bigint, amount1: bigint) =>
    host<{ pool: boolean; lpTokens?: string; reserve0?: string; reserve1?: string; error?: string }>('bitassets.quote', {
      kind: 'mint',
      asset0,
      asset1,
      amount0: unitsNumber(amount0),
      amount1: unitsNumber(amount1),
    }),
  quoteBurn: (asset0: string, asset1: string, lpTokens: bigint) =>
    host<{ pool: boolean; out0?: string; out1?: string; error?: string }>('bitassets.quote', { kind: 'burn', asset0, asset1, lpTokens: unitsNumber(lpTokens) }),
  auctions: () => host<AuctionView[]>('bitassets.auctions'),
  bidQuote: (id: string, bid: bigint) => host<{ height: number; price: string | null; quantity?: string; error?: string }>('bitassets.bidQuote', { id, bid: unitsNumber(bid) }),
};

/** The largest whole amount a page sends (a JSON number stays exact up to it). */
export const MAX_PAGE_UNITS = 9_007_199_254_740_991n;

/** A whole amount the user typed ("1,000,000", "1 000"), or null when it is not one. */
export function parseUnits(text: string): bigint | null {
  const s = text.replace(/[\s,_  ]/g, '');
  if (!/^\d{1,20}$/.test(s)) return null;
  const v = BigInt(s);
  return v <= 18_446_744_073_709_551_615n ? v : null;
}

/** An amount the node wrote (exact text) or a bigint, in groups of three digits. */
export function formatUnits(v: string | bigint | null | undefined): string {
  if (v === null || v === undefined) return '–';
  const s = typeof v === 'bigint' ? v.toString() : v;
  if (!/^\d+$/.test(s)) return s;
  return s.replace(/\B(?=(\d{3})+(?!\d))/g, ',');
}

export function units(v: string | null | undefined): bigint {
  return v && /^\d+$/.test(v) ? BigInt(v) : 0n;
}

/** A bigint amount as the JSON number a host method takes. */
export function unitsNumber(v: bigint): number {
  if (v < 0n || v > MAX_PAGE_UNITS) throw new Error('This amount is too large for this page');
  return Number(v);
}

/** The AssetId of a BitAsset of a 64-hex id. */
export function assetIdOf(id: string): string {
  return '01' + id;
}

/** An asset in words: ECX, its name when the wallet knows it, else the start of its id. */
export function assetLabel(a: Pick<AssetRef, 'kind' | 'id' | 'name'> | null | undefined): string {
  if (!a) return '–';
  if (a.kind === 'ecx') return 'ECX';
  const base = a.name ?? (a.id ? a.id.slice(0, 8) : '?');
  return a.kind === 'control' ? `control of ${base}` : base;
}

/** The hue of an asset's mark (the same everywhere, from its id: two assets of alike names told apart at a glance). */
export function assetHue(id: string | undefined): number {
  if (!id) return 160;
  return parseInt(id.slice(0, 6), 16) % 360;
}

/** Up to two letters of an asset's label for its mark (a name's first letters, an id's first two hex digits). */
export function initials(label: string): string {
  const words = label.trim().split(/[\s._-]+/).filter(Boolean);
  if (words.length === 0) return '?';
  const first = [...words[0]];
  if (words.length > 1) return (first[0] + [...words[1]][0]).toUpperCase();
  return first.slice(0, 2).join('').toUpperCase();
}

/** The price of 1 of `base` in `quote` units a pool gives now (reserves), as text with up to 6 significant digits. */
export function poolPrice(reserveBase: string, reserveQuote: string): string {
  const b = units(reserveBase);
  const q = units(reserveQuote);
  if (b === 0n || q === 0n) return '–';
  return ratioText(q, b);
}

/** a / b as a short decimal (6 significant digits), exact arithmetic. */
export function ratioText(a: bigint, b: bigint): string {
  if (b === 0n) return '–';
  const scale = 10n ** 12n;
  const v = (a * scale) / b;
  const int = v / scale;
  const frac = (v % scale).toString().padStart(12, '0');
  if (int >= 1000n) return formatUnits(int);
  const digits = int === 0n ? 6 + frac.search(/[1-9]/) : Math.max(0, 6 - int.toString().length);
  const f = frac.slice(0, Math.max(0, Math.min(12, digits))).replace(/0+$/, '');
  return f ? `${formatUnits(int)}.${f}` : formatUnits(int);
}

/** How much a swap moves the pool's price against the trader, in percent (from the reserves and the amounts). */
export function priceImpact(reserveSpend: string, reserveReceive: string, spend: bigint, receive: bigint): number {
  const rs = units(reserveSpend);
  const rr = units(reserveReceive);
  if (rs === 0n || rr === 0n || spend === 0n) return 0;
  // spot: rr/rs per unit; got: receive/spend. impact = 1 - got/spot.
  const got = Number((receive * 1_000_000n * rs) / (spend * rr));
  return Math.max(0, 100 - got / 10_000);
}

/** The price an auction asks for all that is left at each block from now to its end, linearly falling as the chain
 *  prices it (for a picture: the chain's own price is the node's, priceNext). */
export function auctionCurve(a: Pick<AuctionView, 'startBlock' | 'duration' | 'initialPrice' | 'finalPrice'>): { block: number; price: number }[] {
  const out: { block: number; price: number }[] = [];
  const p0 = Number(units(a.initialPrice));
  const p1 = Number(units(a.finalPrice));
  const n = Math.max(1, a.duration);
  for (let i = 0; i < n; i++) out.push({ block: a.startBlock + i, price: n === 1 ? p0 : p0 - ((p0 - p1) * i) / (n - 1) });
  return out;
}

/** What a name may be (their rules take any UTF-8 text; ours keep it one line that reads as it is, at most 255 bytes: as
 *  the node's describers check, core's display_text_problem; dropin/text.ts is its twin here). */
export function nameProblem(name: string): string {
  if (!name) return 'Type a name';
  if (new TextEncoder().encode(name).length > 255) return 'At most 255 bytes';
  const why = displayTextProblem(name);
  if (why) return why;
  if (name !== name.trim()) return 'No spaces around it';
  return '';
}

/** The records of an asset to show: label, value. */
export function records(d: AssetData | undefined | null): [string, string][] {
  if (!d) return [];
  const out: [string, string][] = [];
  if (d.socket_addr_v4) out.push(['IPv4', d.socket_addr_v4]);
  if (d.socket_addr_v6) out.push(['IPv6', d.socket_addr_v6]);
  if (d.encryption_pubkey) out.push(['Encryption key', d.encryption_pubkey]);
  if (d.signing_pubkey) out.push(['Signing key', d.signing_pubkey]);
  if (d.commitment) out.push(['Commitment', d.commitment.map((b) => b.toString(16).padStart(2, '0')).join('')]);
  return out;
}
