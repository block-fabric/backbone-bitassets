// The end-to-end run of bitassets-app on a local eCash regtest (run_app.py): the app starts and runs its node; the
// wallet's backup phrase is made (its words only in the trusted dialog); ECX is deposited from eCash through the
// mainchain interface's wallet; a payment is sent to another BitAssets node and one received from it; two assets are
// issued (reserved, then registered), one sent to node B, a pool of both opened, a swap made, a Dutch auction started
// (node B bids on it), collected; a withdrawal to eCash goes through a bundle and is paid; the main pages pictured.
// window.__E2E: { peerAddress, mainAddress, saveFile } from the runner.
(async () => {
  const { host, sleep, log, T, waitFor, check, snapshot, click, type, go, theme, finish, ready } = E2E;
  const overview = () => host('dropin.overview');
  const history = () => host('dropin.history', { limit: 100 });
  const wallet = () => host('bitassets.wallet');
  const ecx = 100_000_000;
  /** Pick an option of a select as the user does (the page's binding hears a change event). */
  function choose(selector, value) {
    const el = document.querySelector(selector);
    if (!el) throw new Error(`no ${selector}`);
    el.value = value;
    el.dispatchEvent(new Event('change', { bubbles: true }));
  }
  const held = async (id) => {
    const a = (await wallet()).assets.find((x) => x.id === id);
    return a ? BigInt(a.amount) : 0n;
  };
  try {
    await theme('midnight');
    await ready();
    await check('the app runs its node (phase ready, a tip)', T().app.phase === 'ready');
    await check('app.info is the node’s and the app’s', T().app.info.name === 'BitAssets' && T().app.info.hostApi === 1 && T().app.info.embedded === false);
    await go('home');
    await waitFor('the setup card', () => document.querySelector('.setup'));

    // ---- the backup phrase: made by the node, shown in the trusted dialog (answered yes by the test), the wallet set
    await host('test.confirmAnswer', { yes: true });
    await click('Make a new backup phrase');
    await waitFor('the wallet set up', async () => (await host('dropin.seed.status')).hasSeed);
    const phraseQ = (await host('test.confirmed')).find((q) => q.kind === 'node:phrase.create');
    await check('the phrase was asked in the trusted dialog, its words never recorded', !!phraseQ && phraseQ.details === '(the words, not recorded)');
    await check('the page got no words', !JSON.stringify(T().app).match(/\b(abandon|zoo)\b/));

    // ---- a deposit from eCash, paid by Backbone's wallet (-grpcwallet), to the bare address
    await go('peg', 'deposit');
    await waitFor('the deposit form', () => document.querySelector('#dep-amount'));
    const shownDeposit = document.querySelector('.copyrow .mono')?.textContent?.trim() ?? '';
    await check('the deposit address is the bare address', /^[1-9A-HJ-NP-Za-km-z]{20,40}$/.test(shownDeposit), shownDeposit);
    type('#dep-amount', '10');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and deposit');
    await waitFor('the deposit credited', async () => (await overview()).wallet.available >= 10 * ecx, 300000);
    const q1 = (await host('test.confirmed')).find((q) => q.kind === 'node:deposit');
    await check('the deposit was asked with its amount', !!q1 && q1.rows.some((r) => r.label === 'Amount' && r.value === '10.00000000 ECX'));

    // ---- send to the other BitAssets node (plain-bitassets' transfer)
    await go('send');
    type('#send-to', window.__E2E.peerAddress);
    await waitFor('the address checked', () => document.querySelector('.state.good'));
    type('#send-amount', '1.5');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and send');
    await waitFor('the payment in a block', async () => (await history()).some((r) => r.kind === 'send' && !r.pending), 300000);
    const q2 = (await host('test.confirmed')).find((q) => q.kind === 'node:send');
    await check('the payment was asked with the recipient first', !!q2 && q2.rows[0].label === 'To' && q2.rows[0].value === window.__E2E.peerAddress);

    // ---- receive from it
    await go('receive');
    const shown = await waitFor('an address shown', () => {
      const el = document.querySelector('.addr .mono.big');
      return el ? el.textContent.trim() : null;
    });
    await log(`SIGNAL receive-address ${shown}`);
    await waitFor('the payment received', async () => (await history()).some((r) => r.kind === 'receive' && !r.pending && r.amount === 0.25 * ecx), 300000);
    await check('a payment received at the address shown', true);

    // ---- two assets issued: both names reserved, then registered once the reservations are in a block
    await go('issue');
    for (const name of ['gold', 'silver']) {
      type('#ba-name', name);
      await waitFor(`${name} free`, () => document.querySelector('.verdict .good'));
      await host('test.confirmAnswer', { yes: true });
      await click('Reserve');
      await waitFor(`${name} reserved`, async () => (await host('test.confirmed')).filter((q) => q.kind === 'node:asset.reserve').length >= (name === 'gold' ? 1 : 2), 30000);
    }
    await waitFor('both reservations in a block', async () => (await wallet()).pending.length === 2, 300000);
    for (const name of ['gold', 'silver']) {
      type('#ba-name', name);
      await waitFor(`${name} ready to register`, () => {
        const b = [...document.querySelectorAll('button')].find((x) => x.textContent.includes('Register'));
        return b && !b.disabled;
      });
      type('#ba-supply', '1000000');
      await host('test.confirmAnswer', { yes: true });
      await click('Register');
      await waitFor(`${name} registered`, async () => (await host('test.confirmed')).filter((q) => q.kind === 'node:asset.register').length >= (name === 'gold' ? 1 : 2), 30000);
    }
    const gold = (await host('bitassets.resolve', { name: 'gold' })).id;
    const silver = (await host('bitassets.resolve', { name: 'silver' })).id;
    await waitFor('both assets in the wallet', async () => (await held(gold)) === 1000000n && (await held(silver)) === 1000000n, 300000);
    const qr = (await host('test.confirmed')).find((q) => q.kind === 'node:asset.register');
    await check('the registration was asked with its supply', !!qr && qr.rows.some((r) => r.label === 'Supply' && r.value === '1,000,000 units'));
    await go('issue');
    type('#ba-name', 'gold');
    await sleep(1200);
    await snapshot('bitassets-03-issue');

    // ---- an asset sent to node B (its quote of the auction below)
    await go('my-assets', 'send');
    await waitFor('the send form', () => document.querySelector('#as-amount'));
    choose('#as-asset', silver);
    type('#as-to', window.__E2E.peerAddress);
    await waitFor('the address checked', () => document.querySelector('.state.good'));
    type('#as-amount', '50000');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and send');
    await waitFor('the asset sent', async () => (await history()).some((r) => r.kind === 'asset-send' && !r.pending), 300000);
    const qs = (await host('test.confirmed')).find((q) => q.kind === 'node:asset.send');
    await check('the asset was sent, asked with its amount', !!qs && qs.rows.some((r) => r.label === 'Amount' && r.value === '50,000') && (await held(silver)) === 950000n);

    // ---- a pool of gold and silver opened
    await go('trade', 'liquidity');
    await waitFor('the liquidity form', () => document.querySelector('#liq-a'));
    choose('select[aria-label="First asset"]', gold);
    await sleep(300);
    choose('select[aria-label="Second asset"]', silver);
    type('#liq-a', '100000');
    type('#liq-b', '400000');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and open the pool');
    await waitFor('the pool open', async () => (await host('bitassets.pools')).length === 1, 300000);
    await check('the pool holds what was put in', (await host('bitassets.pools'))[0].lpTokens === '200000');

    // ---- a swap of gold for silver (the wallet's swap pays silver out with one of its silver coins)
    await go('trade', 'swap');
    await waitFor('the swap form', () => document.querySelector('#sw-pay'));
    choose('select[aria-label="Asset you pay"]', gold);
    type('#sw-pay', '1000');
    await waitFor('the quote', () => document.querySelector('#sw-get-amount')?.textContent !== '0');
    const quoted = document.querySelector('#sw-get-amount').textContent.replace(/,/g, '');
    await snapshot('bitassets-04-swap');
    const silverBefore = await held(silver);
    await host('test.confirmAnswer', { yes: true });
    await click('Review and swap');
    await waitFor('the swap in a block', async () => (await history()).some((r) => r.kind === 'swap' && !r.pending), 300000);
    await check('the swap paid what was quoted', (await held(silver)) === silverBefore + BigInt(quoted), `${quoted}`);

    // ---- a Dutch auction of gold for silver: node B bids, the app collects once it ends
    await go('auctions', 'sell');
    await waitFor('the sell form', () => document.querySelector('#au-amount'));
    choose('#au-base', gold);
    await sleep(300);
    choose('#au-quote', silver);
    type('#au-amount', '10000');
    type('#au-start', '5000');
    type('#au-final', '1000');
    type('#au-in', '3');
    type('#au-dur', '8');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and start');
    const auction = await waitFor('the auction on the chain', async () => (await host('bitassets.auctions')).find((a) => a.mine), 300000);
    await log(`SIGNAL auction-created ${auction.id} ${auction.startBlock}`);
    await waitFor('node B’s bid in a block', async () => (await host('bitassets.auctions')).some((a) => a.id === auction.id && a.quoteRaised === '1000'), 300000);
    await go('auctions');
    await sleep(1500);
    await snapshot('bitassets-05-auctions');
    await waitFor('the auction ended', async () => (await host('bitassets.auctions')).some((a) => a.id === auction.id && a.state === 'ended'), 300000);
    await go('auctions');
    await host('test.confirmAnswer', { yes: true });
    await click('Collect');
    await waitFor('the auction collected', async () => (await history()).some((r) => r.kind === 'auction-collect' && !r.pending), 300000);
    const qc = (await host('test.confirmed')).find((q) => q.kind === 'node:auction.collect');
    await check('the collect paid what node B bid', !!qc && qc.rows.some((r) => r.label === 'Paid to you' && r.value === '1,000 silver'));

    // ---- withdraw to eCash: a bundle, voted, paid
    await go('peg', 'withdraw');
    type('#w-to', window.__E2E.mainAddress);
    await waitFor('the eCash address checked', () => document.querySelector('.state.good'));
    type('#w-amount', '2');
    await host('test.confirmAnswer', { yes: true });
    await click('Review and withdraw');
    await waitFor('the withdrawal asked', async () => (await host('dropin.peg')).withdrawals.length > 0, 60000);
    const paid = await waitFor('the withdrawal paid on eCash', async () => (await host('dropin.peg')).withdrawals.find((w) => w.state === 'paid'), 900000);
    await check('the withdrawal is paid on eCash', !!paid, JSON.stringify(paid));
    await log(`SIGNAL withdrawal-paid ${paid.m6id}`);

    // ---- the main pages, in the dark theme
    for (const [page, tab, name] of [['home', '', '01-home'], ['my-assets', 'holdings', '02-my-assets'], ['transactions', '', '06-activity'], ['assets', '', '07-browse']]) {
      await go(page, tab);
      await sleep(1500);
      await snapshot(`bitassets-${name}`);
    }
    const kinds = new Set((await history()).map((r) => r.kind));
    await check('the activity says each step', ['asset-reserve', 'asset-issue', 'asset-send', 'pool-add', 'swap', 'auction-create', 'auction-collect'].every((k) => kinds.has(k)), [...kinds].join(','));

    // ---- the backup phrase saved to a file the user chose: asked, then written (the runner checks the file)
    await go('settings', 'dropwallet');
    await host('test.fileAnswer', { path: window.__E2E.saveFile });
    await host('test.confirmAnswer', { yes: true });
    await click('Save it to a file');
    await waitFor('the phrase saved', async () => (await host('test.confirmed')).some((q) => q.kind === 'node:phrase.save'), 30000);
    await sleep(1500);
  } catch (e) {
    await check('the run', false, e && e.stack ? e.stack : String(e));
  }
  await finish();
})();
