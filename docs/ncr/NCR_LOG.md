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
| [NCR-008](#ncr-008) | 2026-10-04 | OBC misses a node reboot in the node's first second | Minor | Closed |
| [NCR-009](#ncr-009) | 2026-10-05 | B-dot detumble stalls above the hand-over rate in the loop with 42 | Major | Closed |
| [NCR-010](#ncr-010) | 2026-10-05 | 42 applies corrupted wheel torque commands | Critical | Closed |
| [NCR-011](#ncr-011) | 2026-10-05 | Bridge answers "bus busy" to back-to-back transactions; converged event repeats | Minor | Closed |
| [NCR-012](#ncr-012) | 2026-10-05 | 42 stopped advancing 14 s into one COSMOS end-to-end run | Major | Open |

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

---

## NCR-008

**OBC misses a node reboot in the node's first second**

| | |
|---|---|
| Found by | SIL CAN node system test (`tests/system/test_can_nodes.py`), commanded node reset check |
| Item | OBC flight software `firmware/obc/obc_can.c`, heartbeat monitor |
| Severity | Minor: the reset itself worked and was acknowledged, but the operator got no "rebooted" event, so an unexpected reset of a node could go unreported |
| Status | Closed 2026-10-04 |

**Observation.** `OBC_NODE_RESET ADCS` was acknowledged and the ADCS node restarted with reset cause COMMAND (node log), but the OBC raised no `node rebooted` event. The previous check in the test had power-cycled the ADCS node, so it had been running for under a second when the reset came.

**Root cause.** The OBC detected a reboot only when a heartbeat's uptime (whole seconds) was lower than the previous one. Both heartbeats around the reset said uptime 0: the last one before the reset was sent at power-on, and a node sends its first heartbeat straight after start-up. The heartbeat (8 bytes, a full CAN frame) has no room for a boot counter.

**Fix.** A node sends heartbeats at 1 s intervals, so while it keeps running every heartbeat reports a higher uptime than the one before. The OBC now also reports a reboot when the uptime is unchanged and more than 0.5 s has passed since the previous heartbeat. A CAN frame received twice (possible after an error in its last bit) arrives straight away and isn't mistaken for a reboot.

**Verification.** `test_can_nodes.py` 10/10: the commanded reset 1 s after a power-on reset is reported as `ADCS node rebooted (reset cause 2)`.

---

## NCR-009

**B-dot detumble stalls above the hand-over rate in the loop with 42**

| | |
|---|---|
| Found by | SIL ADCS system test (`tests/system/test_adcs.py`), first run in the loop with 42 |
| Item | ADCS design: B-dot gain and the `DETUMBLE` → `SUN_POINT` threshold (`firmware/obc/adcs/adcs_params.h`, [ADCS design note](../design/ADCS_DESIGN.md) §4.1) |
| Severity | Major: from a 5.4°/s tumble the spacecraft never reached sun pointing, so the automatic mode sequence didn't meet its requirement |
| Status | Closed 2026-10-05 |

**Observation.** The OBC entered `DETUMBLE` on its own 13 s after start and B-dot ran with saturated torquers. According to 42's output, the angular momentum fell from 2.9 to 1.2 mN m s in 50 s. The body rate then stayed at about 1.65°/s for over 7 minutes, with dipoles down to about 0.05 A m², and never reached the 1°/s needed to hand over to sun pointing.

**Investigation.**
- The magnetometer was checked against 42's truth with the spacecraft tumbling: within 0.8°. The sim log showed fresh samples every 0.2 s.
- In the body frame the field was changing by only about 0.1°/s while the body turned at 1.6°/s, so the spin lay almost exactly along the field line.
- A magnetic torque is always perpendicular to the field, so B-dot can't act on that spin. It decays only as the field direction turns. The closed-loop unit test had shown the same behaviour, but at a lower level.

**Root cause.** Two things combined.
1. The gain (200 A m² s, taken from NOS3's ADCS configuration) was about ten times too high. A high gain cancels the off-field rate so quickly that the spin is steered to follow the field line instead of being removed. A gain scan over three tumbles in the unit simulation found k = 20 consistently fast: below 1°/s within 90 s in every case, where k = 200 took between 40 and 710 s.
2. Even at the right gain, the real field near the magnetic equator turns slowly enough that B-dot can hold a spin above 1°/s for minutes. The hand-over rate assumed B-dot would finish the job.

**Fix.**
- B-dot gain 20 A m² s.
- The `DETUMBLE` → `SUN_POINT` hand-over moved to 2°/s (11 % of one wheel's capacity). The wheels absorb the residual spin and momentum management dumps it over the orbit.
- For hysteresis, the `SAFE` → `DETUMBLE` threshold moved to 2.5°/s (still below NOS3's 2.8°/s deployment tip-off) and `SUN_POINT` → `DETUMBLE` to 5°/s.
- The unit test now detumbles from three different tumbles, so a gain that suits only one of them fails.

**Verification.**
- `test_adcs` 28/28.
- In the loop with 42: `DETUMBLE` from 5.39°/s, then `SUN_POINT` automatically after 100 s at 1.62°/s (42 truth), then sun pointing converged.

---

## NCR-010

**42 applies corrupted wheel torque commands**

| | |
|---|---|
| Found by | SIL ADCS system test (`tests/system/test_adcs.py`) and its recorded time series; isolated with a dedicated wheel experiment |
| Item | Test environment: 42's IPC socket reader, `Source/AutoCode/TxRxIPC.c` `ReadFromSocket()` and its generator `MetaCode/JsonToTxRxIPC.jl` (NOS3's 42, branch `dev_20260403`) |
| Severity | Critical: 42 applied wheel torques the flight software never commanded, so every result involving the wheels was suspect |
| Status | Closed 2026-10-05 |

**Observation.** In the ADCS test, the spacecraft was pointing at the Sun with the body rate at 0.03°/s, and the IMU simulator was then disabled. Within a second the true body rate jumped to 1.77°/s. In the same moment all three simulated wheels lost 2.0·10⁻⁴ N m s, which is full torque (1 mN m) for exactly one 0.2 s control period. The OBC had commanded zero torque. The wheel simulator logs confirmed that nothing above 0.06 mN m was ever sent.

A dedicated experiment (TEST mode, one wheel held at 50 rad/s by the OBC's manual speed loop) showed a second symptom. The wheel stopped accelerating at 45.2 rad/s and stayed there, while the OBC kept commanding +82 µN m and the simulator kept forwarding it.

**Root cause.** 42's `ReadFromSocket()` reads one chunk into a 16 KB stack buffer that it never clears or terminates. It then parses newline-delimited lines until it meets a line `[ENDMSG]`, ignoring how many bytes were actually read.

NOS3's simulators never send `[ENDMSG]`, and the wheel simulator doesn't even end its command with a newline: `SC[0].Whl[0].Tcmd = 8.2e-05`. The parser therefore ran on into whatever an earlier, longer message had left in the buffer, and attached its trailing characters to the new number:
- `8.2e-05` followed by a stale `7` parsed as `8.2e-057`, so the wheel froze.
- `-0` followed by `62137e-07` (the tail of an earlier `1.62137e-07`) parsed as −6.2 mN m. 42 clamped that to −1 mN m on every wheel.

Which corruption occurs depends on the lengths of the previous message strings. Earlier results were affected only intermittently. That is why the pointing in the first ADCS runs settled with an unexplained offset and overshoot.

**Fix.** A patch to 42, [`nos3/scripts/cfg/patches/42-ipc-parse-bound.patch`](../../nos3/scripts/cfg/patches/42-ipc-parse-bound.patch), makes the parser stop at the number of bytes read, with the buffer terminated there. It is applied to both the generated reader and its generator, so a regeneration keeps it. NOS3's `prepare.sh` applies it after cloning 42. `sil/sil_env.sh` refuses to start with an unpatched 42.

**Verification.**
- Wheel experiment: wheel 0 reaches and holds exactly 50.0 rad/s (860 µN m s). Disabling the IMU and switching to `SAFE` (zero torque) leave the wheel momentum and the body rate unchanged.
- ADCS system test, 8/8. Sun pointing now settles to 0.03° with no overshoot; before the fix it held a 3–5° offset. After the IMU failure the body rate stays at 0.01°/s, where it previously jumped to 1.77°/s. See the [ADCS design note](../design/ADCS_DESIGN.md) §7.

**Note.** This defect is in NOS3's version of 42, so it affects any NOS3 user whose simulators command 42's wheels. It is a candidate to report upstream (nasa-itc/42).

---

## NCR-011

**Bridge answers "bus busy" to back-to-back transactions; the converged event repeats**

| | |
|---|---|
| Found by | Operator run of `sil/launch.sh` with the GUIs (42 3D view, COSMOS) on the development laptop |
| Item | HIL bridge `nos3/components/hil_bridge/sim/src/hil_bridge.c` (bus workers, NCR-006). OBC `firmware/obc/obc_adcs.c` and `obc_sensors.c`. |
| Severity | Minor: no fault was declared and attitude control worked, but about 2 % of IMU reads were lost, and the console filled with repeated `sun pointing converged` events and miss messages |
| Status | Closed 2026-10-05 |

**Observation.** In a 13-minute run the terminal showed `IMU read missed (error -1)` every few seconds. Each miss was followed by another `sun pointing converged` event. The bridge log had 72 `bus busy: frame type 0x40 bus 0 answered BUS_ERROR` (IMU, CAN) and 5 for the fine sun sensor (SPI). The automated tests, which run without GUIs, had shown none of this.

**Root cause.**
1. *Bridge.* An IMU read is three CAN transactions in a row. A bus worker sent its reply and only then marked itself idle. The OBC answers instantly over the pty, so its next request could arrive in between. On a loaded machine the worker thread is often descheduled at exactly that point, and the bridge then rejected a perfectly sequential request as "busy". The log shows request 21 arriving before the worker had finished with request 20, whose reply had already gone out.
2. *OBC.* On any missed read, sun pointing reset its pointing error and convergence. The next good read declared convergence again and raised the event again. B-dot similarly restarted its field derivative and zeroed the torquers for the cycle.

**Fix.**
1. A bus worker now builds its reply, marks itself idle, and then sends the reply.
2. A single missed read now keeps the last actuator commands and state, applying NCR-005's persistence principle to the control loop. B-dot's derivative spans the time since the last good sample. The converged flag has hysteresis: it clears only at twice the convergence limits, or when the Sun is lost.
3. Missed reads are logged as one summary line per minute instead of one line each.

**Verification.**
- The full regression campaign passes.
- A 200 s run with a heavier load than the GUIs (six busy-loop processes on the 8-thread laptop) gave 5 `bus busy` answers, against 72 in 13 minutes before the fix.
- The remaining missed reads, about 3 % at that load, were other late or failed replies. They appear as one summary line a minute, no fault was declared, and the spacecraft detumbled and handed over to sun pointing.

---

## NCR-012

**42 stopped advancing 14 s into one COSMOS end-to-end run**

| | |
|---|---|
| Found by | Regression campaign `tests/run_all.sh`, stage `e2e:cosmos`, 2026-10-05 14:06 |
| Item | Test environment: 42 (patched for NCR-010), in the SIL container next to COSMOS |
| Severity | Major: when it happens, every result after that point is invalid. It is intermittent and so far seen once. |
| Status | Open: not reproduced, monitoring in place |

**Observation.** The COSMOS check that all six sensors are valid failed: the IMU and coarse sun sensors weren't. COSMOS showed 42's truth body rate as ±22918°/s (400 rad/s) with a zero magnetic field.

In the logs:
- The truth simulator's sample time stopped at 14.5 s of simulated time, and from then on it published zeros.
- The IMU, magnetometer and wheel simulators logged `Parsing exception stof` on 42's data from 12:06:57 on, 19 s after start.
- The OBC had commanded no actuator before that, with automatic modes switched off at 3 s, and no simulator had sent 42 a command.

So 42 itself stopped advancing, or started sending corrupt output, with no input from the flight software. The same test passed in two earlier campaign runs that day and in three immediate reruns.

**Actions so far.** `sil/sil_env.sh` now has a 42 watchdog. If 42's simulation time, as logged several times a second by the truth simulator, stops changing for 6 s, it prints `WARNING: 42 has stopped advancing ... results from now on are invalid` and saves the process state and the last truth samples to `42-stall.txt` in the run's log directory. This keeps an environment failure from being mistaken for a flight software failure, and captures evidence the next time it happens.

The first version watched 42's `time.42` output file and raised false alarms in an operator session. 42 writes its output files in buffered chunks a couple of minutes apart, so between chunks the file looked frozen.

**Next.** On recurrence: check whether 42 is alive and where it is blocked (`42-stall.txt`, then `gdb -p` or `/proc/<pid>/stack`), and whether it coincides with COSMOS and the SIL starting together. One candidate is CPU starvation at start-up; another is a blocking write on one of 42's TX sockets.

