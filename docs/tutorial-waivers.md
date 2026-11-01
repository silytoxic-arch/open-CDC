# Tutorial: waiving a CDC finding and keeping waivers honest

This tutorial takes the `examples/cdc_advanced` design from a first analysis to a waiver that a CI job keeps checking. It uses the command line; the GUI does the same with **Waive...** (Rule Violations and Output Ports views) and the **Waivers** view. See [waivers.md](waivers.md) for the reference.

Work on a copy of the example, without its waiver file, so the example itself stays as it is:

```
cp -r examples/cdc_advanced /tmp/cdc_tutorial
cd /tmp/cdc_tutorial
rm waivers.json cdc_advanced_waived.ocdc.json
```

Every `console` block below is run by the test suite (`cli.waiver_tutorial`) on such a copy: the commands must give the exit codes and the lines shown (`...` stands for lines, or parts of a line, that are left out).

## 1. Analyze

```console
$ opencdc analyze cdc_advanced.ocdc.json
...
Rules   : 3 error violation(s), 0 warning violation(s), 4 path(s) at potential risk, 0 path(s) need review
...
  #7 [potential-risk] cdc_advanced.a_kick (clk_a) -> cdc_advanced.u_kick.meta (clk_b)  width=1 sync=two-flop/necessary-conditions-verified fingerprint=19389cf79353a884
      CDC-007 warning (potential-risk): a one-cycle pulse of 'cdc_advanced.a_kick' (clk_a) can be missed by the level synchronizer 'cdc_advanced.u_kick.meta' (clk_b) ...
...
(exit code 3)
```

Exit code 3 means error-severity violations that are not waived. Path #7 is a warning: a one-cycle pulse of `a_kick` (clk_a, 10 ns) can be missed in clk_b. Suppose you know that `a_kick` is a level that is held for many cycles. Then the finding does not apply, and you waive it.

Each path line that has rule hits ends with the path's **fingerprint**, a hash of the source, the destination and the pin. It does not depend on the order of the paths, so a waiver by fingerprint keeps covering the same path when other paths come and go.

## 2. Waive the finding

```console
$ opencdc waive cdc_advanced.ocdc.json --fingerprint 19389cf79353a884 --rule CDC-007 --reason "a_kick is a level held for at least 4 clk_a cycles (see the kick FSM)" --author jdoe --expires 2099-12-31
waiver 'W-1' added to ...waivers.json
waiver file added to project ...cdc_advanced.ocdc.json
```

The project had no waiver file, so `waivers.json` was created next to it and added to the project. The waiver selects rule CDC-007 on this path only. The id is the next free `W-<n>`.

```console
$ opencdc analyze cdc_advanced.ocdc.json
...
Rules   : 3 error violation(s), 0 warning violation(s), 3 path(s) at potential risk, 0 path(s) need review, 1 hit(s) waived
...
  #7 [safe] cdc_advanced.a_kick (clk_a) -> cdc_advanced.u_kick.meta (clk_b) ...
      CDC-007 warning (potential-risk): a one-cycle pulse of 'cdc_advanced.a_kick' ... [waived by W-1]
...
(exit code 3)
```

Path #7 is now safe, and one path fewer is at potential risk. The three error violations remain: they are real bugs in this example.

## 3. Audit the waivers

```console
$ opencdc waivers cdc_advanced.ocdc.json --fail-on unused,expired,invalid
...
Waivers : 1 from 1 file(s): 1 active, 0 expiring, 0 unused, 0 expired; 0 invalid entr(y/ies) or file problem(s)
  W-1 [active] uses=1 rule=CDC-007 fingerprint=19389cf79353a884 expires=2099-12-31  (...waivers.json:6:1)
      a_kick is a level held for at least 4 clk_a cycles (see the kick FSM) -- jdoe
Audit   : passed
```

A waiver can outlive what it was written for. For example, a signal is renamed, and its waiver then matches nothing. The waiver below selects a signal that no longer exists:

```console
$ opencdc waive cdc_advanced.ocdc.json --source cdc_advanced.a_kick_old --rule CDC-007 --reason "old name of a_kick"
waiver 'W-2' added to ...waivers.json
$ opencdc waivers cdc_advanced.ocdc.json --fail-on unused,expired,invalid
...warning[WAIVER-UNUSED]: waiver 'W-2' does not match any rule hit (stale or too narrow?)
...
  W-2 [unused] uses=0 rule=CDC-007 source=cdc_advanced.a_kick_old  (...waivers.json:...)
...
Audit   : FAILED (unused)
(exit code 4)
```

Exit code 4 fails a CI job. Remove the stale waiver (or fix its selector):

```console
$ opencdc waive cdc_advanced.ocdc.json --remove W-2
waiver 'W-2' removed from ...waivers.json
$ opencdc waivers cdc_advanced.ocdc.json --fail-on unused,expired,invalid
...
Audit   : passed
```

## 4. Expiry dates

A waiver with `expires` stops applying after that date (`WAIVER-EXPIRED`). The hit it covered then counts again. Before that, it is reported as **expiring** within the project's warning period: option `waiverExpiryWarningDays`, default 30 days. `--fail-on expiring` turns that into a CI failure, so the review happens before the date passes.

## 5. Reports for review

```console
$ opencdc analyze cdc_advanced.ocdc.json -q --html report/report.html --csv report
...
HTML    : report/report.html
CSV     : report (paths.csv, violations.csv, waivers.csv, outputs.csv)
(exit code 3)
```

The HTML report counts the waivers per state and shows the reason with every waived hit. `waivers.csv` lists every waiver with its state and use count, for a spreadsheet review.

## 6. In CI

A job that fails on new unwaived errors and on waivers that need attention (GitHub Actions):

```yaml
- name: CDC analysis
  run: opencdc analyze design.ocdc.json --html cdc-report.html     # exit 3: unwaived error violations
- name: Waiver audit
  run: opencdc waivers design.ocdc.json --fail-on unused,expired,invalid,expiring
```
