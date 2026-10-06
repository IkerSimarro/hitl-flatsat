# HITL FlatSat — Test Plan

| | |
|---|---|
| Document | HITL-FLATSAT-TPL |
| Version | 1.0 |
| Requirements | [REQUIREMENTS.md](../requirements/REQUIREMENTS.md) (SYS-01 to SYS-08) |
| Procedures | [TEST_PROCEDURES.md](TEST_PROCEDURES.md) (TC-01 to TC-15) |
| Defects | [NCR log](../ncr/NCR_LOG.md) |

## 1. Purpose and scope

This plan describes how the FlatSat is shown to meet its system requirements: what is tested, at which level, in which environment, how pass and fail are decided, and how defects are handled.

The items under test are:
- the flight software of the three FlatSat nodes: the flight computer (OBC), the ADCS node and the EPS node;
- the ground station software.

The HIL bridge, the NOS3 simulators, 42, COSMOS and the plant model form the test environment. Their own checks (TC-03, and steps marked `ENV`) qualify that environment rather than verify a requirement.

## 2. Test levels

| Level | What runs | Environment | Purpose |
|---|---|---|---|
| Unit | Flight libraries, control laws, ground code | Fake HAL; closed-loop plant and rigid-body models | Logic, protocol and algorithm correctness; control performance by analysis |
| Software-in-the-loop (SIL) | The real flight software of all three nodes and the real ground station | Linux processes in Docker. NOS3 device simulators and 42 provide sensors and dynamics, virtual CAN (`vcan0`) the bus, a plant model the FlatSat's motor and battery, COSMOS the ground. | System behaviour against the requirements: the campaign this plan covers now |
| Hardware-in-the-loop (HIL) | The same flight software on Raspberry Pi Pico 2 boards | Real CAN bus, reaction wheel, power board and LoRa radios; NOS3 and 42 still simulate the orbit and the spacecraft's sensors | Phase 7: the same test cases again on hardware, plus measurements that only hardware can give |

The flight software is identical at the SIL and HIL levels; only the hardware abstraction layer changes. That is why a SIL result carries over to HIL, and why a difference between them points at the hardware or its interfaces.

## 3. Approach

**Requirements-based.** Every requirement is verified by at least one test case step. Every step names the requirements it verifies ([TEST_PROCEDURES.md](TEST_PROCEDURES.md)). The test report's traceability matrix shows the result behind each requirement.

**Independent truth.** Attitude, rates, field, Sun direction and position are checked against 42's own truth stream, not against what the flight software reports about itself. The FlatSat's power measurements are checked against the plant model's true values in the same way.

**Fault injection.** Faults are injected from outside the flight software, the way they would occur:

| Fault | Method |
|---|---|
| Sensor or actuator stops answering | NOS3 command bus: `DISABLE` / `ENABLE` of the device simulator |
| Simulator hangs | `SIGSTOP` / `SIGCONT` of the simulator process |
| Motor power cut, node held in reset | EPS node load switches, commanded by the OBC |
| Node crash | The node's process is killed and restarted |
| CAN bus loss | The virtual CAN interface is taken down and brought back |
| Low battery | NOS3 EPS simulator: `STATE_OF_CHARGE=` |
| Corrupted or lost radio frames | Ground station: `GS_CORRUPT_NEXT`, `GS_SET_LOSS` |

**Automation.** Every test case is a script. `tests/run_all.sh` runs the whole campaign unattended. Each check prints its step ID, PASS or FAIL, and the values it measured. `tools/test_report.py` turns a campaign run into the test report.

**Repeatability.**
- Random elements are seeded: the ground station's link loss uses `--seed`.
- Initial conditions are explicit: `SIL_INIT_RATES`, the ground station position.
- Timing checks use simulation time where it matters. The simulation runs a few percent slower than the wall clock on the development laptop.

**Evidence.** Each campaign run keeps the logs of every stage, the SIL logs of every process, and recorded time series (`adcs_timeseries.csv`, `rf_pass.csv`). It also keeps a `summary.json` with the software versions under test (git commits of both repositories).

## 4. Pass and fail criteria

- A **step** passes when its measured values meet the criterion in the procedures.
- A **test case** passes when all its steps pass.
- A **requirement** is verified when every step that verifies it passes. It is *partially verified* while some of those steps are still planned.

Steps marked *informative* (the comparison with NASA's reference ADCS, TC-15) record measurements without a pass threshold.

## 5. Entry and exit criteria

**Entry**, for a campaign run:
- the firmware and NOS3 build;
- the generated interface files are up to date (`icd_gen.py --check`);
- 42 carries the NCR-010 fix (checked by `sil_env.sh`).

**Exit**, for the SIL campaign:
- all automated test cases pass;
- every failure seen during the campaign has an NCR with its root cause and disposition;
- no Critical or Major NCR is open without an agreed workaround;
- every SYS requirement is verified at the SIL level.

## 6. Defect management

Every deviation from a requirement, an interface or expected behaviour is logged in the [NCR log](../ncr/NCR_LOG.md), whether it lies in the flight software, the ground software or the test environment. Each entry records how it was found, the item, the severity, the observation, the root cause, the fix and the verification of the fix.

Severity:
- **Critical:** invalidates results or risks hardware;
- **Major:** a function doesn't meet its requirement;
- **Minor:** degraded or cosmetic.

A fix is closed only after the test that found it, and the regression campaign, pass again.

## 7. Schedule

| When | Campaign |
|---|---|
| October 2026 (Phase 3) | SIL campaign: all test cases, test report, traceability |
| October 2026 (Phase 4) | The first Pico 2 runs the OBC firmware against NOS3: the first HIL run |
| After the hardware build (Phase 7) | HIL campaign: the same test cases on the FlatSat, plus hardware measurements (wheel tracking, real power per mode, RF loss across passes, umbilical latency); SIL and HIL results compared |

## 8. Limitations of the SIL environment

- **Orbit.** 42 propagates NOS3's orbit as a pure two-body orbit, and its environmental torques are switched off, so the attitude results have no disturbance torques to fight. The HIL campaign doesn't change this either: the orbit and dynamics stay simulated.
- **Plant model.** The FlatSat's motor, battery and charger are first-order models. Their parameters come from datasheets, and the HIL campaign measures the real values.
- **RF link.** The link model is a free-space link budget with an SNR-based loss curve, with no fading or interference. The real radios sit on a desk at low power; the model gives the link budget the frames would have from orbit.
- **Timing.** Processes share one machine. Missed sensor reads under load are expected, and the design tolerates them (NCR-005); the campaign logs how many occur.
- **Environment defect.** NCR-012 (an intermittent 42 stall) is open. The SIL environment detects it and marks the results that follow as invalid.
