# HITL FlatSat — Bill of Materials

| | |
|---|---|
| Version | 1.0 (2026-10-01) |
| Status | Ready to order. Prices are approximate and were checked in October 2026 where a source is linked. |

Interfaces and node roles are defined in the [ICD](../docs/icd/ICD.md). Choices favour boards that plug onto the Pico with no soldering and are safe at 3.3 V, so the riskiest parts of the build need no rework.

## 1. FlatSat

| # | Qty | Part | Used by | Approx. € | Where | Notes |
|---|---|---|---|---|---|---|
| 1 | 4 | Raspberry Pi Pico 2 **H** (pre-soldered headers) | OBC, ADCS, EPS, ground modem | 4 × 7 | Kubii, Botland | One already ordered; buy 3 more |
| 2 | 3 | Waveshare **Pico-CAN-B** (MCP2515 + 3.3 V SIT65HVD230 transceiver, 16 MHz crystal) | OBC, ADCS, EPS | 3 × 12.50 | [Botland](https://botland.store/raspberry-pi-pico-hat-communications/22827-can-bus-module-for-raspberry-pi-pico-waveshare-23775.html) | No soldering. Uses GP4–GP7 (SPI0). |
| 3 | 2 | Waveshare **Pico-LoRa-SX1262-868M** | OBC, ground modem | 2 × 17 | Amazon.es, Waveshare store | Uses GP10–12 (SPI1), GP2, GP3, GP15, GP20, GP26; no clash with the CAN board. **Check that each includes an 868 MHz antenna.** |
| 4 | 1 | Waveshare Pico dual or quad expander | OBC (Pico + CAN + LoRa side by side) | 10 | Amazon.es, Botland | Optional: a breadboard works, but this looks much tidier |
| 5 | 3 | INA219 current/voltage sensor breakout (I2C, 3.3 V OK) | EPS | 3 × 2 | AliExpress, Amazon.es | Headers need soldering. Give each a different I2C address with the A0/A1 pads. |
| 6 | 1 | DRV8833 dual motor driver breakout | ADCS | 2 | AliExpress, Amazon.es | Runs directly from the 18650 (2.7–10.8 V) |
| 7 | 1 | N20 gear motor, 6 V, ~1000 rpm, **with magnetic Hall encoder** | ADCS (reaction wheel) | 7 | AliExpress, Amazon.es | Faster gearing gives a more convincing flywheel |
| 8 | 1 set | N20 3 mm D-shaft metal hub + M3 screws + 4–6 steel washers (M8/M10) | Flywheel | 5 | Hardware store, AliExpress | Replaces a 3D-printed wheel |
| 9 | 1 | 1.3" I2C OLED, 128 × 64 (SH1106) | OBC status display | 5 | AliExpress, Amazon.es | |
| 10 | 1 | **Protected** 18650 cell from a reputable seller | Power | 8 | Nkon.nl, a local battery shop | Avoid unbranded "9900 mAh" cells: they're fake |
| 11 | 1 | 18650 holder with leads | Power | 2 | | |
| 12 | 2 | TP4056 USB-C charger board **with protection** (DW01 + 8205A) | Power | 2 | AliExpress, Amazon.es | One spare. CHRG/STDBY pins go to EPS GPIOs (charge state, ICD 6.3). |
| 13 | 1 pack | 1N5817 Schottky diodes | Power OR-ing into each Pico's VSYS | 2 | | Lets USB and battery both feed a Pico safely |
| 14 | 1 | Panel slide switch | Main power | 2 | | |
| 15 | 1 | USB logic analyser, 8 ch, 24 MHz (works with PulseView/sigrok) | Test equipment | 10 | AliExpress, Amazon.es | Decodes CAN, SPI and I2C traces for the test report |
| 16 | 1 | Base plate: pre-drilled aluminium sheet or large FR4 perfboard (~20 × 30 cm) | Mechanical | 8 | Amazon.es, hardware store | No drilling needed |
| 17 | 1 kit | M2.5/M3 nylon standoffs, screws and nuts | Mechanical | 7 | | Pico mounting holes take M2 to M2.5 screws |
| 18 | 1 set | Dupont jumper wires (M-M, M-F, F-F) | Wiring | 4 | | |
| 19 | 1 set | 22 AWG silicone wire, several colours | Harness, CAN twisted pair | 7 | | Colour-code power, ground, CAN-H, CAN-L |
| 20 | 2 | Half-size breadboard | EPS rail sensing, bench tests | 3 | | |
| 21 | 1 pack | Heat-shrink assortment | Harness | 2 | | |
| | | | | **≈ 170** | | **≈ 155** without the expander and with AliExpress for the small parts |

## 2. Tools

| Qty | Tool | Approx. € | Notes |
|---|---|---|---|
| 1 | Pinecil V2 soldering iron (or a budget TS-style clone) | 30 | Needs a **USB-C PD charger of 45 W or more**, which most laptop chargers are. Add about €15 if you don't have one. |
| 1 | Digital multimeter (e.g. UNI-T UT33-series) | 20 | Continuity beeper and DC current range |
| 1 | Solder wire 0.8 mm with flux core + a flux pen | 10 | |
| 1 | Brass tip cleaner | 4 | |
| 1 | Wire stripper + flush cutters | 12 | |
| 1 | Safety glasses | 4 | Clipped leads fly |
| | **Tools total** | **≈ 80** | |

**Grand total ≈ €235–250**, of which roughly €80 is tools you keep. That's above the original €100–150 FlatSat budget, mainly because of the plug-on CAN and LoRa boards. They cost about €40 more than bare modules, but they remove most of the soldering and every 3.3 V/5 V mismatch. To get closer to €150, drop the expander and buy items 5–9 and 15–21 on AliExpress (2–3 weeks shipping).

## 3. Design notes

**Why these boards**
- **CAN:** most cheap MCP2515 modules use a TJA1050 transceiver that needs 5 V, which would put 5 V logic on the Pico's 3.3 V pins. The Pico-CAN-B uses a 3.3 V transceiver and plugs straight on. Its 16 MHz crystal supports the ICD's 500 kbit/s.
- **LoRa:** the SX1262 is the successor to the SX1276 (RFM95W) in the ICD draft, with the same LoRa modulation. The ICD's radio parameters stay the same; only the sync-word register changes (`0x12` private network becomes `0x1424` on SX126x).
- **Power:** each Pico 2 has its own buck-boost regulator that accepts 1.8–5.5 V on VSYS, so the 18650 (3.0–4.2 V) feeds them directly through a Schottky diode. No separate 3.3 V regulator is needed, and each Pico's 3V3 pin powers its own sensors.

**Fault-injection "load switches" (ICD DD-07).** To keep the wiring safe for a first build, the EPS node controls:
- **"ADCS node power":** the ADCS Pico's RUN pin. Holding it low keeps the RP2350 in reset and its consumption drops, which is visible on the INA219.
- **"Wheel motor power":** the DRV8833's nSLEEP pin, which cuts drive to the motor.

These behave like power switches for every test in the campaign. Upgrading to real high-side MOSFET switches is a stretch goal.

**Pins to confirm when the boards arrive**
- The Pico-CAN-B interrupt pin isn't documented. The firmware can poll the MCP2515 over SPI instead, so this doesn't block anything.

## 4. Order status

| Item | Status |
|---|---|
| Pico 2 H × 1 | Ordered 2026-10-01 |
| Everything else | To order |
