# HITL FlatSat — Integration procedure

How to go from a box of parts to a FlatSat ready for its hardware-in-the-loop campaign, one board and one harness at a time. Each step adds one thing and checks it before the next, so a fault is always in the last thing added.

Every step has an ID (INT-01 …), what to do, what you should see, and a pass criterion. Write each result in the [as-run record](#as-run-record) at the end, as you would on a real integration: the date, what you saw, pass or fail, and anything odd. A failure is an NCR in [the log](../docs/ncr/NCR_LOG.md), like any found in testing.

The harness numbers (P1, C1, A1 …) are the ones in [WIRING.md](WIRING.md) and on the [wiring diagram](../docs/images/wiring.svg); pins are in [PINOUT.md](PINOUT.md).

## Before you start

**You need:** the parts in the [BOM](BOM.md), the multimeter, a USB cable that carries data for each Pico (a charge-only cable powers the Pico but the PC never sees it), and the PC with this repository built (README, Getting started).

**Rules for the whole procedure:**
- **Never power a LoRa board without its antenna.** Transmitting into nothing can damage the radio, and the OBC sends a beacon every 10 s as soon as it starts.
- **Wire with the power off.** Unplug USB and switch the battery off before changing any wire.
- **Check the 18650's polarity before inserting it,** and only use the protected cell and the protected charger board.
- **One ground.** Every board's GND goes to the same point (P8), or the CAN bus and the I2C lines misbehave.

**Two tools do most of the checking:**
- `tools/pico/flash.sh firmware/build-pico/<board>.uf2` puts a board's firmware on the Pico on USB (the first time, hold BOOTSEL while plugging it in). `tools/pico/build.sh` builds all four: `obc`, `adcs_node`, `eps_node`, `modem`.
- `tools/pico/console.py` shows what the board on USB says, without NOS3. A node prints its log; the OBC and the modem speak the HIL link, and the console decodes it. Stop it with Ctrl-C.

## INT-01 Parts check

1. Check every BOM line is there and undamaged.
2. Solder the INA219 address pads: #0 none (0x40), #1 A0 (0x41), #2 A1 (0x44). Label the three boards.
3. On the three Pico-CAN-B boards, set the 120 Ω termination jumper **on** for the OBC's and the EPS node's board and **off** for the ADCS node's (C3). Label them.
4. Check the LoRa boards came with 868 MHz antennas.

**Pass:** every part present, the INA219s addressed and labelled, the CAN boards' jumpers set and labelled.

## INT-02 Each Pico on its own

For each Pico in turn, with nothing plugged onto it:

1. Flash its firmware, then run `tools/pico/console.py --seconds 30`.
2. Watch the console and the Pico's LED.

| Board | The console shows |
|---|---|
| ADCS node | `no CAN board (MCP2515 error -2)`, `ADCS node started, reset cause 0`, `motor driver disabled (nSLEEP low)` |
| EPS node | `no CAN board (MCP2515 error -2)`, three `rail monitor (INA219 at 0x4n) not answering`, `EPS node started, …, switches 0x03` |
| OBC | lines starting `at 0.0xx s:`: `no CAN board`, `no radio board (SX1262 error -2): RF frames go over the umbilical`, `no status display`; then events and a summary every 10 s |
| Ground modem | `[console] modem radio not answering` |

**Pass:** each board's console names it (`[console] /dev/ttyACM0: FlatSat …`), it reports its add-on boards missing, the LED blinks twice a second, and the console ends with `30 s, no restart seen`.

## INT-03 The add-on boards, one at a time

Still on USB power, each board alone. After plugging a board on, run the console again.

| # | Plug on | The console shows |
|---|---|---|
| a | OBC: its Pico-CAN-B (on the dual expander) | `at 0.0xx s: CAN board: MCP2515 at 500 kbit/s` |
| b | ADCS node: its Pico-CAN-B | `CAN board: MCP2515 at 500 kbit/s` |
| c | EPS node: its Pico-CAN-B | `CAN board: MCP2515 at 500 kbit/s` |
| d | Ground modem: its LoRa board, **antenna first** (G2) | `[console] modem radio working`, and the modem's `radio ready: 869525000 Hz, SF7, 125 kHz, 2 dBm` |
| e | OBC: its LoRa board, **antenna first** (O2) | `at 0.0xx s: radio: SX1262 at 869525000 Hz, 2 dBm` |
| f | OBC: the OLED, four wires (O3: GP8 SDA, GP9 SCL, 3V3, GND) | `at 0.0xx s: status display: SH1106 OLED`; the screen shows `FLATSAT  SAFE` and seven more lines |

**Pass:** every line as in the table, and no restart in 30 s with each board added.

## INT-04 The radio link

1. Ground modem on the PC's USB, its console running (`tools/pico/console.py /dev/ttyACM1`, the modem's port).
2. OBC (with its LoRa board) on another USB port or a charger, a metre away.

**Expect:** every 10 s the modem's console shows `received BEACON, … bytes, RSSI … dBm, SNR … dB`.

**Pass:** five beacons in a row received, RSSI stronger than −80 dBm and SNR above 5 dB at a metre at 2 dBm.

## INT-05 The CAN bus

1. Power everything off. Connect the twisted pairs C1 (OBC to ADCS node) and C2 (ADCS node to EPS node): CAN-H to CAN-H, CAN-L to CAN-L, and connect the three boards' grounds.
2. With the multimeter on resistance, measure between CAN-H and CAN-L: **about 60 Ω** (two 120 Ω terminations in parallel). About 120 Ω means one termination is off; about 40 Ω means three are on.
3. Power the three boards from USB. Run the OBC's console.

**Expect:** `EVENT … EPS node up (uptime … s, reset cause 0)` and `EVENT … ADCS node up …`; the OLED shows `NODES ADCS ok EPS ok`.

**Pass:** 55–65 Ω; both nodes up within 5 s of the OBC's console starting, and no `node lost (no heartbeat …)` event in a minute.

## INT-06 The power monitors and the battery

1. Power off. Wire the power chain on the breadboard (P1–P4): cell holder, TP4056, switch, INA219 #0, and the red bus. Wire the three INA219s' I2C to the EPS node (E1, E2), and the charger's CHRG and STDBY (E3). Leave the Picos on USB for now; the rails don't feed them yet.
2. Insert the cell (polarity!) and switch on. Run the EPS node's console, then the OBC's.
3. With the multimeter, measure the battery bus voltage, then put it in series (current range) between the switch and INA219 #0.

**Expect:** the EPS node logs each `rail monitor (INA219 at 0x4n) answering`. The OLED's `BATT` line shows the cell's voltage and current.

**Pass:** the OLED's voltage within 0.05 V of the multimeter's; the current within 5 mA or 3 %, whichever is larger. (The charger's status lines, E3, are checked in COSMOS in INT-09: its charge state follows the charger's LEDs.)

## INT-07 The reaction wheel

1. Power off. Wire the DRV8833 (A1, A2, A6), the motor's encoder (A3, A4), the motor driver's nSLEEP from the EPS node (A5, E5), and the ADCS node's RUN pin to the EPS node (A7, E4). Motor supply VM from the ADCS rail (P6).
2. Power on. Run the OBC's console with:
   `tools/pico/console.py --command "OBC_SET_MODE MODE=TEST" --command "ADCS_PHYS_WHEEL_TEST CTRL_MODE=SPEED SETPOINT=300"`
3. Watch the wheel and the OLED's `WHEEL` line (measured / commanded).

**Expect:** the wheel spins up; `WHEEL` shows about +300/+300 within 2 s.

**If it runs away to full speed with a negative measured speed,** the encoder counts against the motor: power off and swap the encoder's A and B (A4). **If it then holds +300 but turns clockwise** (seen from the wheel's side; positive is anticlockwise), swap the motor leads (A2) and the encoder's A and B together.

**Then the fault switches,** with the wheel running (add them as further `--command`s): `EPS_SWITCH SWITCH_ID=1 STATE=OFF` cuts the motor driver, and the wheel coasts to a stop; `STATE=ON` brings it back to +300. `EPS_SWITCH SWITCH_ID=0 STATE=OFF` holds the ADCS node in reset: after 3 s the OBC logs `ADCS node lost (no heartbeat for 3 s)`; `STATE=ON` brings back `ADCS node up (… reset cause 0)`.

**Pass:** +300 rpm reached and held within 5 % (TC-02.2's criterion on the real wheel); −300 rpm likewise; both switches act as described.

## INT-08 On the battery

1. Power off. Wire the rails to the Picos' VSYS through their diodes (P5–P7). Unplug the ADCS and EPS nodes' USB cables; keep the OBC's, which is its link to the PC (the diode lets USB and the battery feed it together). The ground modem stays on the PC, off the battery.
2. Switch on.

**Expect:** the ADCS and EPS nodes start from the battery: the OBC's console shows both `node up`, the OLED's `BATT` line shows the whole FlatSat's current.

**Pass:** the three FlatSat boards running from the cell for 10 minutes with no restart. Note the battery current in the record: it is the FlatSat's measured power budget.

## INT-09 The full system

With everything wired and the OBC and the modem on the PC: NOS3, the bridge and COSMOS run with the real boards, and the regression campaign runs on them (hardware in the loop, the full FlatSat). This configuration, with the CAN nodes, the radio link and the power measurements real instead of simulated, is the next step of the [test plan](../docs/test/TEST_PLAN.md); this procedure gets the hardware ready for it.

**Pass:** the FlatSat runs for an orbit (about 100 minutes) under NOS3 with no restart.

## As-run record

Copy this table into the integration log and fill it in as you go.

| Step | Date | Result (what you saw) | Pass / fail | Notes, NCR |
|---|---|---|---|---|
| INT-01 Parts check | | | | |
| INT-02 Each Pico on its own | | | | |
| INT-03 a–f Add-on boards | | | | |
| INT-04 Radio link | | | | |
| INT-05 CAN bus | | | | |
| INT-06 Power monitors, battery | | | | |
| INT-07 Reaction wheel, switches | | | | |
| INT-08 On the battery | | | | |
| INT-09 Full system | | | | |
