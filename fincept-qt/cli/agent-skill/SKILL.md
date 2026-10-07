---
name: fincept-local
description: Use the installed Fincept Local read-only CLI for Yahoo ticker discovery, prices, historical bars and local portfolio holdings or valuations. Use when the user asks to analyze data through their maintained Fincept version; does not submit trades or modify holdings.
---

Use `command -v fincept-local`, then `fincept-local --json doctor`. Read
`--help` for the installed contract. On this Mac, a restricted Codex shell can
report Qt `Incompatible processor ... neon`; use the approved host execution
path rather than reinstalling Qt or changing application code. If network reads fail, run `doctor --online`;
check `data.online_ok` independently of the process exit status. Reuse the
existing app Python environment and proxy policy; don't install dependencies or
change proxy settings just to make an analysis succeed.

Use `profiles list` to discover profile names, and pass `--profile NAME`
explicitly when needed. Default is `default`, not necessarily the open GUI's
profile. Discover portfolio IDs with `portfolio list`, or resolve an exact
name with `portfolio resolve --name NAME`; duplicate names are ambiguous.
Read only the selected portfolio. Avoid exposing unrelated holdings.

Examples:

```sh
fincept-local --json quotes get '^GSPC'
fincept-local --json history get AAPL --period 1y --interval 1d --limit 250
fincept-local --json portfolio value PORTFOLIO_ID --profile default --live
```

Use `symbols search QUERY --limit 5` to find ticker candidates before exact
reads. `symbols resolve` uses Yahoo heuristics, including `.NS`/`.BO`, and is
not an exhaustive global security resolver. `data read info SYMBOL` is the
allowlisted raw read hatch; no generic tool execution or write operation exists.

All data commands return `{schema_version,ok,data}` or `{schema_version,ok,error}`.
Require exit zero and `ok:true` before parsing data. `history get` returns the
latest N bars, ordered oldest first; inspect `truncated` and `available`.
Yahoo daily price is not a streaming or executable quote. Inspect timestamps.

Valuation uses the application's shared calculation. Inspect `complete`,
`priced_positions`, `total_positions`, `warnings`, `valuation_basis` and `as_of`.
Missing/incompatible quotes fall back to cost and must be disclosed in the
analysis; never interpret cost fallback as current market price. Live quotes
must match portfolio currency; there is no FX conversion. `--prices FILE`
accepts explicit same-currency marks for offline analysis. Day changes are null
unless available for all holdings. No valuation/snapshot is written to the app.

This CLI has no trading/authentication/write surface. Do not substitute direct
SQL writes or private broker API calls to implement actions outside its scope.
For schema, runtime overrides and rebuilding, see the repository's
`fincept-qt/cli/README.md` next to this skill's source.
