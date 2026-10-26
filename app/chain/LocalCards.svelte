<script lang="ts">
  // BitAssets' card on Home: what the wallet holds of its assets, and issuing or trading one.
  import Icon from '../components/Icon.svelte';
  import Card from '../views/home/Card.svelte';
  import { t } from '../lib/i18n.svelte';
  import { go } from '../lib/nav.svelte';
  import { drop } from './dropin/store.svelte';
  import { ba, formatUnits, units, type WalletAssets } from './ba';
  import AssetMark from './ba/AssetMark.svelte';

  let wallet = $state<WalletAssets | null>(null);
  $effect(() => {
    void drop.version;
    if (drop.seed?.hasSeed) ba.wallet().then((w) => (wallet = w)).catch(() => undefined);
  });
  const held = $derived((wallet?.assets ?? []).filter((a) => units(a.amount) > 0n));
</script>

<Card title={t('My assets')} icon="holdings" open={() => go('my-assets' as never)} openLabel={t('Open My assets')}>
  <div class="big num">{held.length}<span class="unit">{held.length === 1 ? t('asset') : t('assets')}</span></div>
  {#if held.length}
    <ul class="few">
      {#each held.slice(0, 3) as a (a.id)}
        <li><AssetMark asset={a} size="s" control={a.control} /><span class="num">{formatUnits(a.amount)}</span></li>
      {/each}
    </ul>
  {:else}
    <p class="muted small">{t('Issue an asset of your own, or trade for one.')}</p>
  {/if}
  {#snippet actions()}
    <button type="button" class="btn small" onclick={() => go('issue' as never)}><Icon name="plus" size={15} /> {t('Issue')}</button>
    <button type="button" class="btn small" onclick={() => go('trade' as never)}><Icon name="swap" size={15} /> {t('Swap')}</button>
  {/snippet}
</Card>

<style>
  .big {
    font-size: 1.65rem;
    font-weight: 650;
    margin-bottom: 0.4rem;
  }
  .big .unit {
    font-size: 0.85rem;
    font-weight: 500;
    color: var(--text-muted);
    margin-inline-start: 0.4rem;
  }
  .few {
    list-style: none;
    margin: 0;
    padding: 0;
    display: flex;
    flex-direction: column;
    gap: 0.3rem;
  }
  .few li {
    display: flex;
    justify-content: space-between;
    align-items: center;
    gap: 0.5rem;
  }
  .small {
    font-size: 0.85rem;
    margin: 0;
  }
</style>
