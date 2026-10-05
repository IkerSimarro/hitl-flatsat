# HITL FlatSat — ADCS Design Note

| | |
|---|---|
| Document | HITL-FLATSAT-DN-ADCS |
| Version | 1.0 |
| Date | 2026-10-04 |
| Implements | ICD §5.1 (modes); the thresholds the ICD defers to this note |
| Code | [`adcs_law.c`](../../firmware/obc/adcs/adcs_law.c) (control laws), [`adcs_params.h`](../../firmware/obc/adcs/adcs_params.h) (every number below), [`obc_adcs.c`](../../firmware/obc/obc_adcs.c) (modes, sensors, actuators) |

## 1. Scope

The flight computer controls the attitude of the simulated spacecraft (42, NOS3's `SC_NOS3.txt`) through NOS3's simulated sensors and actuators:
- the B-dot detumbling law;
- sun pointing with the reaction wheels;
- wheel momentum management;
- the automatic mode transitions;
- the fault responses.

The physical reaction wheel on the ADCS node mirrors simulated wheel 0 (DD-05). It isn't part of the control loop.

## 2. Spacecraft and equipment

| Item | Value | Source |
|---|---|---|
| Principal inertia | 0.0067, 0.033, 0.033 kg m² | `SC_NOS3.txt` |
| Magnetorquers | 3, body axes, 1.42 A m² each | `SC_NOS3.txt`; duty ±1 = ±1.42 A m² |
| Reaction wheels | 3, body axes, 1 mN m and 10.82 mN m s each, rotor 1.72·10⁻⁵ kg m² | `SC_NOS3.txt` |
| Gyro (IMU) | 3 axes, body frame | read at 5 Hz |
| Magnetometer | 3 axes, 2 nT quantisation | read at 5 Hz |
| Coarse sun sensors | 6, on ±X, ±Y, ±Z, cosine response | read at 5 Hz |
| Wheel momentum | from each wheel | read at 5 Hz |

The fine sun sensor and star tracker are read at 1 Hz for telemetry, but no control law uses them yet.

## 3. Attitude determination

| Quantity | Method |
|---|---|
| Body rate | Gyro, unfiltered |
| Magnetic field | Magnetometer, body frame |
| Sun vector | Coarse sun sensors. Opposite pairs give one component each: s = (c₊ₓ − c₋ₓ, c₊ᵧ − c₋ᵧ, c₊𝓏 − c₋𝓏), then normalised. The Sun is valid when the magnitude of s is at least 0.1. Below that the spacecraft is in eclipse, or is seeing only Earth albedo. |

The coarse sensors alone give a few degrees of accuracy, which is enough for sun pointing. A fine attitude estimate using the star tracker and FSS (a quaternion filter) is future work.

## 4. Control laws

The laws run at 5 Hz (period 0.2 s) in single precision, because the RP2350's FPU is single precision. The law functions are pure, with no device access, so the same code runs in the OBC, in the closed-loop unit test and on the Pico.

Every vector command is saturated by scaling the whole vector, not by clipping each axis. Clipping per axis would change the direction of the torque.

### 4.1 Detumble: B-dot (`DETUMBLE`)

m = −k · (dB/dt) / |B|, with k = 20 A m² s.

- dB/dt is the backward difference of the magnetometer reading, low-pass filtered with weight 0.5 on the newest sample.
- The first cycle after a mode change, and the first after a magnetometer outage, only records the field and commands nothing.
- At tumble rates the law saturates the torquers. Each torquer then works at full dipole with the correct sign: in effect bang-bang control.

**Limit.** A magnetic torque m × B is always perpendicular to B, so rotation about the local field line can't be damped directly. B-dot drives the field's rate of change in the body frame to zero. If a spin about the field line remains, B-dot then steers that spin to follow the field instead of removing it. The spin decays only through the misalignment that builds up between the spin and the field as the field direction turns, twice per orbit. The law's floor is therefore about twice the orbit rate (0.13°/s). Gyro-based rate damping, m = k (ω × B)/|B|², behaves the same way; it was tried in the unit test, brought no benefit, and was removed.

**Gain.** A higher gain removes the off-field rate faster, so the misalignment that does the damping never builds up: the spin is locked onto the field line. The gain was first taken from NOS3's ADCS configuration (k = 200). In the loop with 42 that value stalled at 1.65°/s, above the hand-over rate (NCR-009). A scan in the closed-loop unit simulation, over three tumbles, gave these times to 1°/s / to 0.5°/s:

| k (A m² s) | 7.1°/s tumble | 5.4°/s | 10.2°/s |
|---|---|---|---|
| 3 | 1100 / 1490 s | 900 / 1390 s | 1160 / 1660 s |
| 10 | 160 / 240 s | 140 / 360 s | 150 / 410 s |
| **20** | **90 / 130 s** | **70 / 320 s** | **80 / 110 s** |
| 40 | 140 / 710 s | 50 / 70 s | 70 / 450 s |
| 200 | 50 / 1500 s | 40 / 50 s | 710 / 2210 s |

At k = 20 the result is consistently fast across all three tumbles. Higher gains are erratic, depending on how the initial spin happens to sit against the field. The unit test checks all three tumbles, so a gain that suits only one of them fails.

**In the loop with 42** (real IGRF field, 5.4°/s tumble), B-dot removed most of the rate within 90 s. It then held a spin of about 1.6°/s for minutes: the field changed by only about 0.1°/s in the body frame, so the spin lay almost exactly along the field line. The unit model's uniformly turning field is kinder than a real orbit, where the field direction changes slowly near the magnetic equator.

**Hand-over.** Detumbling therefore doesn't wait for B-dot to finish. The design hands over to the wheels at 2°/s (§5). At that rate the residual momentum on Y or Z is 0.033 × 0.035 = 1.2 mN m s, about 11 % of one wheel's capacity. Sun pointing absorbs it into the wheels, and momentum management (§4.3) dumps it over the orbit, as the inertially held attitude sees the field from changing directions. A spin stalled above 2°/s still decays as the field turns, just more slowly.

### 4.2 Sun pointing (`SUN_POINT`)

The goal is to point body +X at the Sun. The command is a body torque from the wheels; each wheel is driven with the opposite torque.

With a valid Sun vector ŝ, the eigenaxis from +X to the Sun is ê = (x̂ × ŝ)/|x̂ × ŝ| and the error is θ = acos(x̂·ŝ). The law has a rate limit:

ω_des = min((k_p/k_d) θ, ω_max) ê, T = I k_d (ω_des − ω)

The gains are normalised by inertia: k_p = ω_n² and k_d = 2ζω_n. Multiplying by I (per axis) gives every axis the same closed-loop dynamics. For small errors:

θ̈ + k_d θ̇ + k_p θ = 0

| Parameter | Value |
|---|---|
| k_p, k_d | 0.04 s⁻², 0.28 s⁻¹ (ω_n = 0.2 rad/s, ζ = 0.7). These are the values NOS3 uses for inertial pointing. They can be changed with `ADCS_SET_SUN_GAINS`. |
| ω_max | 2°/s |
| Torque limit | 1 mN m per wheel |

- **Sun directly behind** (θ = 180°): x̂ × ŝ vanishes, so any axis perpendicular to +X is used.
- **Eclipse, or the Sun not trusted:** rate damping only, T = −I k_d ω. Attitude is held inertially until the Sun returns.
- **Converged:** θ < 5° and |ω| < 0.2°/s. Reported in `ADCS_STATE.CONVERGED` and by an event.

### 4.3 Momentum management (in `SUN_POINT`)

m = −k (B × h)/|B|², with k = 0.002 s⁻¹, where h is the wheel momentum.

The resulting torque is m × B = −k h, minus its component along B. This drains the wheels while they hold the attitude, with a time constant of about 500 s. The attitude law compensates for the small disturbance this causes.

## 5. Mode transitions

The automatic transitions run at 1 Hz and require automatic modes to be enabled: `OBC_SET_AUTO_MODES`, on by default, shown in `ADCS_STATE.AUTO_MODES`. Each condition must hold continuously for the time given.

| From | To | Condition | Reason |
|---|---|---|---|
| `SAFE` | `DETUMBLE` | Gyro and magnetometer valid; \|ω\| > 2.5°/s for 10 s (NOS3's default deployment tip-off is 2.8°/s) | `AUTO_RATES_HIGH` |
| `DETUMBLE` | `SUN_POINT` | Gyro valid; \|ω\| < 2°/s for 20 s (§4.1); ADCS node alive (sun pointing drives the wheels, and the physical wheel mirrors wheel 0) | `AUTO_CONVERGED` |
| `SUN_POINT` | `DETUMBLE` | \|ω\| > 5°/s for 10 s (control lost; slews stay below 2°/s) | `AUTO_RATES_HIGH` |
| `LOW_POWER` | `SAFE` | Simulated battery state of charge > 50 % for 10 s | `BATTERY_RECOVERED` |

The following responses are always active, whatever the automatic-mode setting:

| Condition | Response | Reason |
|---|---|---|
| Magnetometer declared failed in `DETUMBLE` | `SAFE`, with an event | `FAULT` |
| IMU or wheels declared failed in `SUN_POINT` | `SAFE`, with an event | `FAULT` |
| Simulated battery state of charge < 25 % for 5 s, in `SAFE`, `DETUMBLE` or `SUN_POINT` (not in `TEST`, where the operator is in control) | `LOW_POWER`. Actuators off; packets other than `OBC_HK`, `BEACON`, `EPS_SIM` and `EPS_REAL` drop to a tenth of their rate. | `LOW_BATTERY` |
| ADCS node lost, or no physical wheel telemetry, in `DETUMBLE` or `SUN_POINT` | `SAFE` (ICD §6.5) | `NODE_LOST` / `FAULT` |

A device is declared failed after about 2.5 s of consecutive failed reads: 12 reads at 5 Hz, or 3 at 1 Hz (NCR-005).

The state of charge is estimated from the NOS3 EPS simulator's battery voltage, which the simulator makes linear from 22.8 V (empty) to 25.2 V (full).

Loops are prevented by the guards. For example, after a magnetometer failure `SAFE` doesn't return to `DETUMBLE`, because that transition needs a valid magnetometer. After an IMU failure in `SUN_POINT`, the rates are low, so `SAFE` stays put.

Every mode change stops all actuators and resets the control law state, so each mode starts from a clean, known state.

## 6. Verification

| Level | Test | What it shows |
|---|---|---|
| Unit, closed loop | [`firmware/tests/test_adcs.c`](../../firmware/tests/test_adcs.c): a rigid-body simulation with the spacecraft's inertia, actuator limits, a field turning at twice the orbit rate, cosine sun sensors, and gyro and magnetometer noise. Dynamics at 200 Hz, laws at 5 Hz. | Detumble from three tumbles (5.4 to 10.2°/s): below 1°/s within 90 s, below 0.5°/s within 320 s, then at the B-dot floor. Sun pointing from 74.5° to within 2° in 47 s, with peak wheel torque 0.27 mN m and slew rate limited to 2°/s. The Sun-behind case. Rate damping in eclipse (0.71 to 0.003°/s in 60 s) and reacquisition. Momentum dump from 5.4 to 0.16 mN m s in 3000 s with pointing held within 0.33°. |
| System, closed loop with 42 | [`tests/system/test_adcs.py`](../../tests/system/test_adcs.py): the OBC with NOS3's simulators and 42 from a 5.4°/s tumble, checked against 42's truth stream rather than the OBC's own telemetry. | Automatic `SAFE` → `DETUMBLE` → `SUN_POINT`, convergence, pointing hold, IMU fault response, low battery entry and recovery, gain validation. Results in §7. |

## 7. Results in the loop with 42

From `tests/system/test_adcs.py`, run 2026-10-05: a 5.4°/s tumble, 42 patched for NCR-010, and every value taken from 42's truth stream. The time series is recorded as `adcs_timeseries.csv` in the run's log directory.

| Phase | Result |
|---|---|
| `SAFE` → `DETUMBLE` | Automatic, 13 s after start. All three torquers active, saturated at first. |
| Detumble | 5.39 → 1.45°/s in 92 s, then automatic `SUN_POINT`. |
| Slew | 108° at the 2°/s rate limit, no overshoot. Peak wheel torque 0.40 mN m in the first second, as the wheels absorb the remaining body momentum; then below 0.09 mN m. |
| Fine pointing | 2.8° after 60 s, 0.3° after 80 s, 0.03° after 100 s, at 0.007°/s. The OBC's coarse-sun-sensor estimate agreed with truth to within 0.25° throughout. |
| After convergence | The wheels hold the absorbed tumble: wheel X at 54 rad/s (0.93 mN m s, 9 % of capacity). The physical wheel mirrors it at 1/10 scale. |
| IMU failure | Declared after 2.4 s (12 reads), `SAFE` 3.9 s after the IMU stopped. The spacecraft stays at 0.01°/s with no automatic restart. |
| Low battery | 20 % → `LOW_POWER` (`ADCS_STATE` drops from 1 Hz to 0.1 Hz). 60 % → `SAFE` (`BATTERY_RECOVERED`). |
| Commanding | A NaN B-dot gain and a negative k_p are rejected. Automatic modes can be switched off. |

Earlier runs, before the NCR-010 fix, pointed with a 3–5° offset and overshoot. The cause was corrupted wheel torques in 42, not the control law.

