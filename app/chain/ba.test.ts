// BitAssets' own pages over a pretend node: every spend only through rpc.confirmed with plain-bitassets' names and
// parameters in their order (reserve_bitasset [name], register_bitasset [name, supply, data?], transfer_bitasset [dest,
// asset id, amount, fee], amm_swap [asset_spend, asset_receive, amount], dutch_auction_create [params]), and no request
// outside the manifest; the pages' arithmetic of amounts.
import { fireEvent, render, waitFor } from '@testing-library/svelte';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { afterEach, beforeEach, describe, expect, it } from 'vitest';
import { setTransport, type BridgeRequest } from '../lib/bridge';
import { auctionCurve, formatUnits, nameProblem, parseUnits, priceImpact, ratioText, unitsNumber } from './ba';

const manifest = JSON.parse(readFileSync(join(__dirname, 'manifest.json'), 'utf8')) as { reads: string[]; confirmed: string[]; host: string[] };
const APP_METHODS = new Set(['app.hello', 'app.info', 'app.locale', 'app.languages', 'prefs.get', 'prefs.set', 'node.state', 'rpc.confirmed', 'rpc.confirmedMethods', 'window.state', 'clipboard.write', 'app.notify']);
const GOLD = 'a1'.repeat(32);
const SILVER = 'b2'.repeat(32);
const ADDR = '4XrMw8bmnhchFspSyP4XRUuB5vaR';

let calls: BridgeRequest[] = [];
let answers: Record<string, (r: BridgeRequest) => unknown> = {};
const ref = (id: string, name: string) => ({ assetId: '01' + id, kind: 'asset', id, name });
const pool = { asset0: ref(GOLD, 'gold'), asset1: ref(SILVER, 'silver'), reserve0: '100000', reserve1: '400000', lpTokens: '200000', creationTxid: 'cd'.repeat(32), tradable: true };
const wallet = {
  ecx: 250_000_000,
  assets: [
    { ...ref(GOLD, 'gold'), amount: '900000', coins: 2, control: true, supply: '1000000' },
    { ...ref(SILVER, 'silver'), amount: '600000', coins: 1, control: true, supply: '1000000' },
  ],
  lp: [{ asset0: ref(GOLD, 'gold'), asset1: ref(SILVER, 'silver'), lpTokens: '200000', share0: '100000', share1: '400000', pool }],
  auctions: [],
  reservations: [],
  pending: [] as { name: string; id: string }[],
  registering: [],
};

beforeEach(() => {
  calls = [];
  answers = {
    'dropin.overview': () => ({ wallet: { hasSeed: true, available: 250_000_000, withdrawing: 0, pendingIn: 0, pendingOut: 0 }, chain: { height: 40, peers: 0, slot: 4, network: 'regtest' }, mainchain: { slot: 4, connected: true, state: 'ok' } }),
    'dropin.seed.status': () => ({ hasSeed: true, phraseKept: true, from: 'app', fingerprint: '00', derivation: 'plain_bitassets' }),
    'dropin.checkAddress': (r) => ({ valid: String((r.params as { address: string }).address).startsWith('4X'), mine: false }),
    'bitassets.wallet': () => wallet,
    'bitassets.assets': () => [
      { id: GOLD, assetId: '01' + GOLD, seq: 0, supply: '1000000', data: {}, name: 'gold', held: '900000', control: true },
      { id: SILVER, assetId: '01' + SILVER, seq: 1, supply: '1000000', data: {}, name: 'silver', held: '600000', control: true },
    ],
    'bitassets.pools': () => [pool],
    'bitassets.auctions': () => [],
    'bitassets.quote': (r) => {
      const p = r.params as { kind: string };
      if (p.kind === 'swap') return { pool: true, receive: '3560', reserveSpend: '100000', reserveReceive: '400000' };
      if (p.kind === 'mint') return { pool: true, lpTokens: '1000' };
      return { pool: true, out0: '50000', out1: '200000' };
    },
    'bitassets.resolve': (r) => ({ name: (r.params as { name: string }).name, id: GOLD, assetId: '01' + GOLD, registered: false }),
    'bitassets.bidQuote': () => ({ height: 41, price: '5000', quantity: '2000' }),
    'rpc.confirmed': () => 'cd'.repeat(32),
  };
  setTransport(async (req) => {
    calls.push(req);
    const answer = answers[req.kind === 'host' ? (req.method ?? '') : `rpc:${req.method}`];
    return answer ? answer(req) : null;
  });
});
afterEach(() => setTransport(null));

const confirmedCalls = () => calls.filter((c) => c.kind === 'host' && c.method === 'rpc.confirmed').map((c) => c.params as { method: string; params: unknown[] });
const button = (root: HTMLElement, words: string) => [...root.querySelectorAll('button')].find((b) => b.textContent?.includes(words)) as HTMLButtonElement;

describe('BitAssets pages', () => {
  it('issue: a free name reserved with reserve_bitasset [name], then registered with register_bitasset [name, supply] once in a block', async () => {
    const { refresh, drop } = await import('./dropin/store.svelte');
    await refresh();
    const Issue = (await import('./pages/Issue.svelte')).default;
    const { container, getByLabelText } = render(Issue);
    await fireEvent.input(getByLabelText('Its name'), { target: { value: 'gold' } });
    await waitFor(() => expect(container.querySelector('.verdict .good')).toBeTruthy(), { timeout: 3000 });
    await fireEvent.click(button(container, 'Reserve'));
    await waitFor(() => expect(confirmedCalls()).toEqual([{ method: 'reserve_bitasset', params: ['gold'] }]));
    // The reservation in a block: the node says the name is pending; the page offers to register it.
    wallet.pending = [{ name: 'gold', id: GOLD }];
    drop.version++;
    await waitFor(() => expect(button(container, 'Register').disabled).toBe(false), { timeout: 3000 });
    await fireEvent.input(container.querySelector('#ba-supply') as HTMLInputElement, { target: { value: '21,000,000' } });
    await fireEvent.click(button(container, 'Register'));
    await waitFor(() => expect(confirmedCalls().length).toBe(2));
    expect(confirmedCalls()[1]).toEqual({ method: 'register_bitasset', params: ['gold', 21_000_000] });
    wallet.pending = [];
  });

  it('swap: amm_swap [asset_spend, asset_receive, amount_spend] with the AssetIds of the pool, after the node’s quote', async () => {
    const { nav } = await import('../lib/nav.svelte');
    nav.tab = 'swap';
    const Trade = (await import('./pages/Trade.svelte')).default;
    const { container } = render(Trade);
    await waitFor(() => expect(container.querySelector('#sw-pay')).toBeTruthy(), { timeout: 3000 });
    await fireEvent.input(container.querySelector('#sw-pay') as HTMLInputElement, { target: { value: '1000' } });
    await waitFor(() => expect(container.querySelector('#sw-get-amount')?.textContent).toBe('3,560'), { timeout: 3000 });
    const submit = button(container, 'Review and swap');
    await waitFor(() => expect(submit.disabled).toBe(false), { timeout: 3000 });
    await fireEvent.click(submit);
    await waitFor(() => expect(confirmedCalls().length).toBe(1));
    expect(confirmedCalls()[0]).toEqual({ method: 'amm_swap', params: ['01' + GOLD, '01' + SILVER, 1000] });
  });

  it('send an asset: transfer_bitasset [dest, asset id, amount, fee]; more than held offers nothing', async () => {
    const { nav } = await import('../lib/nav.svelte');
    nav.tab = 'send';
    const MyAssets = (await import('./pages/MyAssets.svelte')).default;
    const { container, getByLabelText } = render(MyAssets);
    await waitFor(() => expect(container.querySelector('#as-amount')).toBeTruthy(), { timeout: 3000 });
    await fireEvent.input(getByLabelText('Send to'), { target: { value: ADDR } });
    await fireEvent.input(container.querySelector('#as-amount') as HTMLInputElement, { target: { value: '1000000' } });
    const submit = button(container, 'Review and send');
    await new Promise((r) => setTimeout(r, 400));
    expect(submit.disabled).toBe(true);
    await fireEvent.input(container.querySelector('#as-amount') as HTMLInputElement, { target: { value: '2500' } });
    await waitFor(() => expect(submit.disabled).toBe(false), { timeout: 3000 });
    await fireEvent.click(submit);
    await waitFor(() => expect(confirmedCalls().length).toBe(1));
    expect(confirmedCalls()[0]).toEqual({ method: 'transfer_bitasset', params: [ADDR, GOLD, 2500, 1000] });
  });

  it('sell by auction: dutch_auction_create [{start_block, duration, base_asset, base_amount, quote_asset, initial_price, final_price}]', async () => {
    const { refresh } = await import('./dropin/store.svelte');
    await refresh();
    const { nav } = await import('../lib/nav.svelte');
    nav.tab = 'sell';
    const Auctions = (await import('./pages/Auctions.svelte')).default;
    const { container } = render(Auctions);
    await waitFor(() => expect(container.querySelector('#au-amount')).toBeTruthy(), { timeout: 3000 });
    await fireEvent.input(container.querySelector('#au-amount') as HTMLInputElement, { target: { value: '10000' } });
    await fireEvent.input(container.querySelector('#au-start') as HTMLInputElement, { target: { value: '5000' } });
    await fireEvent.input(container.querySelector('#au-final') as HTMLInputElement, { target: { value: '1000' } });
    const submit = button(container, 'Review and start');
    await waitFor(() => expect(submit.disabled).toBe(false), { timeout: 3000 });
    await fireEvent.click(submit);
    await waitFor(() => expect(confirmedCalls().length).toBe(1));
    expect(confirmedCalls()[0]).toEqual({
      method: 'dutch_auction_create',
      params: [{ start_block: 43, duration: 20, base_asset: '01' + GOLD, base_amount: 10000, quote_asset: '01' + SILVER, initial_price: 5000, final_price: 1000 }],
    });
  });

  it('call nothing of the node outside the manifest; every spend one of its confirmed', async () => {
    for (const page of ['Issue', 'MyAssets', 'Assets', 'Trade', 'Auctions']) render((await import(`./pages/${page}.svelte`)).default);
    render((await import('./LocalCards.svelte')).default);
    await new Promise((r) => setTimeout(r, 500));
    const allowed = new Set([...manifest.host, ...APP_METHODS]);
    const bad = calls.filter((c) => (c.kind === 'host' ? !allowed.has(c.method ?? '') : !manifest.reads.includes(c.method ?? '')));
    expect(bad.map((c) => `${c.kind}:${c.method}`)).toEqual([]);
    for (const c of confirmedCalls()) expect(manifest.confirmed).toContain(c.method);
  });

  it('amounts, prices and names in words', () => {
    expect(parseUnits('1,000 000')).toBe(1_000_000n);
    expect(parseUnits('1.5')).toBe(null);
    expect(parseUnits('-1')).toBe(null);
    expect(parseUnits('18446744073709551616')).toBe(null);
    expect(formatUnits('21000000')).toBe('21,000,000');
    expect(() => unitsNumber(9_007_199_254_740_992n)).toThrow();
    expect(ratioText(400000n, 100000n)).toBe('4');
    expect(ratioText(1n, 3n)).toBe('0.333333');
    expect(priceImpact('100000', '400000', 1000n, 3560n)).toBeCloseTo(11, 0);
    expect(auctionCurve({ startBlock: 10, duration: 5, initialPrice: '5000', finalPrice: '1000' }).map((c) => c.price)).toEqual([5000, 4000, 3000, 2000, 1000]);
    expect(nameProblem('gold')).toBe('');
    expect(nameProblem(' gold')).toBe('No spaces around it');
    expect(nameProblem('go‮ld')).toContain('U+202E');
    expect(nameProblem('go​ld')).toContain('U+200B');
    expect(nameProblem('go ld')).toContain('U+2028');
  });
});
