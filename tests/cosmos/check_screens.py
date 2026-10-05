#!/usr/bin/env python3
"""Checks the FlatSat COSMOS screens against the generated packet definitions: every telemetry item a widget
shows, and every command (with its parameters) a button sends, must exist. COSMOS would only report a wrong name
when an operator opens the screen."""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
GSW = ROOT / "nos3/components/hil_bridge/gsw"
ITEM_WIDGETS = re.compile(r"^\s*(LABELVALUE\w*|FORMATVALUE|LABELFORMATVALUE|VALUE\w*|LIMITSBAR|LINEGRAPH|"
                          r"LABELTRENDLIMITSBAR|TRENDBAR|RANGEBAR|PROGRESSBAR|LABELPROGRESSBAR)\s+(\S+)\s+(\S+)\s+(\S+)")
BUTTON_CMD = re.compile(r'cmd\("(\S+) (\S+)(?: with ([^"]*))?"\)')


def load_defs():
    tlm, cmd = {}, {}
    for f in GSW.glob("*/cmd_tlm/*.txt"):
        kind = item = None
        for line in f.read_text().splitlines():
            words = line.split()
            if not words:
                continue
            if words[0] in ("TELEMETRY", "COMMAND"):
                kind = tlm if words[0] == "TELEMETRY" else cmd
                item = kind.setdefault((words[1], words[2]), set())
            elif words[0] in ("APPEND_ITEM", "APPEND_ID_ITEM", "APPEND_PARAMETER", "APPEND_ID_PARAMETER", "ITEM"):
                item.add(words[1])
    return tlm, cmd


def main():
    tlm, cmd = load_defs()
    errors, widgets, buttons = [], 0, 0
    for screen in sorted(GSW.glob("*/screens/*.txt")):
        for n, line in enumerate(screen.read_text().splitlines(), 1):
            where = f"{screen.relative_to(ROOT)}:{n}"
            m = ITEM_WIDGETS.match(line)
            if m:
                widgets += 1
                target, packet, item = m.group(2), m.group(3), m.group(4)
                if item not in tlm.get((target, packet), ()):
                    errors.append(f"{where}: no telemetry item {target} {packet} {item}")
            for c in BUTTON_CMD.finditer(line):
                buttons += 1
                key = (c.group(1), c.group(2))
                if key not in cmd:
                    errors.append(f"{where}: no command {' '.join(key)}")
                    continue
                for param in filter(None, (p.strip().split(" ")[0] for p in (c.group(3) or "").split(","))):
                    if param not in cmd[key]:
                        errors.append(f"{where}: {' '.join(key)} has no parameter {param}")
    for e in errors:
        print(e)
    print(f"checked {widgets} telemetry widgets and {buttons} command buttons: "
          f"{'all match the definitions' if not errors else f'{len(errors)} errors'}")
    return 1 if errors or widgets == 0 else 0


if __name__ == "__main__":
    sys.exit(main())
