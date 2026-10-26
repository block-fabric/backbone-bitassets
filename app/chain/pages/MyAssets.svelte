<script lang="ts">
  // My assets: what this wallet holds of each asset (and the control coins it holds), its shares of pools, the auctions it
  // runs (to collect once they end), its reservations waiting to be registered; and sending an asset, asked in the app's
  // trusted dialog (transfer_bitasset [dest, asset id, amount, fee]).
  import Amount from '../../components/Amount.svelte';
  import AmountInput from '../../components/AmountInput.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { DROPIN } from '../dropin.config';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go, nav } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import AddressField from '../dropin/AddressField.svelte';
  import SetupWallet from '../dropin/SetupWallet.svelte';
  import { confirmed, sats, satsNumber } from '../dropin/api';
  import { drop, follow, refresh } from '../dropin/store.svelte';
  import { assetLabel, ba, formatUnits, parseUnits, units, unitsNumber, type WalletAssets } from '../ba';
  import AssetMark from '../ba/AssetMark.svelte';

  $effect(() => follow());

  const TABS = [
    { id: 'holdings', label: 'Holdings' },
    { id: 'send', label: 'Send an asset' },
  ];
  const tab = $derived(TABS.some((x) => x.id === nav.tab) ? nav.tab : 'holdings');

  let wallet = $state<WalletAssets | null>(null);
  let error = $state('');
  $effect(() => {
    void drop.version;
    if (!drop.seed?.hasSeed) return;
    ba.wallet()
      .then((w) => ((wallet = w), (error = '')))
      .catch((e) => (error = errorText(e)));
  });
  const held = $derived((wallet?.assets ?? []).filter((a) => units(a.amount) > 0n));
  const controlsOnly = $derived((wallet?.assets ?? []).filter((a) => units(a.amount) === 0n && a.control));

  // ---- send an asset
  let sendId = $state('');
  let to = $state('');
  let toValid = $state(false);
  let amountText = $state('');
  let fee = $state<bigint | null>(BigInt(DROPIN.defaultFee));
  let busy = $state(false);
  let sendError = $state('');
  const sendAsset = $derived(held.find((a) => a.id === sendId) ?? held[0]);
  const amount = $derived(parseUnits(amountText));
  const tooMuch = $derived(amount !== null && !!sendAsset && amount > units(sendAsset.amount));
  const ready = $derived(!!sendAsset && toValid && amount !== null && amount > 0n && !tooMuch && fee !== null && !busy);

  function sendOf(id: string) {
    sendId = id;
    nav.tab = 'send';
  }

  async function send() {
    if (!ready || !sendAsset?.id || amount === null || fee === null) return;
    busy = true;
    sendError = '';
    try {
      await confirmed<string>('transfer_bitasset', [to.trim(), sendAsset.id, unitsNumber(amount), satsNumber(fee)]);
      toast('success', t('Sent'), t('%1 %2 sent: it arrives with the next block of BitAssets.', formatUnits(amount), assetLabel(sendAsset)));
      amountText = '';
      to = '';
      await refresh();
      wallet = await ba.wallet();
    } catch (e) {
      if (!declined(e)) sendError = errorText(e);
    } finally {
      busy = false;
    }
  }

  // ---- collect an auction this wallet ran
  async function collect(id: string) {
    try {
      await confirmed<unknown>('dutch_auction_collect', [id]);
      toast('success', t('Collected'), t('What the auction raised, and what it did not sell, come with the next block.'));
      await refresh();
    } catch (e) {
      if (!declined(e)) toast('error', t('Collect'), errorText(e));
    }
  }

  function stateText(s: string): string {
    switch (s) {
      case 'upcoming':
        return t('Starts soon');
      case 'open':
        return t('Open');
      case 'closing':
        return t('Closed: collect it soon');
      case 'ended':
        return t('Ended: collect it');
      case 'collected':
        return t('Collected');
    }
    return s;
  }
</script>

<div class="page myassets">
  <header class="page-head row">
    <h1 class="grow">{t('My assets')}</h1>
    <button type="button" class="btn small" onclick={() => go('issue' as never)}><Icon name="plus" size={15} /> {t('Issue an asset')}</button>
  </header>
  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else}
    <div class="tabs" role="tablist" aria-label={t('My assets')}>
      {#each TABS as tb (tb.id)}
        <button type="button" role="tab" aria-selected={tab === tb.id} onclick={() => (nav.tab = tb.id)}>{t(tb.label)}</button>
      {/each}
    </div>
    {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}

    {#if tab === 'holdings'}
      <section class="panel">
        <div class="panel-head row">
          <h2 class="grow">{t('Assets held')}</h2>
          <span class="muted small">{t('ECX available')} <Amount sats={sats(drop.overview?.wallet.available)} /></span>
        </div>
        <div class="panel-body flush">
          {#if wallet && held.length === 0}
            <div class="empty">
              <Icon name="holdings" size={28} />
              <p>{t('No asset yet. Issue one, buy one in an auction, or receive one at an address of this wallet.')}</p>
              <button type="button" class="btn" onclick={() => go('issue' as never)}>{t('Issue an asset')}</button>
            </div>
          {:else}
            <ul class="holdings">
              {#each held as a (a.id)}
                <li>
                  <AssetMark asset={a} control={a.control} />
                  <span class="supply faint num">{t('of %1', formatUnits(a.supply))}</span>
                  <strong class="num amt">{formatUnits(a.amount)}</strong>
                  <button type="button" class="btn small ghost" onclick={() => sendOf(a.id ?? '')}><Icon name="send" size={14} /> {t('Send')}</button>
                </li>
              {/each}
            </ul>
          {/if}
          {#if controlsOnly.length}
            <p class="controls muted small">
              {t('You control, without holding any:')}
              {#each controlsOnly as a (a.id)}<span class="pill"><AssetMark asset={a} size="s" control /></span>{/each}
            </p>
          {/if}
        </div>
      </section>

      {#if wallet?.lp.length}
        <section class="panel">
          <div class="panel-head row"><h2 class="grow">{t('Pool shares')}</h2><button type="button" class="btn small" onclick={() => go('trade' as never, 'liquidity')}>{t('Liquidity')}</button></div>
          <div class="panel-body flush">
            <ul class="holdings">
              {#each wallet.lp as p (p.asset0.assetId + p.asset1.assetId)}
                <li>
                  <span class="pair"><AssetMark asset={p.asset0} size="s" /> <span class="faint">/</span> <AssetMark asset={p.asset1} size="s" /></span>
                  <span class="faint small">{t('%1 LP tokens', formatUnits(p.lpTokens))}</span>
                  <span class="num share">{formatUnits(p.share0)} <SafeText class="faint" text={assetLabel(p.asset0)} /> + {formatUnits(p.share1)} <SafeText class="faint" text={assetLabel(p.asset1)} /></span>
                  <span></span>
                </li>
              {/each}
            </ul>
          </div>
        </section>
      {/if}

      {#if wallet?.auctions.length}
        <section class="panel">
          <div class="panel-head row"><h2 class="grow">{t('Your auctions')}</h2><button type="button" class="btn small" onclick={() => go('auctions' as never)}>{t('Auctions')}</button></div>
          <div class="panel-body flush">
            <ul class="holdings">
              {#each wallet.auctions as a (a.id)}
                <li>
                  <AssetMark asset={a.base} sub={t('for %1', assetLabel(a.quote))} />
                  <span class="badge {a.state === 'ended' ? 'warning' : a.state === 'open' ? 'accent' : ''}">{stateText(a.state)}</span>
                  <span class="num share">{t('%1 raised · %2 left', formatUnits(a.quoteRaised), formatUnits(a.baseRemaining))}</span>
                  {#if a.state === 'ended'}<button type="button" class="btn small primary" onclick={() => collect(a.id)}>{t('Collect…')}</button>{:else}<span></span>{/if}
                </li>
              {/each}
            </ul>
          </div>
        </section>
      {/if}

      {#if wallet?.pending.length || wallet?.registering.length}
        <section class="panel">
          <div class="panel-head"><h2>{t('Names reserved')}</h2></div>
          <div class="panel-body">
            {#each wallet?.pending ?? [] as p (p.id)}
              <div class="row pend"><span class="grow"><SafeText text={p.name} /></span><button type="button" class="btn small" onclick={() => go('issue' as never)}>{t('Register…')}</button></div>
            {/each}
            {#each wallet?.registering ?? [] as p (p.id)}
              <div class="row pend"><Icon name="clock" size={15} /><span class="grow"><SafeText text={p.name} /></span><span class="muted small">{t('waiting for its block')}</span></div>
            {/each}
          </div>
        </section>
      {/if}
    {:else}
      <div class="layout">
        <section class="panel" aria-labelledby="send-title">
          <div class="panel-head"><h2 id="send-title">{t('Send an asset')}</h2></div>
          {#if held.length === 0}
            <div class="panel-body"><p class="muted">{t('This wallet holds no asset to send.')}</p></div>
          {:else}
            <form
              class="panel-body stack"
              onsubmit={(e) => {
                e.preventDefault();
                send();
              }}
            >
              <div class="field">
                <label for="as-asset">{t('Asset')}</label>
                <select id="as-asset" bind:value={sendId}>
                  {#each held as a (a.id)}<option value={a.id}>{assetLabel(a)} · {formatUnits(a.amount)}</option>{/each}
                </select>
              </div>
              <AddressField id="as-to" label={t('Send to')} bind:value={to} bind:valid={toValid} placeholder={DROPIN.addressExample} />
              <div class="field">
                <label for="as-amount">{t('Amount (whole units)')}</label>
                <input id="as-amount" type="text" inputmode="numeric" class="num" bind:value={amountText} placeholder="0" />
                {#if amountText && amount === null}<span class="error">{t('A whole number')}</span>
                {:else if tooMuch}<span class="error">{t('More than this wallet holds (%1)', formatUnits(sendAsset?.amount))}</span>
                {:else if sendAsset}<span class="hint">{t('This wallet holds %1', formatUnits(sendAsset.amount))} <button type="button" class="linkbtn" onclick={() => (amountText = sendAsset?.amount ?? '')}>{t('Send all')}</button></span>{/if}
              </div>
              <AmountInput id="as-fee" label={t('Fee (ECX)')} bind:value={fee} required />
              <div class="row"><button type="submit" class="btn primary" disabled={!ready}><Icon name="send" size={16} /> {t('Review and send…')}</button></div>
              {#if sendError}<p class="error"><Icon name="alert" size={14} /> {sendError}</p>{/if}
            </form>
          {/if}
        </section>
        <section class="panel aside">
          <div class="panel-body">
            {#if sendAsset}
              <AssetMark asset={sendAsset} size="l" control={sendAsset.control} />
              <dl class="facts">
                <dt>{t('You hold')}</dt><dd class="num">{formatUnits(sendAsset.amount)}</dd>
                <dt>{t('Supply')}</dt><dd class="num">{formatUnits(sendAsset.supply)}</dd>
                <dt>{t('Asset id')}</dt><dd class="mono break small">{sendAsset.id}</dd>
              </dl>
            {/if}
            <p class="muted small">{t('An asset goes to a BitAssets address: anyone’s, yours included. A transfer cannot be taken back. Its fee is paid in ECX.')}</p>
          </div>
        </section>
      </div>
    {/if}
  {/if}
</div>

<style>
  .myassets {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .flush {
    padding: 0;
  }
  .holdings {
    list-style: none;
    margin: 0;
    padding: 0;
  }
  .holdings li {
    display: grid;
    grid-template-columns: minmax(10rem, 1.4fr) minmax(6rem, 1fr) minmax(8rem, auto) auto;
    gap: 1rem;
    align-items: center;
    padding: 0.7rem 1.1rem;
    border-top: 1px solid var(--line);
  }
  .holdings li:first-child {
    border-top: none;
  }
  .amt {
    font-size: 1.05rem;
    text-align: end;
  }
  .share {
    text-align: end;
    white-space: nowrap;
  }
  .pair {
    display: inline-flex;
    align-items: center;
    gap: 0.35rem;
  }
  .supply,
  .small {
    font-size: 0.85rem;
  }
  .controls {
    display: flex;
    flex-wrap: wrap;
    gap: 0.4rem;
    align-items: center;
    padding: 0.7rem 1.1rem;
    margin: 0;
    border-top: 1px solid var(--line);
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
    max-width: 30rem;
  }
  .pend {
    padding: 0.35rem 0;
    gap: 0.5rem;
    align-items: center;
  }
  .layout {
    display: grid;
    grid-template-columns: minmax(22rem, 1.4fr) minmax(16rem, 1fr);
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
    gap: 0.85rem;
  }
  .aside .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.8rem;
  }
  .break {
    word-break: break-all;
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
    display: flex;
    gap: 0.35rem;
    align-items: center;
    margin: 0;
  }
</style>
