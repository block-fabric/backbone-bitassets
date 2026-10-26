<script lang="ts">
  // Dutch auctions: an asset sold for another at a price that falls block by block, from its starting price to its final
  // one; anyone buys part or all of what is left at the price of the block (dutch_auction_bid [id, bid_size]); once it
  // ends, its seller collects what was paid and what was not sold (dutch_auction_collect [id], with our wallet's collect).
  // Selling: dutch_auction_create [params]. Each asked in the app's trusted dialog; prices are the node's own arithmetic.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { formatCount } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { nav } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import SetupWallet from '../dropin/SetupWallet.svelte';
  import { confirmed } from '../dropin/api';
  import { drop, follow, refresh } from '../dropin/store.svelte';
  import { assetIdOf, assetLabel, auctionCurve, ba, formatUnits, parseUnits, ratioText, units, unitsNumber, type AssetInfo, type AuctionView, type WalletAssets } from '../ba';
  import AssetMark from '../ba/AssetMark.svelte';

  $effect(() => follow());

  const TABS = [
    { id: 'browse', label: 'Auctions' },
    { id: 'sell', label: 'Sell by auction' },
  ];
  const tab = $derived(TABS.some((x) => x.id === nav.tab) ? nav.tab : 'browse');

  let auctions = $state<AuctionView[]>([]);
  let assets = $state<AssetInfo[]>([]);
  let wallet = $state<WalletAssets | null>(null);
  let loaded = $state(false);
  let error = $state('');
  $effect(() => {
    void drop.version;
    Promise.all([ba.auctions(), ba.assets(), drop.seed?.hasSeed ? ba.wallet() : Promise.resolve(null)])
      .then(([a, s, w]) => {
        auctions = a;
        assets = s;
        wallet = w;
        loaded = true;
        error = '';
      })
      .catch((e) => (error = errorText(e)));
  });
  const ref = (id: string) => ({ kind: 'asset' as const, id, name: assets.find((x) => x.id === id)?.name });
  const heldOf = (id: string | undefined) => units(wallet?.assets.find((a) => a.id === id)?.amount);
  const order = { open: 0, upcoming: 1, closing: 2, ended: 2, collected: 3 } as const;
  const sorted = $derived(auctions.slice().sort((a, b) => order[a.state] - order[b.state] || b.startBlock - a.startBlock));

  function stateText(a: AuctionView): string {
    switch (a.state) {
      case 'upcoming':
        return t('starts at block %1', formatCount(a.startBlock));
      case 'open':
        return t('open until block %1', formatCount(a.endBlock - 1));
      case 'closing':
        return t('closed: collected from block %1', formatCount(a.endBlock + 1));
      case 'ended':
        return a.mine ? t('ended: collect it') : t('ended');
      case 'collected':
        return t('collected');
    }
  }
  /** Where the auction is between its first and its last block (0..1). */
  function progress(a: AuctionView): number {
    if (a.nextHeight <= a.startBlock) return 0;
    return Math.min(1, (a.nextHeight - a.startBlock) / Math.max(1, a.duration));
  }

  // ---- a bid
  let bidId = $state('');
  let bidText = $state('');
  let bidQuote = $state<{ price: string | null; quantity?: string; error?: string } | null>(null);
  let busy = $state(false);
  let actError = $state('');
  const bidOn = $derived(auctions.find((a) => a.id === bidId) ?? null);
  const bid = $derived(parseUnits(bidText));
  $effect(() => {
    const a = bidOn;
    const b = bid;
    void drop.version;
    bidQuote = null;
    if (!a || b === null || b === 0n) return;
    const timer = setTimeout(() => {
      ba.bidQuote(a.id, b)
        .then((q) => (bidQuote = q))
        .catch((e) => (actError = errorText(e)));
    }, 200);
    return () => clearTimeout(timer);
  });
  const bidReady = $derived(!!bidOn && bid !== null && bid > 0n && bid <= heldOf(bidOn.quote.id) && !!bidQuote?.quantity && !busy);
  async function placeBid() {
    if (!bidReady || !bidOn || bid === null) return;
    busy = true;
    actError = '';
    try {
      const got = await confirmed<number>('dutch_auction_bid', [bidOn.id, unitsNumber(bid)]);
      toast('success', t('Bid sent'), t('%1 %2 come to you with the next block of BitAssets.', formatUnits(String(got)), assetLabel(bidOn.base)));
      bidText = '';
      bidId = '';
      await refresh();
    } catch (e) {
      if (!declined(e)) actError = errorText(e);
    } finally {
      busy = false;
    }
  }
  async function collect(id: string) {
    busy = true;
    actError = '';
    try {
      await confirmed<unknown>('dutch_auction_collect', [id]);
      toast('success', t('Collected'), t('What the auction raised, and what it did not sell, come with the next block.'));
      await refresh();
    } catch (e) {
      if (!declined(e)) actError = errorText(e);
    } finally {
      busy = false;
    }
  }

  // ---- sell
  const sellable = $derived((wallet?.assets ?? []).filter((a) => units(a.amount) > 0n));
  let baseId = $state('');
  let quoteId = $state('');
  let amountText = $state('');
  let startIn = $state('2');
  let durationText = $state('20');
  let initialText = $state('');
  let finalText = $state('');
  $effect(() => {
    if (!baseId && sellable.length) baseId = sellable[0].id ?? '';
    if ((!quoteId || quoteId === baseId) && assets.length > 1) quoteId = assets.find((a) => a.id !== baseId)?.id ?? '';
  });
  const amount = $derived(parseUnits(amountText));
  const delay = $derived(parseUnits(startIn));
  const duration = $derived(parseUnits(durationText));
  const initial = $derived(parseUnits(initialText));
  const final_ = $derived(parseUnits(finalText));
  const nextHeight = $derived((drop.overview?.chain.height ?? -1) + 1);
  const startBlock = $derived(delay !== null ? nextHeight + Number(delay) : 0);
  const sellProblem = $derived.by(() => {
    if (!baseId || !quoteId) return t('Pick the asset you sell and the one it is priced in');
    if (baseId === quoteId) return t('Sell an asset for another one');
    if (amount === null || amount === 0n) return t('How much you sell');
    if (amount > heldOf(baseId)) return t('More than this wallet holds');
    if (delay === null || delay < 1n || delay > 10_000n) return t('It starts 1 to 10,000 blocks from now');
    if (duration === null || duration < 1n || duration > 1_000_000n) return t('It lasts 1 to 1,000,000 blocks');
    if (initial === null || initial === 0n) return t('A starting price above zero');
    if (final_ === null) return t('A final price');
    if (final_ > initial) return t('The final price cannot be above the starting price');
    if (duration === 1n && final_ !== initial) return t('An auction of one block has one price');
    return '';
  });
  const curve = $derived(
    duration !== null && initial !== null && final_ !== null && duration > 0n && duration <= 1_000_000n
      ? auctionCurve({ startBlock, duration: Number(duration), initialPrice: initial.toString(), finalPrice: final_.toString() })
      : [],
  );
  const curvePath = $derived.by(() => {
    if (curve.length < 2) return '';
    const max = Math.max(...curve.map((c) => c.price), 1);
    const n = curve.length - 1;
    return curve.map((c, i) => `${i === 0 ? 'M' : 'L'}${((i / n) * 300).toFixed(1)},${(8 + (1 - c.price / max) * 104).toFixed(1)}`).join(' ');
  });
  async function sell() {
    if (sellProblem || amount === null || initial === null || final_ === null || duration === null) return;
    busy = true;
    actError = '';
    try {
      await confirmed<string>('dutch_auction_create', [
        {
          start_block: startBlock,
          duration: Number(duration),
          base_asset: assetIdOf(baseId),
          base_amount: unitsNumber(amount),
          quote_asset: assetIdOf(quoteId),
          initial_price: unitsNumber(initial),
          final_price: unitsNumber(final_),
        },
      ]);
      toast('success', t('Auction started'), t('It opens at block %1. Collect it once it ends (My assets, or here).', formatCount(startBlock)));
      amountText = '';
      await refresh();
      nav.tab = 'browse';
    } catch (e) {
      if (!declined(e)) actError = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="page auctions">
  <header class="page-head row">
    <h1 class="grow">{t('Auctions')}</h1>
    <span class="muted small">{t('next block %1', formatCount(nextHeight))}</span>
  </header>
  <div class="tabs" role="tablist" aria-label={t('Auctions')}>
    {#each TABS as tb (tb.id)}
      <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
    {/each}
  </div>
  {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}
  {#if actError}<p class="error"><Icon name="alert" size={14} /> {actError}</p>{/if}

  {#if tab === 'browse'}
    {#if loaded && auctions.length === 0}
      <section class="panel"><div class="panel-body empty"><Icon name="gavel" size={28} /><p>{t('No auction on the chain yet.')}</p><button type="button" class="btn" onclick={() => (nav.tab = 'sell')}>{t('Sell by auction')}</button></div></section>
    {/if}
    <div class="list">
      {#each sorted as a (a.id)}
        <section class="panel auction" class:open={a.state === 'open'}>
          <div class="panel-body">
            <div class="row head">
              <AssetMark asset={a.base} size="l" sub={t('for %1', assetLabel(a.quote))} />
              <span class="grow"></span>
              {#if a.mine}<span class="badge accent">{t('yours')}</span>{/if}
              <span class="badge {a.state === 'open' ? 'accent' : a.state === 'ended' && a.mine ? 'warning' : ''}">{stateText(a)}</span>
            </div>
            <div class="bar" aria-hidden="true"><span style:width="{(progress(a) * 100).toFixed(1)}%"></span></div>
            <dl class="nums">
              <div><dt>{t('Price now, for all that is left')}</dt><dd class="num">{a.priceNext ? formatUnits(a.priceNext) : '–'} <SafeText class="faint" text={assetLabel(a.quote)} /></dd></div>
              <div><dt>{t('Per unit')}</dt><dd class="num">{a.priceNext && units(a.baseRemaining) > 0n ? ratioText(units(a.priceNext), units(a.baseRemaining)) : '–'}</dd></div>
              <div><dt>{t('Left')}</dt><dd class="num">{formatUnits(a.baseRemaining)} <span class="faint">/ {formatUnits(a.initialBase)}</span></dd></div>
              <div><dt>{t('Raised')}</dt><dd class="num">{formatUnits(a.quoteRaised)}</dd></div>
              <div><dt>{t('Blocks')}</dt><dd class="num">{formatCount(a.startBlock)}–{formatCount(a.endBlock - 1)}</dd></div>
            </dl>
            {#if !a.tradable}<p class="faint small">{t('An auction with ECX is shown, not traded through this app.')}</p>{/if}
            <div class="row actions">
              {#if a.state === 'open' && a.tradable && drop.seed?.hasSeed}
                {#if bidId === a.id}
                  <input type="text" inputmode="numeric" class="num bidin" placeholder="0" aria-label={t('Your bid in %1', assetLabel(a.quote))} bind:value={bidText} />
                  <span class="muted small">{#if bidQuote?.quantity}{t('for %1 %2', formatUnits(bidQuote.quantity), assetLabel(a.base))}{:else if bidQuote?.error}<span class="bad">{bidQuote.error}</span>{:else}{t('You hold %1 %2', formatUnits(heldOf(a.quote.id)), assetLabel(a.quote))}{/if}</span>
                  <span class="grow"></span>
                  <button type="button" class="btn small ghost" onclick={() => ((bidId = ''), (bidText = ''))}>{t('Cancel')}</button>
                  <button type="button" class="btn small primary" disabled={!bidReady} onclick={placeBid}><Icon name="gavel" size={14} /> {t('Review and bid…')}</button>
                {:else}
                  <button type="button" class="btn small primary" onclick={() => ((bidId = a.id), (bidText = ''))}><Icon name="gavel" size={14} /> {t('Bid')}</button>
                {/if}
              {:else if a.state === 'ended' && a.mine}
                <button type="button" class="btn small primary" disabled={busy} onclick={() => collect(a.id)}>{t('Collect…')}</button>
              {/if}
              <span class="faint mono small idl" title={a.id}>{a.id.slice(0, 12)}…</span>
            </div>
          </div>
        </section>
      {/each}
    </div>
  {:else if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else}
    <div class="layout">
      <section class="panel" aria-labelledby="sell-title">
        <div class="panel-head"><h2 id="sell-title">{t('Sell by Dutch auction')}</h2></div>
        {#if sellable.length === 0}
          <div class="panel-body"><p class="muted">{t('This wallet holds no asset to sell.')}</p></div>
        {:else}
          <form
            class="panel-body stack"
            onsubmit={(e) => {
              e.preventDefault();
              sell();
            }}
          >
            <div class="pair">
              <div class="field">
                <label for="au-base">{t('Sell')}</label>
                <select id="au-base" bind:value={baseId}>{#each sellable as a (a.id)}<option value={a.id}>{assetLabel(a)} · {formatUnits(a.amount)}</option>{/each}</select>
              </div>
              <div class="field">
                <label for="au-amount">{t('Amount (whole units)')}</label>
                <input id="au-amount" type="text" inputmode="numeric" class="num" placeholder="0" bind:value={amountText} />
              </div>
            </div>
            <div class="field">
              <label for="au-quote">{t('Priced in')}</label>
              <select id="au-quote" bind:value={quoteId}>{#each assets.filter((a) => a.id !== baseId) as a (a.id)}<option value={a.id}>{assetLabel(ref(a.id))}</option>{/each}</select>
            </div>
            <div class="pair">
              <div class="field">
                <label for="au-start">{t('Starting price (for all of it)')}</label>
                <input id="au-start" type="text" inputmode="numeric" class="num" placeholder="0" bind:value={initialText} />
              </div>
              <div class="field">
                <label for="au-final">{t('Final price (for all of it)')}</label>
                <input id="au-final" type="text" inputmode="numeric" class="num" placeholder="0" bind:value={finalText} />
              </div>
            </div>
            <div class="pair">
              <div class="field">
                <label for="au-in">{t('Starts in (blocks)')}</label>
                <input id="au-in" type="text" inputmode="numeric" class="num" bind:value={startIn} />
                <span class="hint">{t('at block %1', formatCount(startBlock))}</span>
              </div>
              <div class="field">
                <label for="au-dur">{t('Lasts (blocks)')}</label>
                <input id="au-dur" type="text" inputmode="numeric" class="num" bind:value={durationText} />
              </div>
            </div>
            {#if sellProblem && (amountText || initialText)}<p class="muted small">{sellProblem}</p>{/if}
            <div class="row"><button type="submit" class="btn primary" disabled={!!sellProblem || busy}><Icon name="gavel" size={16} /> {t('Review and start…')}</button></div>
          </form>
        {/if}
      </section>
      <section class="panel aside">
        <div class="panel-head"><h2>{t('Its price, block by block')}</h2></div>
        <div class="panel-body">
          {#if curvePath}
            <svg class="curve" viewBox="0 0 300 120" preserveAspectRatio="none" role="img" aria-label={t('The price falls from the starting price to the final one')}>
              <path d={curvePath + ' L300,120 L0,120 Z'} class="fill" />
              <path d={curvePath} class="line" />
            </svg>
            <div class="row ends small"><span class="num">{formatUnits(initial)} <SafeText class="faint" text={assetLabel(ref(quoteId))} /></span><span class="num">{formatUnits(final_)}</span></div>
            <div class="row ends small faint"><span>{t('block %1', formatCount(startBlock))}</span><span>{t('block %1', formatCount(startBlock + Number(duration ?? 1n) - 1))}</span></div>
          {:else}<p class="muted">{t('Set the prices and the blocks to see its curve.')}</p>{/if}
          <p class="muted small">{t('The asset leaves your wallet now, into the auction. From its first block anyone buys part or all of what is left at the price of the block; each bid lowers the price of the rest in step. Once it ends, collect what was paid and what was not sold.')}</p>
        </div>
      </section>
    </div>
  {/if}
</div>

<style>
  .auctions {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .small {
    font-size: 0.86rem;
  }
  .list {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(26rem, 1fr));
    gap: 1rem;
  }
  .auction .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.75rem;
  }
  .auction.open {
    border-color: color-mix(in srgb, var(--accent) 45%, var(--line));
  }
  .head {
    align-items: center;
    gap: 0.5rem;
    flex-wrap: nowrap;
  }
  .bar {
    height: 0.3rem;
    border-radius: 999px;
    background: var(--surface-2);
    overflow: hidden;
  }
  .bar span {
    display: block;
    height: 100%;
    background: var(--accent);
  }
  .nums {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(7.5rem, 1fr));
    gap: 0.5rem 1rem;
    margin: 0;
  }
  .nums dt {
    color: var(--text-muted);
    font-size: 0.78rem;
  }
  .nums dd {
    margin: 0.1rem 0 0;
    font-weight: 600;
  }
  .actions {
    align-items: center;
    gap: 0.5rem;
    flex-wrap: nowrap;
  }
  .bidin {
    width: 8rem;
  }
  .idl {
    margin-inline-start: auto;
  }
  .bad {
    color: var(--negative);
  }
  .empty {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 0.6rem;
    padding: 2rem 1rem;
    color: var(--text-muted);
  }
  .empty p {
    margin: 0;
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
    gap: 0.8rem;
  }
  .pair {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 0.7rem;
  }
  .aside .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .curve {
    width: 100%;
    height: 9rem;
  }
  .curve .line {
    fill: none;
    stroke: var(--accent);
    stroke-width: 2.5;
    vector-effect: non-scaling-stroke;
  }
  .curve .fill {
    fill: var(--chart-fill, color-mix(in srgb, var(--accent) 14%, transparent));
  }
  .ends {
    justify-content: space-between;
  }
  .error {
    color: var(--negative);
    display: flex;
    gap: 0.35rem;
    align-items: center;
    margin: 0;
  }
</style>
