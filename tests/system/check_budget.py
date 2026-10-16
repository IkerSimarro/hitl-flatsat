#!/usr/bin/env python3
"""Test case TC-18: the OBC's resource budget on the Pico 2 (requirement SYS-09), from a campaign's own records.

Flash and RAM come from the firmware's section sizes, recorded when tests/run_all.sh --pico built it
(pico-size.txt). The stack and processor time come from the OBC's once-a-minute resource reports in every stage's
log (<stage>.d/obc.log): the stack's deepest use since each restart (the memory it can reach is filled with a
pattern at start-up), and the processor time not spent sleeping.

    tests/system/check_budget.py RUN_DIR
"""
import pathlib
import re
import sys

FLASH = 4 * 1024 * 1024   # Pico 2: 4 MB QSPI flash
RAM = 520 * 1024          # RP2350: 512 KB main SRAM + two 4 KB scratch banks
RESOURCES = re.compile(r"resources: processor used ([\d.]+) %; stack deepest (-?\d+) of (-?\d+) bytes reserved")


def check(ok, step, name, detail):
    print(f"{'PASS' if ok else 'FAIL'} [{step}] {name:34s}  {detail}", flush=True)
    return ok


def main():
    run = pathlib.Path(sys.argv[1])
    sizes = (run / "pico-size.txt").read_text().split("\n")[1].split()
    text, data, bss = (int(x) for x in sizes[:3])
    cpu, stack, reserved = [], 0, 0
    for log in sorted(run.glob("*.d/obc.log")):
        for m in RESOURCES.finditer(log.read_text(errors="replace")):
            cpu.append(float(m.group(1)))
            stack, reserved = max(stack, int(m.group(2))), int(m.group(3))
    ok = [
        check(text + data <= FLASH / 2, "TC-18.1", "flash",
              f"{(text + data) / 1024:.0f} KB of {FLASH // 1024} KB ({100 * (text + data) / FLASH:.1f} %), limit 50 %"),
        check(data + bss <= RAM / 2, "TC-18.2", "RAM, static",
              f"{(data + bss) / 1024:.1f} KB of {RAM // 1024} KB ({100 * (data + bss) / RAM:.1f} %), limit 50 %"),
        check(reserved > 0 and stack <= 0.75 * reserved, "TC-18.3", "stack, deepest over the campaign",
              f"{stack} of {reserved} bytes reserved ({100 * stack / reserved:.0f} %), limit 75 %" if reserved else
              "no resource report in any stage"),
        check(cpu and sum(cpu) / len(cpu) <= 50 and max(cpu) <= 80, "TC-18.4", "processor time used",
              f"mean {sum(cpu) / len(cpu):.1f} %, most in a minute {max(cpu):.1f} % over {len(cpu)} minutes, "
              f"limits 50 % and 80 %" if cpu else "no resource report in any stage"),
    ]
    print(f"---- {sum(ok)} passed, {len(ok) - sum(ok)} failed ----")
    return 0 if all(ok) else 1


if __name__ == "__main__":
    sys.exit(main())
