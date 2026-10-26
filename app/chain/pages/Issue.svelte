<script lang="ts">
  // Issue an asset: is its name free? Reserve it (only its commitment goes on the chain, so nobody can take it first once
  // they see it), then, once the reservation is in a block, register the asset with its whole supply (all to this wallet)
  // and its records. Each step asked in the app's trusted dialog (reserve_bitasset, register_bitasset).
  import SafeText from '../../components/SafeText.svelte';
  import Icon from '../../components/Icon.svelte';
  import { declined, errorText } from '../../lib/dialogs.svelte';
  import { t } from '../../lib/i18n.svelte';
  import { go } from '../../lib/nav.svelte';
  import { toast } from '../../lib/state.svelte';
  import SetupWallet from '../dropin/SetupWallet.svelte';
  import { confirmed } from '../dropin/api';
  import { drop, follow, refresh } from '../dropin/store.svelte';
  import { ba, formatUnits, nameProblem, parseUnits, unitsNumber, type Resolved, type WalletAssets } from '../ba';
  import AssetMark from '../ba/AssetMark.svelte';

  $effect(() => follow());

  let name = $state('');
  let checked = $state<Resolved | null>(null);
  let busy = $state(false);
  let error = $state('');
  let wallet = $state<WalletAssets | null>(null);
  /** Names this page reserved (their reservation not in a block yet). */
  let reservedHere = $state<string[]>([]);
  let supplyText = $state('1000000');
  let ipv4 = $state('');
  let ipv6 = $state('');

  const problem = $derived(nameProblem(name));
  const supply = $derived(parseUnits(supplyText));
  const pending = $derived(wallet?.pending ?? []);
  const has = (list: { id: string }[] | undefined, id: string) => !!list?.some((n) => n.id === id);
  /** Where this name is: free; reserved here (its reservation waiting for a block); ready to register; registering
   *  (waiting for its block); yours; or taken. */
  const step = $derived.by(() => {
    if (!checked || checked.name !== name) return 'type';
    if (checked.registered) return wallet?.assets.some((a) => a.id === checked?.id && a.control) ? 'yours' : 'taken';
    if (has(wallet?.registering, checked.id)) return 'registering';
    if (has(pending, checked.id)) return 'register';
    if (reservedHere.includes(name)) return 'waiting';
    return 'free';
  });

  // The wallet's assets and the name typed, looked up again at each new block (a reservation or a registration in it).
  $effect(() => {
    void drop.version;
    if (!drop.seed?.hasSeed) return;
    ba.wallet()
      .then((w) => (wallet = w))
      .catch(() => (wallet = null));
  });
  $effect(() => {
    const text = name;
    void drop.version;
    if (nameProblem(text)) {
      checked = null;
      return;
    }
    const timer = setTimeout(() => {
      ba.resolve(text)
        .then((r) => {
          if (text === name) checked = r;
        })
        .catch((e) => (error = errorText(e)));
    }, 250);
    return () => clearTimeout(timer);
  });

  async function reserve() {
    busy = true;
    error = '';
    try {
      await confirmed<string>('reserve_bitasset', [name]);
      reservedHere = [...reservedHere, name];
      toast('success', t('Reserved'), t('Register %1 once the reservation is in a block (the next block of BitAssets).', name));
      await refresh();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }

  async function register() {
    if (supply === null) return;
    busy = true;
    error = '';
    try {
      const data: Record<string, unknown> = {};
      if (ipv4.trim()) data.socket_addr_v4 = ipv4.trim();
      if (ipv6.trim()) data.socket_addr_v6 = ipv6.trim();
      const params: unknown[] = [name, unitsNumber(supply)];
      if (Object.keys(data).length) params.push(data);
      await confirmed<string>('register_bitasset', params);
      toast('success', t('Registration sent'), t('%1 is yours once the registration is in a block.', name));
      await refresh();
      wallet = await ba.wallet();
    } catch (e) {
      if (!declined(e)) error = errorText(e);
    } finally {
      busy = false;
    }
  }
</script>

<div class="page issue">
  <header class="page-head row">
    <h1 class="grow">{t('Issue an asset')}</h1>
    <button type="button" class="btn small" onclick={() => go('my-assets' as never)}><Icon name="wallet" size={15} /> {t('My assets')}</button>
  </header>
  {#if drop.seed && !drop.seed.hasSeed}
    <SetupWallet compact />
  {:else}
    <section class="panel search">
      <div class="panel-body">
        <label for="ba-name" class="big-label">{t('Its name')}</label>
        <div class="namerow">
          {#if checked && !problem}<AssetMark asset={{ kind: 'asset', id: checked.id, name }} size="l" showLabel={false} />{/if}
          <input id="ba-name" type="text" class="big grow" placeholder={t('a name, e.g. gold')} spellcheck="false" autocomplete="off" bind:value={name} />
        </div>
        <div class="verdict">
          {#if !name}<span class="muted">{t('Any text: a name is unique on BitAssets, once registered. The chain keeps only its hash, the asset’s id.')}</span>
          {:else if problem}<span class="bad"><Icon name="alert" size={15} /> {t(problem)}</span>
          {:else if step === 'type'}<span class="muted">{t('Looking it up…')}</span>
          {:else if step === 'free'}<span class="good"><Icon name="check" size={15} /> {t('Free: you can issue it')}</span>
          {:else if step === 'waiting'}<span class="warn"><Icon name="clock" size={15} /> {t('Reserved: waiting for its block, then register it here')}</span>
          {:else if step === 'register'}<span class="good"><Icon name="check" size={15} /> {t('Your reservation is in a block: register the asset now')}</span>
          {:else if step === 'registering'}<span class="warn"><Icon name="clock" size={15} /> {t('Registered: waiting for its block')}</span>
          {:else if step === 'yours'}<span class="good"><Icon name="check" size={15} /> {t('Yours already: this wallet holds its control coin')}</span>
          {:else}<span class="bad"><Icon name="x" size={15} /> {t('Taken: an asset of this name exists (supply %1)', formatUnits(checked?.supply))}</span>{/if}
        </div>
        {#if checked && !problem}<div class="faint idline mono">{t('Asset id')} {checked.id}</div>{/if}
      </div>
    </section>

    <div class="steps">
      <section class="panel step" class:current={step === 'free'} class:done={step === 'waiting' || step === 'register' || step === 'registering' || step === 'yours'}>
        <div class="panel-body">
          <span class="num-badge">1</span>
          <h2>{t('Reserve the name')}</h2>
          <p class="muted">{t('Only a commitment to the name goes on the chain: nobody learns it, so nobody can take it before you. It spends one of your ECX coins back to you.')}</p>
          <button type="button" class="btn primary" disabled={busy || step !== 'free'} onclick={reserve}><Icon name="lock" size={15} /> {t('Reserve…')}</button>
        </div>
      </section>
      <section class="panel step" class:current={step === 'register'} class:done={step === 'registering' || step === 'yours'}>
        <div class="panel-body">
          <span class="num-badge">2</span>
          <h2>{t('Register the asset')}</h2>
          <p class="muted">{t('Reveals the name and creates the asset: its whole supply comes to this wallet, with its control coin.')}</p>
          <div class="field">
            <label for="ba-supply">{t('Supply (whole units)')}</label>
            <input id="ba-supply" type="text" inputmode="numeric" class="num" bind:value={supplyText} />
            {#if supply === null}<span class="error">{t('A whole number')}</span>{:else}<span class="hint">{formatUnits(supply)} {t('units: BitAssets have no decimals')}</span>{/if}
          </div>
          <div class="pair">
            <div class="field">
              <label for="ba-ipv4">{t('IPv4 address and port')} <span class="faint">({t('optional')})</span></label>
              <input id="ba-ipv4" type="text" class="mono" placeholder="203.0.113.7:443" bind:value={ipv4} />
            </div>
            <div class="field">
              <label for="ba-ipv6">{t('IPv6 address and port')} <span class="faint">({t('optional')})</span></label>
              <input id="ba-ipv6" type="text" class="mono" placeholder="[2001:db8::7]:443" bind:value={ipv6} />
            </div>
          </div>
          <button type="button" class="btn primary" disabled={busy || step !== 'register' || supply === null} onclick={register}><Icon name="check" size={15} /> {t('Register…')}</button>
        </div>
      </section>
    </div>
    {#if pending.length && step !== 'register'}
      <section class="panel">
        <div class="panel-head"><h2>{t('Reserved, waiting to be registered')}</h2></div>
        <div class="panel-body list">
          {#each pending as p (p.id)}
            <button type="button" class="link" onclick={() => (name = p.name)}><SafeText text={p.name} /></button>
          {/each}
        </div>
      </section>
    {/if}
    {#if error}<p class="error"><Icon name="alert" size={14} /> {error}</p>{/if}
  {/if}
</div>

<style>
  .issue {
    display: flex;
    flex-direction: column;
    gap: 1rem;
  }
  .search .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.5rem;
  }
  .big-label {
    font-weight: 600;
  }
  .namerow {
    display: flex;
    align-items: center;
    gap: 0.75rem;
  }
  input.big {
    font-size: 1.35rem;
    padding: 0.7rem 0.9rem;
  }
  .verdict {
    min-height: 1.5rem;
    display: flex;
    align-items: center;
  }
  .verdict span {
    display: inline-flex;
    gap: 0.35rem;
    align-items: center;
  }
  .idline {
    font-size: 0.78rem;
    word-break: break-all;
  }
  .good {
    color: var(--positive);
  }
  .bad {
    color: var(--negative);
  }
  .warn {
    color: var(--warning);
  }
  .steps {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(20rem, 1fr));
    gap: 1rem;
  }
  .step .panel-body {
    display: flex;
    flex-direction: column;
    gap: 0.6rem;
    align-items: flex-start;
  }
  .step h2 {
    margin: 0;
    font-size: 1.05rem;
  }
  .step p {
    margin: 0;
  }
  .step.current {
    border-color: color-mix(in srgb, var(--accent) 55%, var(--line));
  }
  .step.done {
    opacity: 0.75;
  }
  .num-badge {
    display: grid;
    place-items: center;
    width: 1.8rem;
    height: 1.8rem;
    border-radius: 50%;
    background: color-mix(in srgb, var(--accent) 18%, transparent);
    color: var(--accent);
    font-weight: 700;
  }
  .step .field,
  .pair {
    width: 100%;
  }
  .pair {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(12rem, 1fr));
    gap: 0.6rem;
  }
  .list {
    display: flex;
    flex-wrap: wrap;
    gap: 0.6rem;
  }
  .link {
    background: none;
    border: none;
    color: var(--accent);
    cursor: pointer;
    padding: 0;
  }
  .error {
    color: var(--negative);
  }
</style>
