# HITL FlatSat — Non-Conformance Report Log

A non-conformance report (NCR) records every case where the system, its test environment or its documentation didn't behave as specified: how it was found, the root cause, the fix, and how the fix was verified. Problems found in the environment (NOS3, 42) are logged too, because they affect the validity of test results.

| ID | Date | Title | Severity | Status |
|---|---|---|---|---|
| [NCR-001](#ncr-001) | 2026-10-02 | 42 overwrites external actuator commands every 0.2 s | Major | Closed |

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
