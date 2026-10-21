# Waivers

A waiver records that a rule hit was reviewed and accepted. Typical cases are a quasi-static configuration register, or a reconvergence that a protocol makes safe. A waived hit stays in every output and is marked with the waiver that covers it. It no longer counts as a violation, and it no longer affects the path's status or the CLI exit code.

## Waiver file

The project file lists waiver files. Paths are relative to the project file:

```json
{
  "format": "opencdc-project",
  "version": 1,
  "name": "cdc_advanced_waived",
  "top": "cdc_advanced",
  "sources": ["..."],
  "waivers": ["waivers.json"]
}
```

A waiver file looks like this:

```json
{
  "format": "opencdc-waivers",
  "version": 1,
  "waivers": [
    {
      "id": "W-CFG",
      "rule": "CDC-008",
      "source": "cdc_advanced.a_cfg",
      "reason": "a_cfg is written only while clk_b logic is held in reset",
      "author": "example",
      "expires": "2099-12-31"
    }
  ]
}
```

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | Unique name. It is shown on every hit the waiver covers. |
| `reason` | yes | Why the hit is acceptable. A waiver without a reason is rejected. |
| `rule` | at least one selector | Rule id; glob allowed (`CDC-00?`, `CDC-*`). |
| `fingerprint` | at least one selector | Exact path fingerprint, as shown in the GUI details, the JSON/CSV/HTML reports and the CLI. It stays stable across runs as long as the source register, destination register and pin stay the same. |
| `source` | at least one selector | Glob on the source register's hierarchical name. |
| `destination` | at least one selector | Glob on the destination register's hierarchical name. |
| `author` | no | Who reviewed the hit. |
| `expires` | no | `YYYY-MM-DD`. From the day after this date, the waiver is no longer applied. |

In globs, `*` matches any sequence of characters (including `.`), and `?` matches exactly one character. All other characters, including `[` and `]`, are literal, so `top.g[0].*` works as written.

## Matching

- A waiver covers a rule hit when every selector it specifies matches. Selectors it leaves out match anything.
- For each hit, the waivers are tried in file order, and files are tried in project order. The first matching waiver that has not expired covers the hit.
- The path's status is recomputed from the hits that are not waived. A path whose hits are all waived is marked "waived" and becomes safe.
- Waivers are applied after the rule checks, in the "Waiver application" stage. The stage's detail line reports how many hits were waived.

## Diagnostics

| Code | Severity | When |
|---|---|---|
| `WAIVER-OPEN` | error | The waiver file cannot be read. |
| `WAIVER-SCHEMA` | error / warning | Invalid JSON or wrong `format`/`version` (error). An unknown field (warning: it is ignored). |
| `WAIVER-INVALID` | error | An entry has a missing or duplicate `id`, no `reason`, no selector, or a malformed `expires`. The entry is skipped. The diagnostic points to the entry's line. |
| `WAIVER-EXPIRED` | warning | The waiver has expired and is not applied. |
| `WAIVER-UNUSED` | warning | The waiver matched no hit. It may be stale, for example after a design change, or too narrow. |

## Where waived hits appear

- **CLI**: each waived hit shows "waived by <id>". The summary counts waived hits separately.
- **JSON**:
  - `ruleHits[].waiver` and `paths[].waived` on each path;
  - `cdc.waivers[]`, with `uses` and `expired` for each waiver;
  - `summary.waivedHits`.
- **HTML report**: a "Waivers" section, plus "Waived by …" with the reason on each hit.
- **CSV**: a `waived` column in `paths.csv`, and a `waiver` column in `violations.csv`.
- **GUI**: the violation table has a Waiver column and a "Waived / Not waived" filter. The rule pane shows the waiver's reason, author and expiry date.

Prefer fingerprint or exact source/destination selectors over broad globs. With narrow selectors, a design change that creates a new crossing is not waived silently. `WAIVER-UNUSED` tells you when a waiver no longer matches anything.
