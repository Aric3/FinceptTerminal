# Fincept Local CLI

`fincept-local` is a read-only, headless interface for agents and shell scripts.
It uses the existing Qt toolchain and Python yfinance environment. No desktop
startup, login, broker credentials, database migrations, sync, snapshots, or orders.

## Build and install

Inside the existing app build, `FINCEPT_BUILD_CLI=ON` (local edition) adds the target
`fincept-local`. To build only the CLI without configuring the whole desktop app:

```sh
cmake -S fincept-qt/cli -B /path/to/isolated/build-cli -G Ninja \
  -DCMAKE_PREFIX_PATH=/path/to/Qt/6.8.3/macos -DCMAKE_BUILD_TYPE=Release
cmake --build /path/to/isolated/build-cli
ctest --test-dir /path/to/isolated/build-cli --output-on-failure
python3 fincept-qt/cli/install_local.py /path/to/isolated/build-cli/fincept-local
```

The installer adds one launcher to `~/.local/bin`, refuses to replace unrelated
commands, and never edits shell startup files. That directory must be on PATH.
The launcher points to the isolated build; keep that build and Qt SDK available.
On this Mac, Qt execution under the restricted Codex shell can report a missing
`neon` processor feature; host execution works. Do not reinstall Qt for that
sandbox-specific failure.
The default scripts directory is compiled from this checkout. Use `--scripts`
to explicitly select another checkout/installed scripts directory. This first
installation is a development tool, not a self-contained distributable binary.

On this Mac the installed launcher uses
`/Users/ac/workspace/projects/FinceptTerminal-dev/build-cli/fincept-local`.

## Commands

```sh
fincept-local --help
fincept-local --json doctor
fincept-local --json doctor --online
fincept-local --json profiles list
fincept-local --json symbols search Apple --limit 5
fincept-local --json symbols resolve AAPL
fincept-local --json quotes get '^GSPC'
fincept-local --json history get AAPL --period 1y --interval 1d --limit 250
fincept-local --json portfolio list --profile default --limit 20
fincept-local --json portfolio resolve --name 'My portfolio'
fincept-local --json portfolio get PORTFOLIO_ID
fincept-local --json portfolio value PORTFOLIO_ID --prices /absolute/prices.json
fincept-local --json portfolio value PORTFOLIO_ID --live
fincept-local --json data read info AAPL
```

The raw read hatch only permits `quote`, `info`, and `resolve_symbol`; it cannot
execute arbitrary tools, scripts or network methods. Symbol resolution uses the
existing Yahoo heuristic (including Indian `.NS`/`.BO` suffixes), not a global
security master. Search candidates should be checked before selecting a ticker.

Defaults: profile `default`, period `1mo`, interval `1d`, 500 history bars, 100
portfolios, 10 search results, total command timeout 120 seconds. `--timeout`
accepts 1..600 seconds across all subprocesses, and kills the CLI-owned process
on expiry. History returns the latest N available bars in oldest-first order
with `available`, `returned` and `truncated`. Portfolio pagination returns
`next_offset`; lists are sorted by name then ID (not a snapshot across invocations).

Select a profile explicitly with `--profile NAME`; the CLI does not change or
infer the GUI's active selection. `profiles list` reads the existing manifest,
never creates one. `--data-root PATH` selects an application data root. `--db PATH`
selects an existing SQLite file for isolated tests/explicit reads. SQL uses bound
parameters, SQLite read-only open and query-only mode. No application database
initializer is linked; missing databases fail instead of being created. Metadata
and assets are read in one snapshot, released before network waits. SQLite WAL
readers may use its shared-memory sidecar; the business database is not written.

Default Python is the root's `venv-numpy1/bin/python3` on macOS/Linux. `--python`
selects an existing Python with yfinance/pandas. The CLI does not install packages.
`doctor` reports executable/script presence offline; it does not verify imports.
`doctor --online` checks that runtime and connectivity by fetching a public index;
its `online_ok` must be checked even when the diagnostic command exits zero.

Proxy settings are read through the same `ProxySettings`/`ProxyManager` used by
the app: system/manual/direct, including bypasses. Inherited shell proxy variables
are replaced according to app policy. No proxy-setting command is provided.
Doctor reports mode and whether HTTP/HTTPS proxy values exist, never proxy
credentials. No official Fincept API is called. Yahoo reads can create ordinary
provider cache files in the existing Python environment; read-only refers to
portfolio/settings/trading state, not to all filesystem activity.

## Price file and valuation semantics

```json
{"currency":"USD","prices":{"AAPL":{"price":120,"change":5,"change_percent":4.35}}}
```

Currency must match the portfolio. Each price must be finite and positive;
optional change fields must be finite. Symbols are exact canonical holding
symbols. This price file is an explicit user assertion that prices share the
portfolio currency; no FX conversion or currency inference is performed.
Missing quotes fall back to holding cost, with `has_quote:false`,
`valuation_basis:"cost"`, `complete:false` and explicit coverage counts. This
shares the application's `value_portfolio` function. Day change fields are null
unless all holdings have prices and supplied day change fields.

`--live` uses Yahoo daily quote metadata to check currency; a missing/mismatched
currency or missing day change excludes that quote and adds a warning. Broker
streaming quotes are not used. Inspect `complete`, `priced_positions`, `warnings`
and `as_of`; `ok:true` alone does not mean valuation has complete prices.
Daily closing/latest daily chart price is not a streaming or executable price.
`as_of` is valuation computation time, not a promise that all source prices have
the same timestamp. The CLI does not save valuation history or NAV snapshots.

## JSON and exit codes

All data commands produce one envelope on stdout, pretty JSON by default and
compact under `--json`. Diagnostics stay on stderr. `--help`/`--version` return text.

```json
{"schema_version":1,"ok":true,"data":{"symbol":"AAPL","price":120}}
{"schema_version":1,"ok":true,"data":{"items":[],"next_offset":null,"offset":0,"limit":100}}
{"schema_version":1,"ok":false,"error":{"code":"provider_failed","message":"Yahoo could not complete the request; check ticker, connectivity and rate limits"}}
```

Provider data is nested under `data`; portfolio/history/doctor results are CLI
objects as described above. Empty lists succeed. Provider failures, bad input
and missing setup produce machine-readable errors and nonzero exits. Provider
stderr/error text is deliberately not copied because it can contain private
URLs or credentials. Exit codes: 1 unexpected internal error, 2 argument/input
error, 3 missing setup/file, 4 provider/JSON failure, 5 database/not-found, 6 total
time/output budget. Maximum provider output and input JSON file size: 8 MiB.

For agent analysis: diagnose, discover IDs, read exact objects, then compose
analysis from the JSON. This executable has no write surface. MCP can later wrap
these command contracts/shared functions without duplicating valuation logic.
