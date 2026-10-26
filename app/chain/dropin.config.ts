// What the drop-in pages (chain/dropin) need to know of BitAssets.
// (No import of the drop-in pages' modules here: they read this file when they load.)
import type { HistoryRow } from './dropin/api';

/** "+1,000 gold, −250 silver": what a row moved of its assets. */
const assetsText = (r: HistoryRow) =>
  (r.assets ?? []).map((a) => `${a.amount.replace(/^-/, '−').replace(/\B(?=(\d{3})+(?!\d))/g, ',')} ${a.label}`).join(', ');

/** A row of assets: the assets it moved, by their labels (text of the chain: shown as data). */
const assetLine = (r: HistoryRow) => (r.assets ?? []).map((a) => a.label).join(', ') || r.reservedName || (r.names ?? []).join(', ');

export const DROPIN = {
  /** Its slot on eCash. */
  slot: 4,
  /** The fee a payment proposes (satoshis). */
  defaultFee: 1000,
  /** The fee on eCash a deposit or a withdrawal proposes. */
  defaultMainFee: 1000,
  /** What an address looks like, for the placeholder. */
  addressExample: '4XrMw8bmnhchFspSyP4XRUuB5vaR',
  /** plain-bitassets' names of the spends of the shared pages (the same parameters as Thunder's). */
  spend: { transfer: 'transfer', withdraw: 'withdraw' },
  /** Under the deposit address of Drivechains. */
  depositNote: 'This is an address of this wallet, as it is: give exactly this. BitAssets credits a deposit to the plain address (or its own s4_ form); one made to anything else is lost.',
  /** BitAssets' own rows of the activity (dropin.history's kinds): their words, icon, line and type filter. */
  kinds: {
    'asset-reserve': { text: 'Asset name reserved', icon: 'lock', line: assetLine },
    'asset-issue': { text: 'Asset issued', icon: 'issue', filter: 'Assets issued', line: assetLine },
    'asset-send': { text: 'Asset sent', icon: 'send', filter: 'Assets sent', line: assetLine },
    'asset-receive': { text: 'Asset received', icon: 'receive', filter: 'Assets received', line: assetLine },
    'asset-mint': { text: 'Asset minted', icon: 'issue', line: assetLine },
    'asset-update': { text: 'Asset records changed', icon: 'edit', line: assetLine },
    swap: { text: 'Swap', icon: 'swap', filter: 'Swaps', line: assetLine },
    'pool-add': { text: 'Liquidity added', icon: 'pool', filter: 'Liquidity', line: assetLine },
    'pool-remove': { text: 'Liquidity taken out', icon: 'pool', line: assetLine },
    'auction-create': { text: 'Auction started', icon: 'gavel', filter: 'Auctions', line: assetLine },
    'auction-bid': { text: 'Auction bid', icon: 'gavel', line: assetLine },
    'auction-collect': { text: 'Auction collected', icon: 'gavel', line: assetLine },
  },
  /** The assets a row moved, in its transaction dialog. */
  rowFacts: (r: HistoryRow) => (r.assets?.length ? [{ label: 'Assets', value: assetsText(r) }] : []),
};

