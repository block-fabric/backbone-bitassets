<script lang="ts">
  // An asset as the pages show it: a round mark in the hue of its id (the same everywhere, so that two assets of names
  // alike are told apart at a glance) with its first letters, its label (its name when the wallet knows it, else the start
  // of its id; text of the chain: shown as data), and a crown when this wallet holds its control coin. ECX in the colour
  // of the chain.
  import Icon from '../../components/Icon.svelte';
  import SafeText from '../../components/SafeText.svelte';
  import { css } from '../../lib/actions';
  import { t } from '../../lib/i18n.svelte';
  import { assetHue, assetLabel, initials, type AssetRef } from '../ba';

  let {
    asset,
    size = 'm',
    showLabel = true,
    control = false,
    sub = '',
  }: { asset: Pick<AssetRef, 'kind' | 'id' | 'name'>; size?: 's' | 'm' | 'l'; showLabel?: boolean; control?: boolean; sub?: string } = $props();
  const label = $derived(assetLabel(asset));
  const ecx = $derived(asset.kind === 'ecx');
</script>

<span class="asset-mark {size}">
  <span class="mark" class:ecx use:css={{ '--ba-h': String(assetHue(asset.id)) }} aria-hidden="true"><span translate="no">{ecx ? 'E' : initials(asset.name ?? asset.id ?? '?')}</span></span>
  {#if showLabel}
    <span class="words">
      <SafeText class="label" text={label} />
      {#if !asset.name && !ecx}<span class="faint idtag">{t('id')}</span>{/if}
      {#if control}<span class="crown" title={t('This wallet holds its control coin')}><Icon name="crown" size={13} /></span>{/if}
      {#if sub}<span class="sub muted">{sub}</span>{/if}
    </span>
  {/if}
</span>

<style>
  .asset-mark {
    display: inline-flex;
    align-items: center;
    gap: 0.55rem;
    min-width: 0;
  }
  .mark {
    flex: none;
    display: grid;
    place-items: center;
    width: 1.9rem;
    height: 1.9rem;
    border-radius: 50%;
    font-size: 0.7rem;
    font-weight: 750;
    letter-spacing: 0.02em;
    /* The asset's hue, leaning to the theme's text: light on a dark theme, dark on a light one. */
    color: color-mix(in srgb, hsl(var(--ba-h) 70% 50%) 70%, var(--text));
    background: hsl(var(--ba-h) 60% 50% / 0.14);
    border: 1.5px solid color-mix(in srgb, hsl(var(--ba-h) 65% 52%) 80%, transparent);
  }
  .mark.ecx {
    color: var(--accent);
    background: color-mix(in srgb, var(--accent) 14%, transparent);
    border-color: color-mix(in srgb, var(--accent) 70%, transparent);
  }
  .s .mark {
    width: 1.45rem;
    height: 1.45rem;
    font-size: 0.6rem;
  }
  .l .mark {
    width: 2.6rem;
    height: 2.6rem;
    font-size: 0.9rem;
  }
  .words {
    display: inline-flex;
    align-items: baseline;
    gap: 0.4rem;
    min-width: 0;
  }
  .words :global(.label) {
    font-weight: 650;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }
  .l .words :global(.label) {
    font-size: 1.15rem;
  }
  .idtag {
    font-size: 0.72rem;
    text-transform: uppercase;
    letter-spacing: 0.04em;
  }
  .crown {
    color: var(--warning);
    display: inline-flex;
    align-self: center;
  }
  .sub {
    font-size: 0.82rem;
  }
</style>
