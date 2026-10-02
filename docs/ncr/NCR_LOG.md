# HITL FlatSat — Non-Conformance Report Log

A non-conformance report (NCR) records every case where the system, its test environment or its documentation didn't behave as specified: how it was found, the root cause, the fix, and how the fix was verified. Problems found in the environment (NOS3, 42) are logged too, because they affect the validity of test results.

| ID | Date | Title | Severity | Status |
|---|---|---|---|---|
| [NCR-001](#ncr-001) | 2026-10-02 | 42 overwrites external actuator commands every 0.2 s | Major | Closed |
| [NCR-002](#ncr-002) | 2026-10-02 | Enum values OFF/ON generated as False/True | Major | Closed |
| [NCR-003](#ncr-003) | 2026-10-02 | GPS port open request lost before the bridge attaches | Minor | Closed |
| [NCR-004](#ncr-004) | 2026-10-02 | Star tracker and wheel reads time out right after start-up | Minor | Closed |

Severity: **Critical** invalidates results or risks hardware; **Major** a function doesn't meet its requirement; **Minor** degraded or cosmetic.

---

## NCR-001

**42 overwrites external actuator commands every 0.2 s**

| | |
|---|---|
| Found by | SIL device test (`firmware/tests/sil/test_devices.c`), reaction wheel torque check |
| Item | Test environment: 42 configuration `nos3/cfg/InOut/SC_NOS3.txt` |
| Severity | Major: closed-loop attitude control would receive only a fraction of the commanded torque, so ADCS results would be wrong |
| Status | Closed 2026-10-02 |

**Observation.** A torque of 0.5 mNm commanded on wheel 0 for 2 s increased the wheel's momentum by 0.000095 Nms instead of the expected 0.001 Nms: about 10 % of the commanded impulse.

**Root cause.** NOS3 runs 42 with its internal flight software set to `PASSIVE_FSW` and an FSW sample time of 0.2 s. In `MapCmdsToActuators()` (42 `Source/42fsw.c`), every FSW sample copies the internal controller's actuator commands into the wheels (`S->Whl[].Tcmd`) and magnetorquers (`S->MTB[].Mcmd`). These are the same fields the NOS3 reaction-wheel and torquer simulators write when the flight computer sends a command. A passive controller commands zero, so any external command lasted only until the next 0.2 s tick. 0.5 mNm × ~0.19 s = 0.000095 Nms matches the observation. The same applies to NOS3's own cFS ADCS application.

**Fix.** FSW sample time set to 10⁶ s in `SC_NOS3.txt`. 42 initialises its sample counter to the maximum, so the passive commands are applied once at start-up (zero, as before) and never again. `PASSIVE_FSW` computes nothing, so no other behaviour changes. The 42 source and the NOS3 simulators are unchanged.

**Verification.** Same test after the fix: 0.5 mNm for 2 s gives +0.001005 Nms (expected +0.001000, error 0.5 %, consistent with command latency). All 14 device checks pass.

**Follow-up.** The torquer path has the same root cause; its physical effect will be verified by the Phase 2 detumble test.

---

## NCR-002

**Enum values OFF/ON generated as False/True**

| | |
|---|---|
| Found by | Compiler error building the OBC (`FLATSAT_ADCS_MODE_OFF` undeclared) |
| Item | Interface definitions `docs/icd/flatsat_icd.yaml`, generator `tools/icd_gen.py` |
| Severity | Major: COSMOS would have shown the ADCS mode, wheel control mode and switch states as "False"/"True", and C code couldn't name those values |
| Status | Closed 2026-10-02 |

**Root cause.** YAML 1.1, which PyYAML implements, reads unquoted `OFF`, `ON`, `YES` and `NO` as booleans (the "Norway problem"). The enums `adcs_mode`, `rw_ctrl_mode` and `switch_state` therefore had value names `False`/`True` in every generated file. The unit tests didn't catch it because they test round trips by value, not by name.

**Fix.** Keys quoted in the YAML. The generator now rejects any enum value name that isn't an `UPPER_CASE` identifier, with a message explaining the cause, and a unit test asserts the `OFF`/`ON` names exist.

**Verification.** The generator rejects a copy of the file with `OFF` unquoted; the regenerated files contain no `False`/`True`; all unit tests and the COSMOS cross-check pass.

---

## NCR-003

**GPS port open request lost before the bridge attaches**

| | |
|---|---|
| Found by | OBC system test (`tests/system/test_obc_umbilical.py`): sensor valid mask 0x1F, GPS bit never set |
| Item | Umbilical client `firmware/common/src/fs_umbilical.c` |
| Severity | Minor: GPS data never reached the OBC when it booted before the bridge |
| Status | Closed 2026-10-02 |

**Root cause.** The OBC opens the GPS port (`UART_OPEN`) at boot. If the bridge hasn't attached yet, the request waits in the serial line, and the bridge flushes the line when it opens it, so the request is lost. A bridge restart would lose every open port the same way.

**Fix.** The umbilical client re-sends `UART_OPEN` for every open port whenever the link comes up (first frame received, or first frame after a link loss).

**Verification.** New unit test `test_uart_reopen_on_link_up`; the system test shows all six sensors valid (mask 0x3F).

---

## NCR-004

**Star tracker and wheel reads time out right after start-up**

| | |
|---|---|
| Found by | Operator, first interactive launch (`sil/launch.sh` with graphics): `star tracker read failed (-2)` and `reaction wheels read failed (-2)` at 1.5 s, recovered within a second |
| Item | Umbilical client and OBC start-up; HIL bridge protocol |
| Severity | Minor: spurious fault events at every start that an operator could mistake for real failures; the first second of star tracker and wheel data is lost |
| Status | Closed 2026-10-02 |

**Root cause.** The bridge opens each NOS Engine bus on its first use. Timing added to the bridge log measured 51–59 ms per bus. A first UART request therefore spends about 55 ms on the open before the request even reaches the simulator, against the driver's 100 ms first-byte timeout. With 42's graphics also loading the machine, the star tracker and wheel replies arrived after the deadline. The bridge log of the failing run confirms the replies did arrive. Later cycles are unaffected because the buses stay open. Headless runs didn't reproduce it, so the margin was small but not zero.

**Fix.**
- New frames `I2C_OPEN`, `SPI_OPEN` and `CAN_OPEN` (0x22/0x32/0x42), alongside the existing `UART_OPEN`. The OBC declares all ten of its buses at start-up (`dev_open_all()`), and the umbilical client sends the opens whenever the link comes up or comes back.
- The OBC waits 2 s after link-up before its first sensor read. Ten opens take about 0.55 s back to back, so this leaves a wide margin.

**Verification.**
- Unit test `test_uart_reopen_on_link_up` extended to all bus types and the link-up timer.
- Three headless launches: all ten buses opened immediately after link-up, the first sensor transaction about 550 log lines later, zero sensor fault events.
- Full regression passes: devices 14/14, OBC system test 12/12, COSMOS end to end 7/7, bridge end to end.
