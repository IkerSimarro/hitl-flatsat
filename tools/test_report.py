#!/usr/bin/env python3
"""Writes the test report of one campaign run (tests/run_all.sh).

    tools/test_report.py [RUN_DIR [LATER_DIR ...]] [--hil HIL_RUN_DIR [LATER_DIR ...]] [--out DIR]

RUN_DIR defaults to the latest tests/logs/run-*; the report goes to RUN_DIR/report unless --out is given (the
published report is docs/test/report). LATER_DIRs are later runs of some of the stages (tests/run_all.sh <words>): a
retest after a fix, or a long stage run on its own. Their results replace the campaign's for those stages, and the
report lists both. Reads the run's stage logs ("PASS [TC-nn.m] ..." lines), its
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
LEVELS = ["sil", "hil"]  # a test case's default configurations (verification.yaml "levels")

# The OBC's timing budget, logged once a minute (obc_main.c task_timing)
TIMING_TASKS = re.compile(r"task timing, longest run in ms: (.*); CPU load (\d+) %")
TIMING_DEV = re.compile(r"device round trips: (\d+) bus transactions, mean ([\d.]+) ms, longest ([\d.]+) ms; (\d+) UART "
                        r"exchanges, first byte mean [\d.]+ ms, longest [\d.]+ ms, whole exchange mean ([\d.]+) ms, "
                        r"longest ([\d.]+) ms")
TIMING_ECHO = re.compile(r"umbilical echo: (\d+) heartbeats, round trip mean ([\d.]+) ms, longest ([\d.]+) ms")
TIMING_CMD = re.compile(r"commands: (\d+) run, queue wait mean ([\d.]+) ms, longest ([\d.]+) ms")
TIMING_RES = re.compile(r"resources: processor used ([\d.]+) %; stack deepest (-?\d+) of (-?\d+) bytes reserved")


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


def read_timing(run):
    """The OBC's once-a-minute timing reports in every stage of a run (<stage>.d/obc.log)."""
    t = {"tasks": {}, "cpu": [], "bus": [], "uart": [], "echo": [], "cmd": [], "used": [], "stack": None}
    for log in sorted(run.glob("*.d/obc.log")):
        for line in log.read_text(errors="replace").splitlines():
            m = TIMING_TASKS.search(line)
            if m and int(m.group(2)) > 0:  # the report at start-up comes before any task has run
                for item in m.group(1).split(", "):
                    name, ms = item.rsplit(" ", 1)
                    t["tasks"][name] = max(t["tasks"].get(name, 0), int(ms))
                t["cpu"].append(int(m.group(2)))
            m = TIMING_RES.search(line)
            if m:
                t["used"].append(float(m.group(1)))
                if int(m.group(3)) > 0:
                    t["stack"] = (max(int(m.group(2)), (t["stack"] or (0, 0))[0]), int(m.group(3)))
            for key, rx, groups in (("bus", TIMING_DEV, (1, 2, 3)), ("uart", TIMING_DEV, (4, 5, 6)),
                                    ("echo", TIMING_ECHO, (1, 2, 3)), ("cmd", TIMING_CMD, (1, 2, 3))):
                m = rx.search(line)
                if m and int(m.group(groups[0])) > 0:
                    t[key].append((int(m.group(groups[0])), float(m.group(groups[1])), float(m.group(groups[2]))))
    return t


def timing_summary(samples):
    """[(count, mean, longest)] -> 'mean X ms, longest Y ms' over all of them."""
    n = sum(c for c, _, _ in samples)
    if not n:
        return "–"
    return f"{sum(c * m for c, m, _ in samples) / n:.1f} / {max(x for _, _, x in samples):.1f} ms"


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


def positive(x):
    """For a logarithmic axis: zero and missing values are left out of the line."""
    v = num(x)
    return v if v is not None and v > 0 else float("nan")


def orbit_plot(rows, path, title):
    """TC-14's time series: Sun angle and wheel momentum over the orbit, eclipse shaded. False if there is none."""
    if not rows:
        return False
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt

    t = [num(r["t_s"]) / 60 for r in rows]
    fig, (a1, a2) = plt.subplots(2, 1, sharex=True, figsize=(9, 6))
    a1.plot(t, [positive(r["err_truth_deg"]) for r in rows], color="#d62728", label="Sun angle (42 truth)")
    a1.set_ylabel("deg")
    a1.set_yscale("log")
    a1.legend(loc="upper right")
    a2.plot(t, [num(r["wheel_momentum_pct"]) for r in rows], color="#8c564b", label="wheel momentum, % of capacity")
    a2.set_ylabel("%")
    a2.set_xlabel("time from convergence, min (wall clock: the simulation runs about 10 % slower)")
    a2.legend(loc="upper right")
    for ax in (a1, a2):
        ecl = [r["eclipse_truth"] == "1" for r in rows]
        for i in range(1, len(rows)):
            if ecl[i]:
                ax.axvspan(t[i - 1], t[i], color="#cccccc", alpha=0.5, lw=0)
    fig.suptitle(title)
    fig.tight_layout()
    fig.savefig(path)
    plt.close(fig)
    return True


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
        a2.plot(t, [positive(r["err_truth_deg"]) for r in rows], color="#d62728", label="Sun angle (42 truth)")
        a2.plot(t, [positive(r["err_obc_deg"]) for r in rows], color="#ff9896", ls=":", label="OBC estimate")
        a2.axhline(5.0, color="gray", ls="--", lw=1, label="SYS-02 limit 5 deg")
        a2.set_ylabel("deg")
        a2.set_yscale("log")
        a2.set_xlabel("time from test start, s")
        a2.legend(loc="upper right")
        modes = {"0": ("SAFE", "#e8e8e8"), "1": ("DETUMBLE", "#fde0c5"), "2": ("SUN_POINT", "#d5f5e3"),
                 "3": ("LOW_POWER", "#fff3b0")}
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
        fig.suptitle("TC-08: detumble and sun pointing in the loop with 42\nmode: SAFE grey, DETUMBLE orange, "
                     "SUN_POINT green, LOW_POWER yellow (the last two: injected faults)", fontsize=10)
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

    if orbit_plot(load_csv(data("sil-orbit.d/orbit.csv")), out / "tc14_orbit.png",
                  "TC-14: one orbit without intervention (grey: eclipse, from 42's geometry)"):
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


CONFIGS = {
    "sil": "Software-in-the-loop: the OBC and the CAN nodes as Linux processes",
    "hil-pico": "Hardware-in-the-loop: the OBC on a Raspberry Pi Pico 2 (umbilical on USB); the CAN nodes as Linux "
                "processes, their bus carried over the umbilical",
}


def software(sm):
    return (f"hitl-flatsat `{sm.get('commit', '?')}`{' (with local changes)' if sm.get('dirty') else ''}, "
            f"NOS3 fork `{sm.get('nos3_commit', '?')}`")


def hil_section(v, sil_results, hil_runs, doc, out):
    """The campaign with the OBC on the Pico (and its retests): verdicts, stages, every step next to its SIL result."""
    hil = hil_runs[0]
    sm = summary_of(hil)
    res = {}
    for r in hil_runs:
        res.update(read_results(r))
    retested = {st["name"]: r.name for r in hil_runs[1:] for st in summary_of(r).get("stages", [])}
    steps = [st for tc in v["test_cases"] if "hil" in tc.get("levels", LEVELS) for st in tc["steps"]]
    n_pass = sum(1 for st in steps if res.get(st["id"], (None,))[0] == "PASS")
    n_fail = sum(1 for st in steps if res.get(st["id"], (None,))[0] == "FAIL")
    o = ["", "## 4. Hardware in the loop: the OBC on the Pico 2", "",
         f"The same procedures with the OBC firmware on a Raspberry Pi Pico 2, its umbilical on USB "
         f"(`tests/run_all.sh --pico`, run `{hil.name}`, {software(sm)}). The CAN nodes still run as Linux processes; "
         f"the bridge carries their bus to the Pico over the umbilical. Steps that test the Linux build only "
         f"(TC-04, the driver test) don't run. **{n_pass} steps passed, {n_fail} failed.**", "",
         "| Requirement | SIL | HIL (Pico) |", "|---|---|---|"]
    def applicable(level):
        return [st for tc in v["test_cases"] if not tc.get("informative") and level in tc.get("levels", LEVELS)
                for st in tc["steps"]]
    for r in v["requirements"]:
        a = [st["id"] for st in applicable("sil") if r["id"] in st["reqs"]]
        b = [st["id"] for st in applicable("hil") if r["id"] in st["reqs"]]
        n_a = sum(1 for i in a if sil_results.get(i, (None,))[0] == "PASS")
        n_b = sum(1 for i in b if res.get(i, (None,))[0] == "PASS")
        o.append(f"| {r['id']} {r['title']} | {verdict(a, sil_results, set())} ({n_a} of {len(a)}) | "
                 f"{verdict(b, res, set())} ({n_b} of {len(b)}) |")
    o += ["", "| Stage | Result | Duration |", "|---|---|---|"]
    for st in sm.get("stages", []):
        again = f" (retest in `{retested[st['name']]}`: {ICON[read_stage_result(hil_runs, st['name'])]})" \
            if st["name"] in retested else ""
        o.append(f"| `{st['name']}` | {ICON.get(st['result'], st['result'])}{again} | {st['seconds'] // 60} min "
                 f"{st['seconds'] % 60} s |")
    orbit = next((r / "sil-orbit.d/orbit.csv" for r in reversed(hil_runs) if (r / "sil-orbit.d/orbit.csv").exists()), None)
    if orbit is not None and orbit_plot(load_csv(orbit), out / "tc14_orbit_pico.png",
                                        "TC-14 on the Pico: one orbit, the OBC on its target (grey: eclipse)"):
        o += ["", "![TC-14 on the Pico](tc14_orbit_pico.png)"]
    o += ["", "Every step that ran in both, side by side:", "",
          "| Step | SIL | HIL (Pico) | Measured on the PC | Measured on the Pico |", "|---|---|---|---|---|"]
    for st in steps:
        if st["id"] in res:
            a, b = sil_results.get(st["id"], (None, "")), res[st["id"]]
            o.append(f"| {st['id']} {st['title']} | {ICON[a[0]]} | {ICON[b[0]]} | {measured(a[1])} | {measured(b[1])} |")
    return o


def measured(text):
    """A step's printed result without the check's name in front (the table already says which step it is)."""
    return text.split(" — ", 1)[-1].replace("|", "/")


def read_stage_result(runs, name):
    """The stage's result in the latest of the runs that ran it."""
    for r in reversed(runs):
        for st in summary_of(r).get("stages", []):
            if st["name"] == name:
                return st["result"]
    return None


def timing_section(runs, number, doc):
    """The OBC's timing budget from its once-a-minute reports, per configuration."""
    runs = [(name, t) for name, t in runs if t["cpu"]]
    if not runs:
        return []
    head = " | ".join(name for name, _ in runs)
    o = ["", f"## {number}. Timing budget", "",
         "From the OBC's own once-a-minute timing reports, over every stage of the campaign. Busy time includes "
         "waiting for device replies: the scheduler is cooperative, so a task that waits holds the others "
         f"([NCR-018]({doc('docs/ncr/NCR_LOG.md')}#ncr-018)). Times are mean / longest. The longest runs include the "
         "fault-injection tests, which freeze a simulator, take the CAN bus down, or freeze the OBC process (TC-13.4 on "
         "the PC, for 3 s) or its bridge (on the Pico): they are worst cases under faults, not in normal operation.", "",
         f"| | {head} |", "|---|" + "---|" * len(runs)]

    def row(label, f):
        o.append(f"| {label} | " + " | ".join(f(t) for _, t in runs) + " |")
    row("Busy time per second (incl. waiting for replies), mean / most",
        lambda t: f"{sum(t['cpu']) / len(t['cpu']):.0f} / {max(t['cpu'])} %")
    row("Processor time used (not sleeping), mean / most",
        lambda t: f"{sum(t['used']) / len(t['used']):.1f} / {max(t['used']):.1f} %" if t["used"] else "–")
    row("Stack, deepest / reserved",
        lambda t: f"{t['stack'][0]} / {t['stack'][1]} bytes" if t["stack"] else "– (the operating system's)")
    row("Command queue wait", lambda t: timing_summary(t["cmd"]))
    row("Device bus transaction (I2C, SPI, CAN)", lambda t: timing_summary(t["bus"]))
    row("Device UART exchange (incl. the quiet time ending a reply)", lambda t: timing_summary(t["uart"]))
    row("Umbilical echo (no device)", lambda t: timing_summary(t["echo"]))
    tasks = sorted({k for _, t in runs for k in t["tasks"]}, key=lambda k: -max(t["tasks"].get(k, 0) for _, t in runs))
    for k in tasks:
        row(f"Task `{k}`, longest run", lambda t, k=k: f"{t['tasks'][k]} ms" if k in t["tasks"] else "–")
    return o


def report(runs, out, hil=None):
    run, retests = runs[0], runs[1:]
    v = yaml.safe_load((ROOT / "docs/test/verification.yaml").read_text())
    summary = summary_of(run)
    results = {}
    for r in runs:  # a later run (a retest, or a long stage run on its own) replaces the stages it ran
        results.update(read_results(r))
    ncrs = read_ncrs()

    def data(path):
        return next((r / path for r in reversed(runs) if (r / path).exists()), run / path)
    figs = plots(data, out)
    planned = {tc["id"] for tc in v["test_cases"] if tc.get("status") == "planned"}
    all_steps = [st for tc in v["test_cases"] if "sil" in tc.get("levels", LEVELS) for st in tc["steps"]]
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
        o.append(f"| Later run | `{r.name}` (finished {sm.get('finished', '?')}): {stages}, on {software(sm)} |")
    o += [f"| Configuration | {CONFIGS.get(summary.get('config', 'sil'), summary.get('config'))} |",
          f"| Environment | {summary.get('host', '?')}; NOS3 image `ivvitc/nos3-64:20260619`, "
         f"COSMOS 4.5.0 |",
         f"| Plan and procedures | [TEST_PLAN.md]({doc('docs/test/TEST_PLAN.md')}), "
         f"[TEST_PROCEDURES.md]({doc('docs/test/TEST_PROCEDURES.md')}) |",
         f"| Generated | {datetime.date.today().isoformat()} by `tools/test_report.py` |", "",
         "## 1. Summary", "",
         f"**{n_pass} of {len(all_steps)} procedure steps passed**, {n_fail} failed, "
         f"{len(all_steps) - n_run} not run."]
    idle = [tc for tc in v["test_cases"] if "sil" in tc.get("levels", LEVELS) and
            not any(st["id"] in results for st in tc["steps"])]
    if idle:
        o[-1] += " Not run: " + "; ".join(
            f"{tc['id']} ({len(tc['steps'])} steps{', ' + tc['status'] if tc.get('status') else ''}"
            f"{', informative' if tc.get('informative') else ''})" for tc in idle) + "."
    o.append("")

    o += ["| Requirement | Verdict | Steps passed |", "|---|---|---|"]
    verdict_steps = [st for tc in v["test_cases"] if not tc.get("informative") and "sil" in tc.get("levels", LEVELS)
                     for st in tc["steps"]]
    for r in v["requirements"]:
        ids = [st["id"] for st in verdict_steps if r["id"] in st["reqs"]]
        ok = sum(1 for s in ids if results.get(s, (None,))[0] == "PASS")
        o.append(f"| [{r['id']}]({doc('docs/requirements/REQUIREMENTS.md')}#{r['id'].lower()}) {r['title']} | "
                 f"{verdict(ids, results, planned)} | {ok} / {len(ids)} |")

    def duration(st):
        return f"{st['seconds'] // 60} min {st['seconds'] % 60} s"
    later = {st["name"]: (r.name, st) for r in retests for st in summary_of(r).get("stages", [])}
    extra = " Later run |" if later else ""
    o += ["", "| Stage | Result | Duration |" + extra, "|---|---|---|" + ("---|" if later else "")]
    for st in summary.get("stages", []):
        again = later.pop(st["name"], None)
        o.append(f"| `{st['name']}` | {ICON.get(st['result'], st['result'])} | {duration(st)} |"
                 + ((f" {ICON.get(again[1]['result'])} in `{again[0]}` |" if again else " |") if extra else ""))
    for name, (run_name, st) in later.items():  # stages only in a later run
        o.append(f"| `{name}` | – | – | {ICON.get(st['result'])} in `{run_name}` ({duration(st)}) |")

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

    if hil:
        o += hil_section(v, results, hil, doc, out)
    o += timing_section([("PC (SIL)", read_timing(r)) for r in runs[:1]] +
                        ([("Pico 2 (HIL)", read_timing(hil[0]))] if hil else []),
                        "5" if hil else "4", doc)

    open_ncrs = [n for n in ncrs if n["status"] != "Closed"]
    n_def = "6" if hil else "5"
    o += ["", f"## {n_def}. Defects", "",
          f"{len(ncrs)} non-conformance reports were raised up to this campaign: {len(ncrs) - len(open_ncrs)} closed, "
          f"{len(open_ncrs)} open. The full records are in the [NCR log]({doc('docs/ncr/NCR_LOG.md')}).", "",
          "| NCR | Date | Title | Severity | Status |", "|---|---|---|---|---|"]
    for n in ncrs:
        o.append(f"| [{n['id']}]({doc('docs/ncr/NCR_LOG.md')}#{n['id'].lower()}) | {n['date']} | {n['title']} | "
                 f"{n['severity']} | {n['status']} |")

    o += ["", f"## {int(n_def) + 1}. Notes", "",
          "- Steps that print measured values show them in the *Measured* column, exactly as the test printed them.",
          "- The environment's limitations (two-body orbit without disturbance torques, first-order plant models, "
          f"a free-space link model) are listed in the [test plan]({doc('docs/test/TEST_PLAN.md')}"
          "#8-limitations-of-the-sil-environment).",
          "- The hardware-in-the-loop campaign continues when the CAN, power and radio boards are fitted: the same "
          "procedures on the FlatSat hardware."]
    (out / "TEST_REPORT.md").write_text("\n".join(o) + "\n")
    return n_pass, n_fail, len(all_steps)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="*")
    ap.add_argument("--out")
    ap.add_argument("--hil", nargs="+", help="a campaign run with the OBC on the Pico (tests/run_all.sh --pico), "
                    "then any later runs of some of its stages")
    a = ap.parse_args()
    runs = [pathlib.Path(r).resolve() for r in a.runs] or [latest_run()]
    out = pathlib.Path(a.out).resolve() if a.out else runs[0] / "report"
    out.mkdir(parents=True, exist_ok=True)
    n_pass, n_fail, total = report(runs, out, [pathlib.Path(h).resolve() for h in a.hil] if a.hil else None)
    print(f"test report: {out / 'TEST_REPORT.md'} ({n_pass} passed, {n_fail} failed, {total} steps)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
