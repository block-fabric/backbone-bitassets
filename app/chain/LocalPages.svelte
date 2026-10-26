<script lang="ts">
  // The pages BitAssets draws: the drop-in pages in the kit's places (chain/overrides.ts), and its own (chain/local.ts).
  import Explorer from './dropin/Explorer.svelte';
  import History from './dropin/History.svelte';
  import Home from './dropin/Home.svelte';
  import NodePage from './dropin/NodePage.svelte';
  import Overview from './dropin/Overview.svelte';
  import Peg from './dropin/Peg.svelte';
  import Receive from './dropin/Receive.svelte';
  import Send from './dropin/Send.svelte';
  import Assets from './pages/Assets.svelte';
  import Auctions from './pages/Auctions.svelte';
  import Issue from './pages/Issue.svelte';
  import MyAssets from './pages/MyAssets.svelte';
  import Trade from './pages/Trade.svelte';
  let { page }: { page: string } = $props();
</script>

{#if page === 'home'}<Home />
{:else if page === 'overview'}<Overview />
{:else if page === 'send'}<Send />
{:else if page === 'receive'}<Receive />
{:else if page === 'transactions'}<History />
{:else if page === 'peg'}<Peg />
{:else if page === 'explorer'}<Explorer />
{:else if page === 'node'}<NodePage />
{:else if page === 'my-assets'}<MyAssets />
{:else if page === 'issue'}<Issue />
{:else if page === 'assets'}<Assets />
{:else if page === 'trade'}<Trade />
{:else if page === 'auctions'}<Auctions />
{/if}
