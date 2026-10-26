// The three first steps of BitAssets, on Home in Simple mode until dismissed.
import type { FirstStep } from '../lib/firststeps';

export const FIRST_STEPS: readonly FirstStep[] = [
  { title: 'Bring ECX here', text: 'Coins come from eCash. Drivechains gives a deposit address to pay from an eCash wallet (the Backbone app: Sidechains → BitAssets). They arrive with the next block of BitAssets.', icon: 'down', action: { label: 'Get a deposit address', page: 'peg', tab: 'deposit' } },
  { title: 'Issue an asset', text: 'An asset of your own: reserve its name (hidden), then register it with its supply. It all comes to your wallet.', icon: 'plus', action: { label: 'Issue an asset', page: 'issue' as never } },
  { title: 'Trade assets', text: 'Swap one asset for another in their pool, or sell by Dutch auction, its price falling block by block.', icon: 'arrow', action: { label: 'Swap', page: 'trade' as never } },
];
