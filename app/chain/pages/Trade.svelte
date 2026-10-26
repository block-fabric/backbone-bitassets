<script lang="ts">
  // Trade: swap one BitAsset for another in their pool (amm_swap [asset_spend, asset_receive, amount_spend]), the pools of
  // the chain, and liquidity (amm_mint [asset0, asset1, amount0, amount1], amm_burn [asset0, asset1, lp_token_amount]).
  // What a pool gives is the node's own arithmetic (bitassets.quote), and every trade is asked in the app's trusted
  // dialog. The app trades pools of two BitAssets; a pool with ECX is shown, not traded.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { formatCount } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import SetupWallet from '../dropin/SetupWallet.svelte';
  import { confirmed } from '../dropin/api';
  import { drop, follow, refresh } from '../dropin/store.svelte';
  import {
    assetIdOf,
    assetLabel,
    ba,
    formatUnits,
    parseUnits,
    poolPrice,
    priceImpact,
    units,
    unitsNumber,
    type AssetInfo,
    type PoolView,
    type SwapQuote,
    type WalletAssets,
  } from '../ba';
  import AssetMark from '../ba/AssetMark.svelte';

  $effect(() => follow());

  const TABS = [
    { id: 'swap', label: 'Swap' },
    { id: 'pools', label: 'Pools' },
    { id: 'liquidity', label: 'Liquidity' },
  ];
  const tab = $derived(TABS.some((x) => x.id === nav.tab) ? nav.tab : 'swap');

  let pools = $state<PoolView[]>([]);
  let assets = $state<AssetInfo[]>([]);
  let wallet = $state<WalletAssets | null>(null);
  let error = $state('');
  $effect(() => {
    void drop.version;
    Promise.all([ba.pools(), ba.assets(), drop.seed?.hasSeed ? ba.wallet() : Promise.resolve(null)])
      .then(([p, a, w]) => {
        pools = p;
        assets = a;
        wallet = w;
        error = '';
      })
      .catch((e) => (error = errorText(e)));
  });

  const ref = (id: string) => {
    const a = assets.find((x) => x.id === id);
    return { kind: 'asset' as const, id, name: a?.name };
  };
  const heldOf = (id: string | undefined) => units(wallet?.assets.find((a) => a.id === id)?.amount);
  const tradable = $derived(pools.filter((p) => p.tradable));
  /** The assets a pool pairs with `id`. */
  const partners = (id: string) => tradable.flatMap((p) => (p.asset0.id === id ? [p.asset1.id ?? ''] : p.asset1.id === id ? [p.asset0.id ?? ''] : []));
  const poolOf = (a: string, b: string) => tradable.find((p) => (p.asset0.id === a && p.asset1.id === b) || (p.asset0.id === b && p.asset1.id === a));

  // ---- swap
  const payable = $derived((wallet?.assets ?? []).filter((a) => units(a.amount) > 0n && partners(a.id ?? '').length > 0));
  let payId = $state('');
  let getId = $state('');
  let payText = $state('');
  let quote = $state<SwapQuote | null>(null);
  let busy = $state(false);
  let swapError = $state('');
  $effect(() => {
    if (!payId && payable.length) payId = payable[0].id ?? '';
  });
  $effect(() => {
    const options = partners(payId);
    if (!options.includes(getId)) getId = options[0] ?? '';
  });
  const pay = $derived(parseUnits(payText));
  const payTooMuch = $derived(pay !== null && pay > heldOf(payId));
  $effect(() => {
    const p = pay;
    const a = payId;
    const b = getId;
    void drop.version;
    quote = null;
    if (!a || !b || p === null || p === 0n) return;
    const timer = setTimeout(() => {
      ba.quoteSwap(assetIdOf(a), assetIdOf(b), p)
        .then((q) => {
          if (a === payId && b === getId && p === pay) quote = q;
        })
        .catch((e) => (swapError = errorText(e)));
    }, 200);
    return () => clearTimeout(timer);
  });
  const receive = $derived(quote?.receive ? units(quote.receive) : null);
  const impact = $derived(quote && receive !== null && pay ? priceImpact(quote.reserveSpend ?? '0', quote.reserveReceive ?? '0', pay, receive) : 0);
  // The chain pays a BitAsset out of a pool together with a coin of it the wallet spends (our wallet's swap does so).
  const needsCoin = $derived(!!getId && heldOf(getId) === 0n);
  const swapReady = $derived(!!payId && !!getId && pay !== null && pay > 0n && !payTooMuch && receive !== null && receive > 0n && !needsCoin && !busy);

  async function swap() {
    if (!swapReady || pay === null) return;
    busy = true;
    swapError = '';
    try {
      const got = await confirmed<number>('amm_swap', [assetIdOf(payId), assetIdOf(getId), unitsNumber(pay)]);
      toast('success', t('Swapped'), t('%1 %2 for %3 %4: in the next block of BitAssets.', formatUnits(pay), assetLabel(ref(payId)), formatUnits(String(got)), assetLabel(ref(getId))));
      payText = '';
      await refresh();
    } catch (e) {
      if (!declined(e)) swapError = errorText(e);
    } finally {
      busy = false;
    }
  }

  function flip() {
    const a = payId;
    payId = getId;
    getId = a;
    payText = '';
  }

  // ---- liquidity: add
  const heldAssets = $derived((wallet?.assets ?? []).filter((a) => units(a.amount) > 0n));
  let addA = $state('');
  let addB = $state('');
  let addAText = $state('');
  let addBText = $state('');
  let addQuote = $state<{ pool: boolean; lpTokens?: string; error?: string } | null>(null);
  $effect(() => {
    if (!addA && heldAssets.length) addA = heldAssets[0].id ?? '';
    if ((!addB || addB === addA) && heldAssets.length > 1) addB = heldAssets.find((a) => a.id !== addA)?.id ?? '';
  });
  const addX = $derived(parseUnits(addAText));
  const addY = $derived(parseUnits(addBText));
  const addPool = $derived(addA && addB ? poolOf(addA, addB) : undefined);
  $effect(() => {
    const [a, b, x, y] = [addA, addB, addX, addY];
    addQuote = null;
    if (!a || !b || a === b || x === null || y === null || x === 0n || y === 0n) return;
    const timer = setTimeout(() => {
      ba.quoteMint(assetIdOf(a), assetIdOf(b), x, y)
        .then((q) => (addQuote = q))
        .catch(() => undefined);
    }, 200);
    return () => clearTimeout(timer);
  });
  /** In an existing pool, the second amount at the pool's ratio. */
  function matchB() {
    if (!addPool || addX === null) return;
    const first = addPool.asset0.id === addA;
    const ra = units(first ? addPool.reserve0 : addPool.reserve1);
    const rb = units(first ? addPool.reserve1 : addPool.reserve0);
    if (ra > 0n) addBText = ((addX * rb) / ra).toString();
  }
  const addReady = $derived(
    !!addA && !!addB && addA !== addB && addX !== null && addY !== null && addX > 0n && addY > 0n && addX <= heldOf(addA) && addY <= heldOf(addB) && !!addQuote?.lpTokens && !busy,
  );
  async function add() {
    if (!addReady || addX === null || addY === null) return;
    busy = true;
    swapError = '';
    try {
      await confirmed<string>('amm_mint', [assetIdOf(addA), assetIdOf(addB), unitsNumber(addX), unitsNumber(addY)]);
      toast('success', t('Liquidity added'), t('Your LP tokens come with the next block of BitAssets.'));
      addAText = addBText = '';
      await refresh();
    } catch (e) {
      if (!declined(e)) swapError = errorText(e);
    } finally {
      busy = false;
    }
  }

  // ---- liquidity: take out
  let outKey = $state('');
  let outText = $state('');
  let outQuote = $state<{ out0?: string; out1?: string; error?: string } | null>(null);
  const positions = $derived(wallet?.lp ?? []);
  const position = $derived(positions.find((p) => p.asset0.assetId + p.asset1.assetId === outKey) ?? positions[0]);
  const outLp = $derived(parseUnits(outText));
  $effect(() => {
    const p = position;
    const lp = outLp;
    outQuote = null;
    if (!p || lp === null || lp === 0n) return;
    const timer = setTimeout(() => {
      ba.quoteBurn(p.asset0.assetId, p.asset1.assetId, lp)
        .then((q) => (outQuote = q))
        .catch(() => undefined);
    }, 200);
    return () => clearTimeout(timer);
  });
  const outNeedsCoins = $derived(!!position && (heldOf(position.asset0.id) === 0n || heldOf(position.asset1.id) === 0n));
  const outReady = $derived(!!position && outLp !== null && outLp > 0n && outLp <= units(position.lpTokens) && !!outQuote?.out0 && !outNeedsCoins && !busy);
  async function takeOut() {
    if (!outReady || !position || outLp === null) return;
    busy = true;
    swapError = '';
    try {
      await confirmed<string>('amm_burn', [position.asset0.assetId, position.asset1.assetId, unitsNumber(outLp)]);
      toast('success', t('Liquidity taken out'), t('Your share comes back with the next block of BitAssets.'));
      outText = '';
      await refresh();
    } catch (e) {
      if (!declined(e)) swapError = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="page trade">
  <header class="page-head row">
    <h1 class="grow">{t('Trade')}</h1>
    <span class="muted small">{pools.length === 1 ? t('1 pool') : t('%1 pools', formatCount(pools.length))}</span>
  </header>
  <div class="tabs" role="tablist" aria-label={t('Trade')}>
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>
  {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}

  {#if tab === 'pools'}
    <section class="panel">
      <div class="panel-body flush">
        {#if pools.length === 0}
          <div class="empty"><Icon name="pool" size={28} /><p>{t('No pool yet. Open one in Liquidity with two assets you hold.')}</p><button type="button" class="btn" onclick={() => (nav.tab = 'liquidity')}>{t('Add liquidity')}</button></div>
        {:else}
          <ul class="pools">
            {#each pools as p (p.asset0.assetId + p.asset1.assetId)}
              <li>
                <span class="pair"><AssetMark asset={p.asset0} size="s" /><span class="faint">/</span><AssetMark asset={p.asset1} size="s" /></span>
                <span class="num">{formatUnits(p.reserve0)} <span class="faint">/</span> {formatUnits(p.reserve1)}</span>
                <span class="num muted">1 <SafeText text={assetLabel(p.asset0)} /> = {poolPrice(p.reserve0, p.reserve1)} <SafeText text={assetLabel(p.asset1)} /></span>
                <span class="faint num">{t('%1 LP', formatUnits(p.lpTokens))}</span>
                {#if p.tradable}
                  <button type="button" class="btn small ghost" onclick={() => { payId = p.asset0.id ?? ''; getId = p.asset1.id ?? ''; nav.tab = 'swap'; }}><Icon name="swap" size={14} /> {t('Swap')}</button>
                {:else}<span class="badge" title={t('A pool with ECX is shown, not traded through this app')}>{t('read only')}</span>{/if}
              </li>
            {/each}
          </ul>
        {/if}
      </div>
    </section>
  {:else if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else if tab === 'swap'}
    <div class="layout">
      <section class="panel" aria-labelledby="swap-title">
        <div class="panel-head"><h2 id="swap-title">{t('Swap')}</h2></div>
        {#if payable.length === 0}
          <div class="panel-body empty"><Icon name="swap" size={28} /><p>{t('No pool trades an asset this wallet holds. Open a pool in Liquidity, or get an asset that has one.')}</p></div>
        {:else}
          <form
            class="panel-body stack"
            onsubmit={(e) => {
              e.preventDefault();
              swap();
            }}
          >
            <div class="leg">
              <div class="row legtop"><label for="sw-pay">{t('You pay')}</label><span class="faint small">{t('You hold %1', formatUnits(heldOf(payId)))}</span></div>
              <div class="row legrow">
                <input id="sw-pay" type="text" inputmode="numeric" class="num big grow" placeholder="0" bind:value={payText} />
                <select aria-label={t('Asset you pay')} bind:value={payId}>
                  {#each payable as a (a.id)}<option value={a.id}>{assetLabel(a)}</option>{/each}
                </select>
              </div>
              {#if payText && pay === null}<span class="error small">{t('A whole number')}</span>{:else if payTooMuch}<span class="error small">{t('More than this wallet holds')}</span>{/if}
            </div>
            <div class="flipline"><button type="button" class="btn icon small round" aria-label={t('The other way')} onclick={flip} disabled={heldOf(getId) === 0n}><Icon name="swap" size={16} /></button></div>
            <div class="leg">
              <div class="row legtop"><label for="sw-get">{t('You get')}</label><span class="faint small">{t('You hold %1', formatUnits(heldOf(getId)))}</span></div>
              <div class="row legrow">
                <output id="sw-get-amount" class="num big grow">{receive !== null ? formatUnits(receive) : '0'}</output>
                <select id="sw-get" aria-label={t('Asset you get')} bind:value={getId}>
                  {#each partners(payId) as id (id)}<option value={id}>{assetLabel(ref(id))}</option>{/each}
                </select>
              </div>
            </div>
            {#if quote?.error}<p class="error small"><Icon name="alert" size={14} /> {quote.error}</p>{/if}
            {#if needsCoin}<p class="warn small"><Icon name="info" size={14} /> {t('The chain pays an asset out of a pool together with a coin of it this wallet spends: get some %1 first (any amount).', assetLabel(ref(getId)))}</p>{/if}
            <div class="row"><button type="submit" class="btn primary" disabled={!swapReady}><Icon name="swap" size={16} /> {t('Review and swap…')}</button></div>
            {#if swapError}<p class="error"><Icon name="alert" size={14} /> {swapError}</p>{/if}
          </form>
        {/if}
      </section>
      <section class="panel aside">
        <div class="panel-head"><h2>{t('The pool')}</h2></div>
        <div class="panel-body">
          {#if payId && getId && poolOf(payId, getId)}
            {@const p = poolOf(payId, getId)}
            <dl class="facts">
              <dt>{t('Price now')}</dt><dd class="num">1 <SafeText text={assetLabel(ref(payId))} /> = {p ? poolPrice(p.asset0.id === payId ? p.reserve0 : p.reserve1, p.asset0.id === payId ? p.reserve1 : p.reserve0) : '–'} <SafeText text={assetLabel(ref(getId))} /></dd>
              <dt>{t('Holds')}</dt><dd class="num">{formatUnits(p?.reserve0)} <SafeText class="faint" text={assetLabel(p?.asset0)} /> · {formatUnits(p?.reserve1)} <SafeText class="faint" text={assetLabel(p?.asset1)} /></dd>
              {#if receive !== null && pay}<dt>{t('Price impact')}</dt><dd class="num" class:warn={impact > 2}>{impact.toFixed(2)}%</dd>{/if}
              <dt>{t('Pool fee')}</dt><dd>{t('0.3% on what you pay and 0.3% on what you get')}</dd>
            </dl>
          {:else}<p class="muted">{t('Pick two assets of a pool.')}</p>{/if}
          <p class="muted small">{t('The swap is at the pool’s price in the next block. If the pool changes first, the chain refuses it and nothing is spent.')}</p>
        </div>
      </section>
    </div>
  {:else}
    <div class="layout">
      <section class="panel" aria-labelledby="add-title">
        <div class="panel-head"><h2 id="add-title">{t('Add liquidity')}</h2></div>
        {#if heldAssets.length < 2}
          <div class="panel-body empty"><Icon name="pool" size={28} /><p>{t('A pool holds two BitAssets: this wallet needs some of two assets.')}</p><button type="button" class="btn" onclick={() => go('issue' as never)}>{t('Issue an asset')}</button></div>
        {:else}
          <form
            class="panel-body stack"
            onsubmit={(e) => {
              e.preventDefault();
              add();
            }}
          >
            <div class="row legrow">
              <input id="liq-a" type="text" inputmode="numeric" class="num grow" placeholder="0" aria-label={t('First amount')} bind:value={addAText} onchange={matchB} />
              <select aria-label={t('First asset')} bind:value={addA}>{#each heldAssets as a (a.id)}<option value={a.id}>{assetLabel(a)}</option>{/each}</select>
            </div>
            <div class="row legrow">
              <input id="liq-b" type="text" inputmode="numeric" class="num grow" placeholder="0" aria-label={t('Second amount')} bind:value={addBText} />
              <select aria-label={t('Second asset')} bind:value={addB}>{#each heldAssets.filter((a) => a.id !== addA) as a (a.id)}<option value={a.id}>{assetLabel(a)}</option>{/each}</select>
            </div>
            <p class="muted small">
              {#if addPool}{t('The pool exists: put both in at its price (1 %1 = %2 %3).', assetLabel(ref(addA)), poolPrice(addPool.asset0.id === addA ? addPool.reserve0 : addPool.reserve1, addPool.asset0.id === addA ? addPool.reserve1 : addPool.reserve0), assetLabel(ref(addB)))}
              {:else if addA && addB}{t('No pool of these two yet: yours opens it, at the price your two amounts make.')}{/if}
            </p>
            {#if addQuote?.lpTokens}<p class="small">{t('You get %1 LP tokens', formatUnits(addQuote.lpTokens))}</p>{:else if addQuote?.error}<p class="error small">{addQuote.error}</p>{/if}
            <div class="row"><button type="submit" class="btn primary" disabled={!addReady}><Icon name="plus" size={16} /> {addPool ? t('Review and add…') : t('Review and open the pool…')}</button></div>
          </form>
        {/if}
      </section>
      <section class="panel" aria-labelledby="out-title">
        <div class="panel-head"><h2 id="out-title">{t('Take liquidity out')}</h2></div>
        {#if positions.length === 0}
          <div class="panel-body"><p class="muted">{t('This wallet has no share of a pool.')}</p></div>
        {:else}
          <form
            class="panel-body stack"
            onsubmit={(e) => {
              e.preventDefault();
              takeOut();
            }}
          >
            <div class="field">
              <label for="out-pool">{t('Pool')}</label>
              <select id="out-pool" bind:value={outKey}>
                {#each positions as p (p.asset0.assetId + p.asset1.assetId)}<option value={p.asset0.assetId + p.asset1.assetId}>{assetLabel(p.asset0)} / {assetLabel(p.asset1)} · {formatUnits(p.lpTokens)} LP</option>{/each}
              </select>
            </div>
            <div class="field">
              <label for="out-lp">{t('LP tokens to burn')}</label>
              <input id="out-lp" type="text" inputmode="numeric" class="num" placeholder="0" bind:value={outText} />
              {#if position}<span class="hint">{t('You hold %1', formatUnits(position.lpTokens))} <button type="button" class="linkbtn" onclick={() => (outText = position?.lpTokens ?? '')}>{t('All')}</button></span>{/if}
            </div>
            {#if outQuote?.out0}<p class="small">{t('You get %1 %2 and %3 %4', formatUnits(outQuote.out0), assetLabel(position?.asset0), formatUnits(outQuote.out1), assetLabel(position?.asset1))}</p>{:else if outQuote?.error}<p class="error small">{outQuote.error}</p>{/if}
            {#if outNeedsCoins}<p class="warn small"><Icon name="info" size={14} /> {t('Taking liquidity out spends a coin of each asset of the pool with the LP tokens (the chain’s rules): this wallet needs some of both.')}</p>{/if}
            <div class="row"><button type="submit" class="btn" disabled={!outReady}><Icon name="down" size={16} /> {t('Review and take out…')}</button></div>
          </form>
        {/if}
      </section>
    </div>
    {#if swapError}<p class="error"><Icon name="alert" size={14} /> {swapError}</p>{/if}
  {/if}
</div>

<style>
  .trade {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .small {
    font-size: 0.86rem;
  }
  .flush {
    padding: 0;
  }
  .layout {
    display: grid;
    grid-template-columns: minmax(22rem, 1.3fr) minmax(16rem, 1fr);
    gap: 1rem;
    align-items: start;
  }
  @media (max-width: 900px) {
    .layout {
      grid-template-columns: 1fr;
    }
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .leg {
    display: flex;
    flex-direction: column;
    gap: 0.35rem;
    padding: 0.75rem 0.9rem;
    border-radius: var(--radius-l);
    background: var(--surface-2);
    border: 1px solid var(--line);
  }
  .legtop {
    justify-content: space-between;
    align-items: baseline;
  }
  .legtop label {
    font-weight: 600;
    font-size: 0.9rem;
  }
  .legrow {
    gap: 0.6rem;
    align-items: center;
    flex-wrap: nowrap;
  }
  .legrow select {
    max-width: 12rem;
  }
  .big {
    font-size: 1.45rem;
    font-weight: 650;
  }
  output.big {
    padding: 0.35rem 0;
  }
  .leg input.big {
    border: none;
    background: transparent;
    padding: 0.2rem 0;
    box-shadow: none;
  }
  .flipline {
    display: flex;
    justify-content: center;
    margin: -0.65rem 0;
    z-index: 1;
  }
  .aside .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
  }
  .pools {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .pools li {
    display: grid;
    grid-template-columns: minmax(12rem, 1.2fr) minmax(9rem, 1fr) minmax(10rem, 1.2fr) auto auto;
    gap: 1rem;
    align-items: center;
    padding: 0.75rem 1.1rem;
    border-top: 1px solid var(--line);
  }
  .pools li:first-child {
    border-top: none;
  }
  .pair {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }
  .empty {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 0.6rem;
    padding: 2rem 1rem;
    color: var(--text-muted);
    text-align: center;
  }
  .empty p {
    margin: 0;
    max-width: 28rem;
  }
  .warn {
    color: var(--warning);
  }
  p.warn,
  p.error {
    display: flex;
    gap: 0.35rem;
    align-items: flex-start;
    margin: 0;
  }
  .linkbtn {
    background: none;
    border: none;
    color: var(--accent);
    cursor: pointer;
    padding: 0;
    font: inherit;
  }
  .error {
    color: var(--negative);
  }
</style>
