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
| `timingException` | at least one selector | Glob on the timing exception of the constraints that covers the crossing (a `set_false_path` or `set_max_delay -datapath_only`, written as `set_false_path (<file>:<line>)`; shown in the GUI, the reports and the JSON as `timingException`). `"*"` matches any crossing the designer excluded from timing, `"*sys_top.sdc:34*"` the one constraint. A crossing no exception covers never matches; output ports are not selected this way. CLI: `opencdc waive --timing-exception`. |
| `author` | no | Who reviewed the hit. |
| `expires` | no | `YYYY-MM-DD`. From the day after this date, the waiver is no longer applied. |

In globs, `*` matches any sequence of characters (including `.`), and `?` matches exactly one character. All other characters, including `[` and `]`, are literal, so `top.g[0].*` works as written.

A step-by-step walk through waiving a finding and auditing waivers in CI is in [tutorial-waivers.md](tutorial-waivers.md).

## Adding a waiver

A waiver for one rule hit (or one output crossing) can be written for you. It selects the rule and the path's fingerprint, so it covers that path only:

- **GUI**: select the hit in *Rule Violations* (or the output in *Output Ports*) and press **Waive...**. Enter a reason (required), and optionally an author and an expiry date. Choose the waiver file: one of the project's, or `waivers.json` next to the project file. A new file is added to the project and the project is saved. The analysis then runs again. The project must have been saved to a file first.
- **CLI**: `opencdc waive <project> --fingerprint <fp> --rule <rule> --reason "<text>" [--author …] [--expires YYYY-MM-DD] [--id …] [--file …]`. The fingerprint is printed at the end of every path line that has rule hits (`fingerprint=…`) and in every `CDC-OUTPUT-CLOCK` warning. Without `--file` the waiver goes to the project's first waiver file, or to a new `waivers.json` next to the project, which is then added to the project. Without `--id` the next free `W-<n>` is used.

Existing waivers are listed in the GUI's **Waivers** view: id, state (*active*, *unused* — it matched nothing, *expiring* — within the warning period, *expired*), number of uses, rule, selectors, reason, author, expiry date and where it is defined. A filter shows only one state. **Edit...** changes the id, reason, author or expiry date (the selectors stay); **Remove** deletes the waiver from its file. Double-click opens the waiver in its file. From the command line, `opencdc waive <project> --remove <id>` removes a waiver from whichever of the project's waiver files has it.

In both cases the waiver is checked before anything is written: the id must not already be used in any of the project's waiver files, a reason is required, and the expiry date must be YYYY-MM-DD. An existing waiver file keeps its entries, their field order and any other fields. The file is replaced only once the new text has been written completely.

## Auditing waivers

`opencdc waivers <project>` runs the analysis and lists every waiver with its state (*active*, *expiring*, *unused*, *expired*), its use count, selectors, expiry date, location and reason, after the waiver diagnostics (unreadable files, invalid entries, expiry warnings). In CI, `--fail-on` makes the command fail with exit code 4 when a waiver is in one of the given states:

```
opencdc waivers my.ocdc.json --fail-on unused,expired,invalid   # stale or broken waivers fail the build
opencdc waivers my.ocdc.json --fail-on expiring --json waivers.json
```

`invalid` counts waiver file errors: an unreadable file, invalid JSON, or an entry that is skipped (`WAIVER-INVALID`). `--json` writes the list (with `state`, `uses`, `expiresInDays`, `file`, `line`). The analysis result's JSON also has `cdc.waivers[].state`.

## Matching

- A waiver covers a rule hit when every selector it specifies matches. Selectors it leaves out match anything.
- For each hit, the waivers are tried in file order, and files are tried in project order. The first matching waiver that has not expired covers the hit.
- The path's status is recomputed from the hits that are not waived. A path whose hits are all waived is marked "waived" and becomes safe.
- Waivers are applied after the rule checks, in the "Waiver application" stage. The stage's detail line reports how many hits were waived.
- Output ports driven asynchronously to their `set_output_delay` clock (`CDC-OUTPUT-CLOCK`) can be waived as well: use `"rule": "CDC-OUTPUT-CLOCK"` (or a glob that matches it). `source` is matched against the driving registers, `destination` against the output ports (one match among them is enough), and `fingerprint` against the fingerprint printed in the warning. A waived output crossing is not reported; the JSON result and the HTML report list it with the waiver's id.

## Diagnostics

| Code | Severity | When |
|---|---|---|
| `WAIVER-OPEN` | error | The waiver file cannot be read. |
| `WAIVER-SCHEMA` | error / warning | Invalid JSON or wrong `format`/`version` (error). An unknown field (warning: it is ignored). |
| `WAIVER-INVALID` | error | An entry has a missing or duplicate `id`, no `reason`, no selector, or a malformed `expires`. The entry is skipped. The diagnostic points to the entry's line. |
| `WAIVER-EXPIRED` | warning | The waiver has expired and is not applied. |
| `WAIVER-EXPIRING` | warning | The waiver is still applied but expires within the project's warning period (option `waiverExpiryWarningDays`, default 30 days, 0 = never; GUI: project settings, "Warn before waivers expire"). Review it, renew its date or fix the crossing. |
| `WAIVER-UNUSED` | warning | The waiver matched no hit. It may be stale, for example after a design change, or too narrow. |

## Where waived hits appear

- **CLI**: each waived hit shows "waived by <id>". The summary counts waived hits separately.
- **JSON**:
  - `ruleHits[].waiver` and `paths[].waived` on each path;
  - `cdc.waivers[]`, with `uses`, `expired`, `expiresInDays` and `state` for each waiver;
  - `summary.waivedHits`.
- **HTML report**: a "Waivers" section that counts the waivers per state and marks unused, expiring and expired ones (the summary has a "waivers to review" card), plus "Waived by …" with the reason on each hit.
- **CSV**: a `waived` column in `paths.csv`, a `waiver` column in `violations.csv` and `outputs.csv`, and `waivers.csv` with every waiver, its state (active, expiring, unused, expired), uses and location.
- **GUI**: the violation table has a Waiver column and a "Waived / Not waived" filter. The rule pane shows the waiver's reason, author and expiry date. Output crossings (`CDC-OUTPUT-CLOCK`) are in the Output Ports view, with the same filter and the waiver in the details pane. The Waivers view lists every waiver with its state and use count.

A `timingException` waiver turns the designer's own declaration into a reviewed waiver. Use it with a rule (`"rule": "CDC-00[13]"`) or a particular constraint line, and only after checking that the values really are static while the destination uses them: `set_false_path` only removes the path from timing analysis.

Prefer fingerprint or exact source/destination selectors over broad globs. With narrow selectors, a design change that creates a new crossing is not waived silently. `WAIVER-UNUSED` tells you when a waiver no longer matches anything.
