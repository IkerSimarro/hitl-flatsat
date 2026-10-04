# HITL FlatSat — Non-Conformance Report Log

A non-conformance report (NCR) records every case where the system, its test environment or its documentation didn't behave as specified: how it was found, the root cause, the fix, and how the fix was verified. Problems found in the environment (NOS3, 42) are logged too, because they affect the validity of test results.

| ID | Date | Title | Severity | Status |
|---|---|---|---|---|
| [NCR-001](#ncr-001) | 2026-10-02 | 42 overwrites external actuator commands every 0.2 s | Major | Closed |
| [NCR-002](#ncr-002) | 2026-10-02 | Enum values OFF/ON generated as False/True | Major | Closed |
| [NCR-003](#ncr-003) | 2026-10-02 | GPS port open request lost before the bridge attaches | Minor | Closed |
| [NCR-004](#ncr-004) | 2026-10-02 | Star tracker and wheel reads time out right after start-up | Minor | Closed |
| [NCR-005](#ncr-005) | 2026-10-02 | Isolated late replies raise sensor fault events | Minor | Closed |
| [NCR-006](#ncr-006) | 2026-10-02 | One stalled simulator blocks the bridge for every device | Major | Closed |
| [NCR-007](#ncr-007) | 2026-10-02 | COSMOS Launcher crashes after the legal agreement | Major | Closed |

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

---

## NCR-005

**Isolated late replies raise sensor fault events**

| | |
|---|---|
| Found by | Operator, interactive launches with graphics: 18 fault/recovery event pairs in about 65 s, mostly the IMU, sometimes every device at once |
| Item | OBC sensor acquisition `firmware/obc/obc_sensors.c` |
| Severity | Minor: the data handling was correct (failed reads were marked invalid), but the event stream was flooded with false alarms |
| Status | Closed 2026-10-02 |

**Root cause.** Two parts:
- **Load.** The software-in-the-loop environment keeps about five of the laptop's eight CPU threads busy by design (measured: 42 at 100 % even without graphics, the NOS Engine server at about 70 %, the simulators). With 42's software-rendered graphics (about 165 %) and normal desktop use on top, requests occasionally reach a simulator late. One IMU request arrived 1.215 s after the previous one instead of 1 s, so its reply missed the OBC's 100 ms deadline.
- **Fault logic.** The OBC declared a device failed on its first missed read, so every such hiccup became a fault event and, a second later, a recovery event.

**Fix.**
- A reusable persistence filter (`firmware/common/src/fs_persist.c`), standard FDIR practice: a device is declared failed only after 3 consecutive missed reads (3 s at the 1 Hz acquisition rate), and recovered on the first good read after that. Each failed read still marks that device's data invalid immediately.
- Fault state is kept across umbilical outages, so faults declared before an outage still get their recovery event.
- New telemetry `OBC_HK.SENSOR_MISSES` (in place of a spare field) counts every failed read, so transient misses stay measurable for the test report instead of disappearing.
- Environment: 42 now runs at lower priority with two render threads (`sil/sil_env.sh`), so its graphics bursts don't take every core. This is harmless, but the measurement couldn't show an effect: two interactive runs, before and after the change, both had zero faults.

**Verification.**
- Unit test `test_persistence`: isolated misses never trip; three in a row trip exactly once; the first good read clears.
- Fault injection `tests/system/test_fault_persistence.sh`, which freezes the IMU simulator (3/3 pass):
  - normal operation: 0 misses;
  - 1.5 s freeze: 13 misses counted, no fault events;
  - 6 s freeze: one fault event and one recovery event per device.
- Full regression passes: unit, COSMOS cross-check, devices 14/14, OBC system test 12/12, COSMOS end to end 7/7.

---

## NCR-006

**One stalled simulator blocks the bridge for every device**

| | |
|---|---|
| Found by | Fault injection `tests/system/test_fault_persistence.sh`: freezing only the IMU simulator made every device miss, and a 6 s freeze dropped the umbilical link |
| Item | HIL bridge `nos3/components/hil_bridge/sim/src/hil_bridge.c` |
| Severity | Major: one unresponsive device takes every other device down with it, so the OBC can't isolate the faulty one; the same coupling makes load spikes affect all devices at once |
| Status | Closed 2026-10-04 |

**Root cause.** The bridge is single-threaded, and NOS Engine bus calls block until the simulator answers. Every synchronous-operation timeout in the NOS Engine C API defaults to infinite (measured). Setting `NE_set_default_timeout()` for send and receive to 80 ms had no effect on CAN transactions, and that change was reverted.

**Fix.** One worker thread per I2C, SPI and CAN bus in the bridge. The main thread keeps the serial link, UDP, UART polling and simulation time, and hands each transaction to its bus's worker without waiting. If that worker is still stuck on an earlier transaction, the new request is answered `BUS_ERROR` at once. Serial output and bus opening are protected by mutexes. At shutdown, buses with a stuck worker are left open rather than closed, because closing could block. This matters for the hardware phase too: the real OBC reads the simulated sensors through the same bridge.

**Verification** (`tests/system/test_fault_persistence.sh`, 5/5):
- **Unresponsive IMU simulator** (process frozen for 6 s):
  - the IMU is answered "bus busy" (`-1`) and declared failed, then recovered;
  - every other bus device keeps working and the umbilical never drops.
  - Before the fix, every device faulted and the link dropped. GPS also faults, because freezing a simulator process stalls 42 itself while the simulator stops reading 42's socket, so GPS fixes really do stop. That is a property of this injection method, not of the flight software.
- **Disabled IMU** (NOS3 command bus `DISABLE`, which leaves the simulator connected to 42), the clean single-device failure:
  - a 1.5 s outage costs one reading and raises no fault;
  - a 6 s outage gives one IMU fault, one recovery, no other device affected and no link drop.
- **Full regression passes:** bridge end to end, devices 14/14, OBC system test 12/12, COSMOS end to end 7/7.

**New test capability.** The SIL environment now runs NOS3's command bus bridge, and `sil/sim_cmd.py` sends commands to any simulator. This is the fault-injection mechanism for the Phase 3 test campaign.

---

## NCR-007

**COSMOS Launcher crashes after the legal agreement**

| | |
|---|---|
| Found by | Operator: after clicking Ok in the Legal Agreement window, the NOS3 Launcher never appeared |
| Item | Launcher `sil/launch.sh` |
| Severity | Major: the ground station GUI was unusable from the one-command launch |
| Status | Closed 2026-10-02 |

**Root cause.** COSMOS's crash report (`nos3/gsw/cosmos/outputs/logs/*_exception.txt`) showed `EPERM: Operation not permitted` in `Process.setpgrp`, called when the Launcher's main window starts (`cosmos/gui/qt_tool.rb:52`). Linux refuses `setpgrp()` for a session leader. `sil/launch.sh` ran `ruby Launcher` directly as the container's command, making it PID 1 and a session leader. The legal agreement dialog comes before that call, so it appeared normally. The earlier GUI checks only confirmed that window, so they missed this. The headless server (`CmdTlmServer --no-gui`) doesn't call `setpgrp`, so the automated tests passed.

**Fix.** The COSMOS container starts with Docker's `--init`: a minimal init process is PID 1, and COSMOS runs as an ordinary child process.

**Verification.**
- `Process.setpgrp` in the COSMOS image fails with EPERM without `--init` and succeeds with it.
- The operator then accepted the agreement and used the NOS3 Launcher, Command Sender (`FLATSAT OBC_NOOP`) and Packet Viewer (`FLATSAT OBC_HK`) against the running simulation.
