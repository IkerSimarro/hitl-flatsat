#!/usr/bin/env python3
"""Writes the test report of one campaign run (tests/run_all.sh).

    tools/test_report.py [RUN_DIR [RETEST_DIR ...]] [--out DIR]

RUN_DIR defaults to the latest tests/logs/run-*; the report goes to RUN_DIR/report unless --out is given (the
published report is docs/test/report). RETEST_DIRs are later runs of some of the stages (tests/run_all.sh <words>),
after a fix: their results replace the campaign's for those stages, and the report lists both. Reads the run's stage logs ("PASS [TC-nn.m] ..." lines), its
summary.json and recorded data (<stage>.d/*.csv), docs/test/verification.yaml and the NCR log. Needs matplotlib:
run it in the flatsat-report image (tools/report/Dockerfile), as tests/run_all.sh does.
"""
import argparse
import csv
import datetime
import json
import os
import pathlib
import re
import sys

import yaml

ROOT = pathlib.Path(__file__).resolve().parent.parent
STEP_LINE = re.compile(r"^\s*(PASS|FAIL) \[(TC-\d+\.\d+)\]\s+(.*?)\s*$")
NCR_ROW = re.compile(r"^\| \[(NCR-\d+)\]\(#ncr-\d+\) \| ([\d-]+) \| (.*) \| (\w+) \| (\w+) \|$")
ICON = {"PASS": "✅ PASS", "FAIL": "❌ FAIL", None: "⬜ not run"}


def latest_run():
    runs = sorted((ROOT / "tests/logs").glob("run-*"))
    if not runs:
        raise SystemExit("no campaign run in tests/logs")
    return runs[-1]


def read_results(run):
    """{step id: (PASS|FAIL, text)}: the last result of each step across the run's stage logs."""
    out = {}
    for log in sorted(run.glob("*.log")):
        for line in log.read_text(errors="replace").splitlines():
            m = STEP_LINE.match(line)
            if m:
                out[m.group(2)] = (m.group(1), re.sub(r"\s{2,}", " — ", m.group(3), count=1))
    return out


def read_ncrs():
    rows = []
    for line in (ROOT / "docs/ncr/NCR_LOG.md").read_text().splitlines():
        m = NCR_ROW.match(line)
        if m:
            rows.append(dict(id=m.group(1), date=m.group(2), title=m.group(3), severity=m.group(4), status=m.group(5)))
    return rows


def load_csv(path):
    if not path.exists():
        return []
    with open(path) as f:
        return list(csv.DictReader(f))


def num(x):
    try:
        return float(x)
    except (TypeError, ValueError):
        return None


# ---- Plots ----

def plots(data, out):
    """data(path): the recorded file, from the latest run that has it."""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    plt.rcParams.update({"figure.figsize": (9, 4.6), "figure.dpi": 110, "axes.grid": True, "grid.alpha": 0.3,
                         "axes.spines.top": False, "axes.spines.right": False, "font.size": 10})
    made = {}

    rows = load_csv(data("sil-adcs.d/adcs_timeseries.csv"))
    if rows:
        t = [num(r["t_s"]) for r in rows]
        fig, (a1, a2) = plt.subplots(2, 1, sharex=True, figsize=(9, 6))
        a1.plot(t, [num(r["rate_truth_dps"]) for r in rows], color="#1f77b4", label="body rate (42 truth)")
        a1.axhline(2.0, color="gray", ls="--", lw=1, label="SYS-01 limit 2 deg/s")
        a1.set_ylabel("deg/s")
        a1.legend(loc="upper right")
        a2.plot(t, [num(r["err_truth_deg"]) for r in rows], color="#d62728", label="Sun angle (42 truth)")
        a2.plot(t, [num(r["err_obc_deg"]) for r in rows], color="#ff9896", ls=":", label="OBC estimate")
        a2.axhline(5.0, color="gray", ls="--", lw=1, label="SYS-02 limit 5 deg")
        a2.set_ylabel("deg")
        a2.set_yscale("log")
        a2.set_xlabel("time from test start, s")
        a2.legend(loc="upper right")
        modes = {"1": ("DETUMBLE", "#fde0c5"), "2": ("SUN_POINT", "#d5f5e3")}
        for ax in (a1, a2):
            start = None
            for i, r in enumerate(rows + [{"mode": None, "t_s": rows[-1]["t_s"]}]):
                m = r["mode"]
                if start is not None and m != rows[start]["mode"]:
                    if rows[start]["mode"] in modes:
                        ax.axvspan(num(rows[start]["t_s"]), num(r["t_s"]), color=modes[rows[start]["mode"]][1],
                                   alpha=0.6, lw=0)
                    start = None
                if start is None and m is not None:
                    start = i
        fig.suptitle("TC-08: detumble and sun pointing in the loop with 42 (shading: DETUMBLE, SUN_POINT)")
        fig.tight_layout()
        fig.savefig(out / "tc08_attitude.png")
        plt.close(fig)
        made["TC-08"] = "tc08_attitude.png"

    rows = load_csv(data("sil-rf-pass.d/rf_pass.csv"))
    if rows:
        t = [num(r["t_s"]) for r in rows]
        fig, a1 = plt.subplots()
        a1.plot(t, [num(r["elevation_deg"]) for r in rows], color="#2ca02c", label="elevation")
        a1.axhline(10, color="gray", ls="--", lw=1, label="10 deg mask")
        a1.set_ylabel("elevation, deg")
        a1.set_xlabel("time from test start, s")
        a2 = a1.twinx()
        rssi = [(num(r["t_s"]), num(r["rssi_dbm"])) for r in rows if r["contact"] == "1" and num(r["rssi_dbm"]) and num(r["rssi_dbm"]) > -140]
        a2.plot([x for x, _ in rssi], [y for _, y in rssi], color="#9467bd", label="RSSI of the last frame")
        a2.set_ylabel("RSSI, dBm")
        a2.spines["right"].set_visible(True)
        h1, l1 = a1.get_legend_handles_labels()
        h2, l2 = a2.get_legend_handles_labels()
        a1.legend(h1 + h2, l1 + l2, loc="upper left")
        a1.set_title("TC-10: a pass over NASA Wallops (ground station view)")
        fig.tight_layout()
        fig.savefig(out / "tc10_pass.png")
        plt.close(fig)
        made["TC-10"] = "tc10_pass.png"

    rows = load_csv(data("sil-hardware-twin.d/wheel_steps.csv"))
    if rows:
        t = [num(r["t_s"]) for r in rows]
        fig, a1 = plt.subplots()
        a1.plot(t, [num(r["setpoint_rpm"]) for r in rows], color="gray", ls="--", label="set point")
        a1.plot(t, [num(r["true_rpm"]) for r in rows], color="#1f77b4", label="wheel speed (plant truth)")
        a1.set_ylabel("rpm")
        a1.set_xlabel("time, s")
        a1.legend(loc="upper right")
        a1.set_title("TC-13: physical reaction wheel speed steps (ADCS node, 100 Hz loop)")
        fig.tight_layout()
        fig.savefig(out / "tc13_wheel.png")
        plt.close(fig)
        made["TC-13"] = "tc13_wheel.png"

    rows = load_csv(data("sil-orbit.d/orbit.csv"))
    if rows:
        t = [num(r["t_s"]) / 60 for r in rows]
        fig, (a1, a2) = plt.subplots(2, 1, sharex=True, figsize=(9, 6))
        a1.plot(t, [num(r["err_truth_deg"]) for r in rows], color="#d62728", label="Sun angle (42 truth)")
        a1.set_ylabel("deg")
        a1.set_yscale("log")
        a1.legend(loc="upper right")
        a2.plot(t, [num(r["wheel_momentum_pct"]) for r in rows], color="#8c564b", label="wheel momentum, % of capacity")
        a2.set_ylabel("%")
        a2.set_xlabel("time, min")
        a2.legend(loc="upper right")
        for ax in (a1, a2):
            ecl = [r["eclipse_truth"] == "1" for r in rows]
            for i in range(1, len(rows)):
                if ecl[i]:
                    ax.axvspan(t[i - 1], t[i], color="#cccccc", alpha=0.5, lw=0)
        fig.suptitle("TC-14: one orbit without intervention (grey: eclipse, from 42's geometry)")
        fig.tight_layout()
        fig.savefig(out / "tc14_orbit.png")
        plt.close(fig)
        made["TC-14"] = "tc14_orbit.png"
    return made


# ---- Report ----

def verdict(step_ids, results, planned):
    states = [results.get(s, (None,))[0] for s in step_ids]
    if any(s == "FAIL" for s in states):
        return "❌ Failed"
    if all(s == "PASS" for s in states):
        return "✅ Verified"
    if any(s == "PASS" for s in states):
        return "🟨 Partially verified"
    return "⬜ Not yet verified"


def summary_of(run):
    return json.loads((run / "summary.json").read_text()) if (run / "summary.json").exists() else {}


def software(sm):
    return (f"hitl-flatsat `{sm.get('commit', '?')}`{' (with local changes)' if sm.get('dirty') else ''}, "
            f"NOS3 fork `{sm.get('nos3_commit', '?')}`")


def report(runs, out):
    run, retests = runs[0], runs[1:]
    v = yaml.safe_load((ROOT / "docs/test/verification.yaml").read_text())
    summary = summary_of(run)
    results = {}
    for r in runs:  # a retest replaces the results of the stages it ran
        results.update(read_results(r))
    ncrs = read_ncrs()

    def data(path):
        return next((r / path for r in reversed(runs) if (r / path).exists()), run / path)
    figs = plots(data, out)
    planned = {tc["id"] for tc in v["test_cases"] if tc.get("status") == "planned"}
    all_steps = [st for tc in v["test_cases"] for st in tc["steps"]]
    n_pass = sum(1 for st in all_steps if results.get(st["id"], (None,))[0] == "PASS")
    n_fail = sum(1 for st in all_steps if results.get(st["id"], (None,))[0] == "FAIL")
    n_run = n_pass + n_fail

    def doc(path):
        """A repository document, linked relative to where the report is written."""
        return os.path.relpath(ROOT / path, out)

    o = ["# HITL FlatSat — Test Report", "",
         "| | |", "|---|---|", "| Document | HITL-FLATSAT-TR |",
         f"| Campaign | `{run.name}` (finished {summary.get('finished', '?')}) |",
         f"| Software under test | {software(summary)} |"]
    for r in retests:
        sm = summary_of(r)
        stages = ", ".join(f"`{st['name']}`" for st in sm.get("stages", []))
        o.append(f"| Retest | `{r.name}` (finished {sm.get('finished', '?')}): {stages}, on {software(sm)} |")
    o += [f"| Environment | Software-in-the-loop on {summary.get('host', '?')}; NOS3 image `ivvitc/nos3-64:20260619`, "
         f"COSMOS 4.5.0 |",
         f"| Plan and procedures | [TEST_PLAN.md]({doc('docs/test/TEST_PLAN.md')}), "
         f"[TEST_PROCEDURES.md]({doc('docs/test/TEST_PROCEDURES.md')}) |",
         f"| Generated | {datetime.date.today().isoformat()} by `tools/test_report.py` |", "",
         "## 1. Summary", "",
         f"**{n_pass} of {len(all_steps)} procedure steps passed**, {n_fail} failed, "
         f"{len(all_steps) - n_run} not run.", ""]

    o += ["| Requirement | Verdict | Steps passed |", "|---|---|---|"]
    verdict_steps = [st for tc in v["test_cases"] if not tc.get("informative") for st in tc["steps"]]
    for r in v["requirements"]:
        ids = [st["id"] for st in verdict_steps if r["id"] in st["reqs"]]
        ok = sum(1 for s in ids if results.get(s, (None,))[0] == "PASS")
        o.append(f"| [{r['id']}]({doc('docs/requirements/REQUIREMENTS.md')}#{r['id'].lower()}) {r['title']} | "
                 f"{verdict(ids, results, planned)} | {ok} / {len(ids)} |")

    retested = {st["name"]: (r.name, st) for r in retests for st in summary_of(r).get("stages", [])}
    o += ["", "| Stage | Result | Duration | Retest |", "|---|---|---|---|"]
    for st in summary.get("stages", []):
        again = retested.get(st["name"])
        o.append(f"| `{st['name']}` | {ICON.get(st['result'], st['result'])} | {st['seconds'] // 60} min "
                 f"{st['seconds'] % 60} s | " + (f"{ICON.get(again[1]['result'])} in `{again[0]}`" if again else "")
                 + " |")

    o += ["", "## 2. Traceability", "", "Each requirement, the steps that verify it, and their results.", "",
          "| Requirement | Step | Check | Result |", "|---|---|---|---|"]
    for r in v["requirements"]:
        for tc in v["test_cases"]:
            for st in tc["steps"]:
                if r["id"] in st["reqs"]:
                    res = results.get(st["id"], (None, ""))[0]
                    note = " *(informative)*" if tc.get("informative") else ""
                    o.append(f"| {r['id']} | [{st['id']}](#{tc['id'].lower()}) | {st['title']}{note} | {ICON[res]} |")

    o += ["", "## 3. Results by test case"]
    for tc in v["test_cases"]:
        status = f" *({tc['status']})*" if tc.get("status") else ""
        o += ["", f"### {tc['id']}", "", f"**{tc['title']}**{status}. {tc['objective']}", ""]
        if tc["id"] in figs:
            o += [f"![{tc['id']}]({figs[tc['id']]})", ""]
        o += ["| Step | Criterion | Result | Measured |", "|---|---|---|---|"]
        for st in tc["steps"]:
            res, text = results.get(st["id"], (None, ""))
            o.append(f"| {st['id']} {st['title']} | {st['criterion']} | {ICON[res]} | {text.replace('|', '/')} |")

    open_ncrs = [n for n in ncrs if n["status"] != "Closed"]
    o += ["", "## 4. Defects", "",
          f"{len(ncrs)} non-conformance reports were raised up to this campaign: {len(ncrs) - len(open_ncrs)} closed, "
          f"{len(open_ncrs)} open. The full records are in the [NCR log]({doc('docs/ncr/NCR_LOG.md')}).", "",
          "| NCR | Date | Title | Severity | Status |", "|---|---|---|---|---|"]
    for n in ncrs:
        o.append(f"| [{n['id']}]({doc('docs/ncr/NCR_LOG.md')}#{n['id'].lower()}) | {n['date']} | {n['title']} | "
                 f"{n['severity']} | {n['status']} |")

    o += ["", "## 5. Notes", "",
          "- Steps that print measured values show them in the *Measured* column, exactly as the test printed them.",
          "- The environment's limitations (two-body orbit without disturbance torques, first-order plant models, "
          f"a free-space link model) are listed in the [test plan]({doc('docs/test/TEST_PLAN.md')}"
          "#8-limitations-of-the-sil-environment).",
          "- The hardware-in-the-loop campaign (Phase 7) will run the same procedures on the FlatSat hardware."]
    (out / "TEST_REPORT.md").write_text("\n".join(o) + "\n")
    return n_pass, n_fail, len(all_steps)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="*")
    ap.add_argument("--out")
    a = ap.parse_args()
    runs = [pathlib.Path(r).resolve() for r in a.runs] or [latest_run()]
    out = pathlib.Path(a.out).resolve() if a.out else runs[0] / "report"
    out.mkdir(parents=True, exist_ok=True)
    n_pass, n_fail, total = report(runs, out)
    print(f"test report: {out / 'TEST_REPORT.md'} ({n_pass} passed, {n_fail} failed, {total} steps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
