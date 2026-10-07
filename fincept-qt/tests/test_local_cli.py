"""Black-box CLI acceptance: isolated SQLite and provider boundary, no user writes/network."""
import hashlib
import json
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

BINARY = Path(sys.argv.pop(1)).resolve()


class LocalCliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.db = self.root / 'fixture.db'
        with sqlite3.connect(self.db) as db:
            db.executescript('''
                CREATE TABLE portfolios(id TEXT PRIMARY KEY,name TEXT,currency TEXT);
                CREATE TABLE portfolio_assets(portfolio_id TEXT,symbol TEXT,quantity REAL,
                                              avg_buy_price REAL,sector TEXT);
                INSERT INTO portfolios VALUES('p1','Example','USD'),('p2','Other','USD'),
                                             ('p3','Duplicate','USD'),('p4','Duplicate','USD');
                INSERT INTO portfolio_assets VALUES('p1','GOOD',10,100,'Tech'),('p1','EURO',5,200,'');
            ''')
        self.prices = self.root / 'prices.json'
        self.prices.write_text(json.dumps({'currency': 'USD', 'prices': {
            'GOOD': {'price': 120, 'change': 5, 'change_percent': 4.35},
            'EURO': {'price': 180, 'change': -2, 'change_percent': -1.10},
        }}))
        (self.root / 'yfinance_data.py').write_text('''
import json, sys, time
args = sys.argv[1:]
action, name = args[:2]
if name == 'HANG': time.sleep(3)
if name == 'BAD': print('bad-json'); sys.exit(0)
if name == 'FAIL':
    print(json.dumps({'error': 'secret https://user:password@example.invalid'})); sys.exit(0)
if action == 'quote':
    result = {'symbol':name,'price':120,'change':5,'change_percent':4.35,
              'currency':'EUR' if name == 'EURO' else 'USD'}
elif action == 'historical_period':
    result = [{'timestamp':i,'close':100+i} for i in range(20)]
elif action == 'search': result = {'query':name,'results':[{'symbol':'GOOD'}]*20}
elif action == 'resolve_symbol': result = {'resolved_symbol':name,'found':True}
else: result = {'symbol':name,'company_name':'Example'}
print(json.dumps(result))
''')
        self.before = self.fingerprint()

    def tearDown(self):
        self.assertEqual(self.before, self.fingerprint(), 'CLI modified the portfolio database')
        self.tmp.cleanup()

    def fingerprint(self):
        return hashlib.sha256(self.db.read_bytes()).digest(), self.db.stat().st_mtime_ns

    def call(self, *args, ok=True):
        command = [str(BINARY), '--json', '--data-root', str(self.root), '--db', str(self.db),
                   '--python', sys.executable, '--scripts', str(self.root), *args]
        result = subprocess.run(command, capture_output=True, text=True, timeout=8, cwd='/private/tmp' if sys.platform == 'darwin' else '/tmp')
        value = json.loads(result.stdout)
        self.assertEqual(value['schema_version'], 1)
        self.assertEqual(value['ok'], ok, result.stdout)
        self.assertEqual(result.returncode == 0, ok, result.stdout)
        self.assertNotIn('password', result.stdout + result.stderr)
        return value.get('data') if ok else value['error']

    def test_doctor_and_profiles_without_app_boot(self):
        data = self.call('doctor')
        self.assertTrue(data['read_only'])
        self.assertFalse(data['online_checked'])
        self.assertFalse(data['auth_required'])
        self.assertEqual(self.call('profiles', 'list')['items'][0]['name'], 'default')
        self.assertTrue(self.call('doctor', '--online')['online_ok'])
        self.assertFalse((self.root / 'profiles.json').exists())

    def test_discovery_pagination_and_name_resolution(self):
        data = self.call('portfolio', 'list', '--limit', '2')
        self.assertEqual(len(data['items']), 2)
        self.assertEqual(data['next_offset'], 2)
        self.assertIsNone(self.call('portfolio', 'list', '--offset', '2', '--limit', '2')['next_offset'])
        self.assertEqual(self.call('portfolio', 'resolve', '--name', 'Example')['id'], 'p1')
        self.assertEqual(self.call('portfolio', 'resolve', '--name', 'Duplicate', ok=False)['code'], 'ambiguous_name')
        self.assertEqual(self.call('portfolio', 'get', "p1' OR 1=1--", ok=False)['code'], 'not_found')
        self.assertEqual(len(self.call('portfolio', 'get', 'p1')['holdings']), 2)

    def test_shared_valuation_and_missing_quote(self):
        data = self.call('portfolio', 'value', 'p1', '--prices', str(self.prices))
        self.assertEqual(data['total_market_value'], 2100)
        self.assertEqual(data['total_cost_basis'], 2000)
        self.assertEqual(data['unrealized_pnl'], 100)
        self.assertTrue(data['complete'])
        self.assertAlmostEqual(sum(h['weight_percent'] for h in data['holdings']), 100)
        self.prices.write_text('{"currency":"USD","prices":{"GOOD":{"price":120}}}')
        data = self.call('portfolio', 'value', 'p1', '--prices', str(self.prices))
        self.assertFalse(data['complete'])
        self.assertEqual(data['total_market_value'], 2200)
        self.assertEqual(data['priced_positions'], 1)
        self.assertFalse(data['day_change_complete'])
        self.assertIsNone(data['day_change'])
        self.assertEqual(data['holdings'][0]['valuation_basis'], 'cost')

    def test_live_marks_never_mix_currency(self):
        data = self.call('portfolio', 'value', 'p1', '--live')
        self.assertFalse(data['complete'])
        self.assertEqual(data['total_market_value'], 2200)
        self.assertEqual(data['warnings'][0]['code'], 'currency_mismatch')
        self.assertEqual(self.call('portfolio', 'value', 'p2', '--live')['total_positions'], 0)

    def test_quote_search_resolve_raw_and_history_bounds(self):
        self.assertEqual(self.call('quotes', 'get', 'good')['symbol'], 'GOOD')
        self.assertEqual(len(self.call('symbols', 'search', 'Apple', '--limit', '3')['results']), 3)
        self.assertTrue(self.call('symbols', 'resolve', '^GSPC')['found'])
        self.assertEqual(self.call('data', 'read', 'info', 'GOOD')['company_name'], 'Example')
        data = self.call('history', 'get', 'GOOD', '--limit', '3')
        self.assertTrue(data['truncated'])
        self.assertEqual([b['timestamp'] for b in data['bars']], [17, 18, 19])

    def test_provider_errors_are_structured_and_redacted(self):
        self.assertEqual(self.call('quotes', 'get', 'FAIL', ok=False)['code'], 'provider_failed')
        self.assertEqual(self.call('quotes', 'get', 'BAD', ok=False)['code'], 'provider_invalid_json')
        self.assertEqual(self.call('quotes', 'get', 'HANG', '--timeout', '1', ok=False)['code'], 'timeout')

    def test_reject_invalid_input_and_writes(self):
        for args in [('portfolio', 'delete', 'p1'), ('quotes', 'get', 'GOOD', '--live'),
                     ('history', 'get', 'GOOD', '--period', 'nonsense'),
                     ('symbols', 'search', 'Apple', '--limit', '51'),
                     ('doctor', '--profile', '../other'), ('doctor', '--unknown'),
                     ('quotes', 'get', '--bad'), ('doctor', '--timeout', '0'),
                     ('portfolio', 'value', 'p1')]:
            self.call(*args, ok=False)
        self.prices.write_text('{"currency":"EUR","prices":{}}')
        self.assertEqual(self.call('portfolio', 'value', 'p1', '--prices', str(self.prices), ok=False)['code'], 'invalid_prices')
        self.prices.write_text('{"currency":"USD","prices":{"GOOD":{"price":-1}}}')
        self.assertEqual(self.call('portfolio', 'value', 'p1', '--prices', str(self.prices), ok=False)['code'], 'invalid_prices')

    def test_missing_db_does_not_create_one(self):
        missing = self.root / 'missing.db'
        self.assertEqual(self.call('portfolio', 'list', '--db', str(missing), ok=False)['code'], 'database_unavailable')
        self.assertFalse(missing.exists())


if __name__ == '__main__':
    unittest.main()
