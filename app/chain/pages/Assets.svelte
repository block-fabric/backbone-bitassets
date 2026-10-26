<script lang="ts">
  // Browse assets: every BitAsset of the chain (the chain keeps an asset's id, the hash of its name, not the name: a name
  // is shown in words when this wallet knows it, else the start of its id), its supply and records, its pools and
  // auctions; a name looked up (taken or free, and its asset). Asset text is anyone's: shown as data.
  import CopyButton from '../../components/CopyButton.svelte';
  import Dialog from '../../components/Dialog.svelte';
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { errorText } from '../../lib/dialogs.svelte';
  import { formatCount, shortHash } from '../../lib/format';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { drop, follow } from '../dropin/store.svelte';
  import { assetLabel, ba, formatUnits, nameProblem, poolPrice, records, units, type AssetDetail, type AssetInfo, type Resolved } from '../ba';
  import AssetMark from '../ba/AssetMark.svelte';

  $effect(() => follow());

  let assets = $state<AssetInfo[]>([]);
  let loaded = $state(false);
  let pools = $state(0);
  let auctions = $state(0);
  let error = $state('');
  $effect(() => {
    void drop.version;
    Promise.all([ba.assets(), ba.pools(), ba.auctions()])
      .then(([a, p, au]) => {
        assets = a;
        pools = p.length;
        auctions = au.length;
        loaded = true;
        error = '';
      })
      .catch((e) => (error = errorText(e)));
  });

  const FILTERS = [
    { id: 'all', label: 'All' },
    { id: 'held', label: 'Held or controlled' },
    { id: 'mine', label: 'Known by name' },
  ];
  let filter = $state('all');
  let query = $state('');
  const shown = $derived(
    assets
      .filter((a) => (filter === 'held' ? units(a.held) > 0n || a.control : filter === 'mine' ? !!a.name : true))
      .filter((a) => {
        const q = query.trim().toLowerCase();
        if (!q) return true;
        return a.id.startsWith(q) || (a.name ?? '').toLowerCase().includes(q) || String(a.seq) === q;
      })
      .slice()
      .sort((x, y) => y.seq - x.seq),
  );
  const heldCount = $derived(assets.filter((a) => units(a.held) > 0n || a.control).length);

  // ---- a name looked up (what its asset is, or that it is free)
  let looked = $state<Resolved | null>(null);
  let lookError = $state('');
  async function lookUp() {
    const name = query.trim();
    lookError = '';
    looked = null;
    if (/^[0-9a-f]{64}$/i.test(name)) {
      void open(name.toLowerCase());
      return;
    }
    const why = nameProblem(name);
    if (why) {
      lookError = t(why);
      return;
    }
    try {
      looked = await ba.resolve(name);
      // A name found is now known by this wallet: the list shows it in words.
      if (looked.registered) assets = await ba.assets();
    } catch (e) {
      lookError = errorText(e);
    }
  }

  // ---- an asset's details
  let detail = $state<AssetDetail | null>(null);
  let detailOpen = $state(false);
  async function open(id: string) {
    try {
      detail = await ba.asset(id);
      detailOpen = true;
    } catch (e) {
      lookError = errorText(e);
    }
  }
</script>

<div class="page assets">
  <header class="page-head row">
    <h1 class="grow">{t('Browse assets')}</h1>
    <span class="muted small">{t('%1 assets · %2 pools · %3 auctions', formatCount(assets.length), formatCount(pools), formatCount(auctions))}</span>
  </header>
  <section class="panel">
    <div class="panel-body stack">
      <form
        class="row find"
        onsubmit={(e) => {
          e.preventDefault();
          void lookUp();
        }}
      >
        <Icon name="search" size={17} />
        <input type="search" class="grow" placeholder={t('Find an asset by its name or its id, or filter the list')} aria-label={t('Find an asset')} bind:value={query} />
        <button type="submit" class="btn small" disabled={!query.trim()}>{t('Look up')}</button>
      </form>
      <div class="row chips">
        {#each FILTERS as f (f.id)}
          <button type="button" class="chip" class:on={filter === f.id} onclick={() => (filter = f.id)}>{t(f.label)}{#if f.id === 'all'} <span class="n">{assets.length}</span>{:else if f.id === 'held'} <span class="n">{heldCount}</span>{/if}</button>
        {/each}
      </div>
      {#if looked}
        <div class="looked" class:taken={looked.registered}>
          <AssetMark asset={{ kind: 'asset', id: looked.id, name: looked.name }} />
          {#if looked.registered}
            <span class="grow">{t('Registered: supply %1', formatUnits(looked.supply))}</span>
            <button type="button" class="btn small" onclick={() => open(looked?.id ?? '')}>{t('Open')}</button>
          {:else}
            <span class="grow good">{t('Free: nobody issued an asset of this name')}</span>
            <button type="button" class="btn small primary" onclick={() => go('issue' as never)}>{t('Issue it')}</button>
          {/if}
        </div>
      {/if}
      {#if lookError}<p class="error"><Icon name="alert" size={14} /> {lookError}</p>{/if}
    </div>
  </section>
  {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}
  {#if loaded && shown.length === 0}
    <section class="panel"><div class="panel-body empty"><Icon name="gem" size={28} /><p>{assets.length ? t('No asset matches.') : t('No asset on the chain yet.')}</p></div></section>
  {/if}
  <div class="grid">
    {#each shown as a (a.id)}
      <button type="button" class="card" onclick={() => open(a.id)}>
        <div class="row top">
          <AssetMark asset={{ kind: 'asset', id: a.id, name: a.name }} control={a.control} />
          <span class="faint seq">#{a.seq}</span>
        </div>
        <div class="recs">
          {#if records(a.data).length}
            {#each records(a.data).slice(0, 2) as [label, value] (label)}<div class="rec"><span class="faint">{t(label)}</span> <SafeText class="mono" text={value} /></div>{/each}
          {:else}<span class="faint">{t('No records.')}</span>{/if}
        </div>
        <div class="row bottom">
          <span class="muted">{t('Supply')} <strong class="num">{formatUnits(a.supply)}</strong></span>
          {#if units(a.held) > 0n}<span class="badge accent">{t('You hold %1', formatUnits(a.held))}</span>{:else if a.control}<span class="badge">{t('Yours')}</span>{/if}
        </div>
      </button>
    {/each}
  </div>
</div>

<Dialog bind:open={detailOpen} title={detail ? assetLabel({ kind: 'asset', id: detail.id, name: detail.name }) : ''} wide onclose={() => (detail = null)}>
  {#if detail}
    <div class="dhead"><AssetMark asset={{ kind: 'asset', id: detail.id, name: detail.name }} size="l" control={detail.control} sub={'#' + detail.seq} /></div>
    <dl class="facts">
      <dt>{t('Asset id')}</dt><dd class="mono break">{detail.id} <CopyButton text={detail.id} /></dd>
      <dt>{t('Supply')}</dt><dd class="num">{formatUnits(detail.supply)}</dd>
      {#if detail.held !== undefined}<dt>{t('You hold')}</dt><dd class="num">{formatUnits(detail.held)}{#if detail.control} · {t('and its control coin')}{/if}</dd>{/if}
      <dt>{t('Registered')}</dt><dd>{t('block %1', formatCount(detail.registeredAt))} · <span class="mono">{shortHash(detail.registrationTxid, 10)}</span></dd>
      {#each records(detail.data) as [label, value] (label)}<dt>{t(label)}</dt><dd class="mono break"><SafeText text={value} /></dd>{/each}
    </dl>
    <h3>{t('Pools')}</h3>
    {#if detail.pools.length === 0}<p class="muted">{t('No pool of this asset yet.')}</p>{/if}
    <ul class="mini">
      {#each detail.pools as p (p.asset0.assetId + p.asset1.assetId)}
        <li>
          <AssetMark asset={p.asset0} size="s" /> <span class="faint">/</span> <AssetMark asset={p.asset1} size="s" />
          <span class="grow num muted">{formatUnits(p.reserve0)} / {formatUnits(p.reserve1)}</span>
          <span class="num">1 = {poolPrice(p.reserve0, p.reserve1)}</span>
        </li>
      {/each}
    </ul>
    <h3>{t('Auctions')}</h3>
    {#if detail.auctions.length === 0}<p class="muted">{t('No auction of this asset.')}</p>{/if}
    <ul class="mini">
      {#each detail.auctions as a (a.id)}
        <li>
          <AssetMark asset={a.base} size="s" /> <span class="faint">{t('for')}</span> <AssetMark asset={a.quote} size="s" />
          <span class="grow muted">{t('blocks %1–%2', formatCount(a.startBlock), formatCount(a.endBlock - 1))}</span>
          <span class="badge">{a.state}</span>
        </li>
      {/each}
    </ul>
    <div class="row actions">
      <button type="button" class="btn small" onclick={() => { detailOpen = false; go('trade' as never, 'swap'); }}><Icon name="swap" size={14} /> {t('Trade')}</button>
      <button type="button" class="btn small" onclick={() => { detailOpen = false; go('auctions' as never); }}><Icon name="gavel" size={14} /> {t('Auctions')}</button>
    </div>
  {/if}
</Dialog>

<style>
  .assets {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .small {
    font-size: 0.88rem;
  }
  .stack {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
  }
  .find {
    gap: 0.6rem;
    align-items: center;
    flex-wrap: nowrap;
    color: var(--text-muted);
  }
  .chips {
    gap: 0.4rem;
  }
  .chip {
    border: 1px solid var(--line);
    background: transparent;
    color: var(--text);
    border-radius: 999px;
    padding: 0.25rem 0.75rem;
    font-size: 0.82rem;
    cursor: pointer;
  }
  .chip.on {
    border-color: color-mix(in srgb, var(--accent) 70%, transparent);
    background: color-mix(in srgb, var(--accent) 14%, transparent);
  }
  .chip .n {
    font-weight: 700;
    margin-inline-start: 0.2rem;
  }
  .looked {
    display: flex;
    align-items: center;
    gap: 0.8rem;
    padding: 0.6rem 0.8rem;
    border-radius: 0.7rem;
    background: color-mix(in srgb, var(--positive) 9%, transparent);
  }
  .looked.taken {
    background: color-mix(in srgb, var(--accent) 8%, transparent);
  }
  .good {
    color: var(--positive);
  }
  .grid {
    display: grid;
    grid-template-columns: repeat(auto-fill, minmax(17rem, 1fr));
    gap: 1rem;
  }
  .card {
    display: flex;
    flex-direction: column;
    gap: 0.7rem;
    text-align: start;
    padding: 1rem 1.1rem;
    border-radius: var(--radius-xl);
    border: 1px solid var(--line);
    background: linear-gradient(180deg, var(--card-top), transparent 9rem), var(--surface);
    box-shadow: var(--shadow-card);
    color: var(--text);
    cursor: pointer;
  }
  .card:hover {
    border-color: color-mix(in srgb, var(--accent) 50%, var(--line));
  }
  .top,
  .bottom {
    justify-content: space-between;
    align-items: center;
    flex-wrap: nowrap;
    gap: 0.5rem;
  }
  .seq {
    font-size: 0.8rem;
  }
  .recs {
    min-height: 2.6rem;
    font-size: 0.85rem;
    display: flex;
    flex-direction: column;
    gap: 0.15rem;
  }
  .rec {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .empty {
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 0.5rem;
    color: var(--text-muted);
    padding: 1.6rem;
  }
  .empty p {
    margin: 0;
  }
  .dhead {
    margin-bottom: 0.8rem;
  }
  .break {
    word-break: break-all;
  }
  .mini {
    list-style: none;
    margin: 0 0 0.8rem;
    padding: 0;
  }
  .mini li {
    display: flex;
    align-items: center;
    gap: 0.5rem;
    padding: 0.4rem 0;
    border-top: 1px solid var(--line);
  }
  .actions {
    gap: 0.5rem;
    margin-top: 0.6rem;
  }
  .error {
    color: var(--negative);
    display: flex;
    gap: 0.35rem;
    align-items: center;
    margin: 0;
  }
</style>
