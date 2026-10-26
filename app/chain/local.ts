// BitAssets' own pages beside the drop-in pages (chain/overrides.ts puts those in the kit's places): the wallet's assets
// (and sending one), issuing an asset (reserved, then registered), every asset of the chain, trading in the pools (swaps
// and liquidity) and the Dutch auctions.
import type { IconName } from '../components/Icon.svelte';

export interface LocalPage {
  id: string;
  title: string;
  icon: IconName;
  summary: string;
  section: 'wallet' | 'chain' | 'tools';
  wallet?: boolean;
  shortcut?: string;
  keywords?: string;
  quick?: { label: string; icon?: IconName; tab?: string; simpleLabel?: string };
  simpleTitle?: string;
  advanced?: boolean;
  simple?: boolean;
  simpleSummary?: string;
  tabOf?: { page: string; tab: string };
}

export const LOCAL_PAGES = [
  { id: 'my-assets', title: 'My assets', simple: true, simpleSummary: 'The assets you hold', icon: 'holdings', section: 'chain', shortcut: 'Alt+6', summary: 'What this wallet holds and controls: assets, pool shares, auctions to collect; send an asset', wallet: true, keywords: 'portfolio balances holdings send transfer control liquidity receipts' },
  { id: 'issue', title: 'Issue an asset', simpleTitle: 'Create an asset', simple: true, simpleSummary: 'Make an asset of your own', icon: 'issue', section: 'chain', summary: 'Reserve a name, then register an asset with its supply and records', wallet: true, keywords: 'create new asset token register reserve mint', quick: { label: 'Issue an asset', icon: 'issue', simpleLabel: 'Create an asset' } },
  { id: 'assets', title: 'Browse assets', icon: 'gem', section: 'chain', summary: 'Every asset of the chain: its supply, records, pools and auctions; look one up by name', keywords: 'bitassets explore token find search supply lookup' },
  { id: 'trade', title: 'Trade', simpleTitle: 'Swap', simple: true, simpleSummary: 'Trade one asset for another', icon: 'swap', section: 'chain', summary: 'Swap assets in their pools, and add or take out liquidity', keywords: 'swap exchange amm pool liquidity price dex', quick: { label: 'Swap', icon: 'swap' } },
  { id: 'auctions', title: 'Auctions', icon: 'gavel', section: 'chain', summary: 'Buy at a price that falls block by block, or sell by Dutch auction', keywords: 'dutch auction bid sell buy collect' },
] as const satisfies readonly LocalPage[];

export type LocalPageId = (typeof LOCAL_PAGES)[number]['id'];

export interface LocalCommand {
  id: string;
  label: string;
  page: LocalPageId;
  tab?: string;
  tip?: string;
  keywords?: string;
}

export const LOCAL_COMMANDS = [
  { id: 'ba.send', label: 'Send an asset…', page: 'my-assets', tab: 'send', tip: 'Send some of an asset to an address', keywords: 'transfer asset token' },
  { id: 'ba.swap', label: 'Swap assets…', page: 'trade', tab: 'swap', tip: 'Trade one asset for another in their pool', keywords: 'exchange trade' },
  { id: 'ba.liquidity', label: 'Add liquidity…', page: 'trade', tab: 'liquidity', tip: 'Put two assets in their pool for a share of its fees', keywords: 'pool provide' },
  { id: 'ba.sell', label: 'Sell by auction…', page: 'auctions', tab: 'sell', tip: 'Start a Dutch auction of an asset', keywords: 'dutch auction' },
  { id: 'ba.lookup', label: 'Look up an asset…', page: 'assets', tip: 'Whether a name is taken, and what its asset is', keywords: 'resolve lookup name' },
] as const satisfies readonly LocalCommand[];
