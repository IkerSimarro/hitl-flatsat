# HITL FlatSat — Wiring

Every connection between the FlatSat's boards, harness by harness, for the build. Pin numbers are the Pico 2's GPIO numbers, as in [PINOUT.md](PINOUT.md); parts are numbered as in the [BOM](BOM.md). Colours follow the BOM's wire set.

![Wiring diagram: the power distribution from the 18650 cell through the charger, main switch and three INA219 rail monitors to each Pico; the CAN bus between the OBC, ADCS and EPS nodes with 120 Ω at both ends; the motor driver, wheel, OLED and switch lines; each wire tagged with its harness number](../docs/images/wiring.svg)

| Colour | Use |
|---|---|
| Red | Battery positive (3.0–4.2 V), after the main switch |
| Black | Ground: one common ground for every board |
| Yellow / green | CAN-H / CAN-L, twisted together |
| Blue | Signals |
| White | Motor leads |

## 1. Power

```
18650 cell ── TP4056 (BAT+/BAT-) ── OUT+ ── main switch ── INA219 #0 (0x40) ──┬── INA219 #1 (0x41) ── 1N5817 ── OBC VSYS (radio board on the OBC's 3V3)
  (USB-C on the TP4056 charges it)                     "battery output"       ├── INA219 #2 (0x44) ──┬── 1N5817 ── ADCS VSYS
                                                                              │   "ADCS rail"        └── DRV8833 VM (motor supply)
                                                                              └── 1N5817 ── EPS VSYS
```

| # | From | To | Notes |
|---|---|---|---|
| P1 | 18650 holder + / − | TP4056 BAT+ / BAT− | Use the protected TP4056 board (DW01 + 8205A); check the cell's polarity before inserting it |
| P2 | TP4056 OUT+ | Main switch | |
| P3 | Main switch | INA219 #0 VIN+ | Each INA219 is high-side: VIN+ from the source, VIN− to the load |
| P4 | INA219 #0 VIN− | Distribution point (red bus) | |
| P5 | Red bus | INA219 #1 VIN+; VIN− → 1N5817 anode; cathode → OBC VSYS (pin 39) | The diode lets USB and the battery both feed the OBC: on the bench, USB powers it |
| P6 | Red bus | INA219 #2 VIN+; VIN− → 1N5817 anode → ADCS VSYS (pin 39), and VIN− → DRV8833 VM | Motor and ADCS Pico on one monitored rail (ICD 6.3, rail 2) |
| P7 | Red bus | 1N5817 anode → EPS VSYS (pin 39) | The EPS node's own consumption is part of rail 0 only |
| P8 | TP4056 OUT− / every board GND | Black bus | One star point near the battery |

**Before the first power-on:** with the cell out, check with the multimeter that there's no short between the red and black buses, and that each diode conducts towards its Pico. Power each board from USB on its own first.

## 2. CAN bus (ICD 6.1)

| # | From | To | Notes |
|---|---|---|---|
| C1 | OBC Pico-CAN-B CAN-H / CAN-L | ADCS Pico-CAN-B CAN-H / CAN-L | Twisted pair, about 30 cm |
| C2 | ADCS Pico-CAN-B CAN-H / CAN-L | EPS Pico-CAN-B CAN-H / CAN-L | Twisted pair |
| C3 | 120 Ω termination | On at the OBC and the EPS (the ends), off at the ADCS node | The Pico-CAN-B has a termination jumper; check it per board. Measured between CAN-H and CAN-L with power off: about 60 Ω |

The three CAN boards plug onto their Picos; the CAN controller's interrupt jumper (GP21/GP22) can stay in either position: the firmware polls it.

## 3. ADCS node (reaction wheel)

| # | From | To | Notes |
|---|---|---|---|
| A1 | ADCS GP16 / GP17 | DRV8833 AIN1 / AIN2 | Blue |
| A2 | DRV8833 AOUT1 / AOUT2 | N20 motor terminals | White; the direction check is in [INT-07](INTEGRATION.md#int-07-the-reaction-wheel) |
| A3 | N20 encoder VCC / GND | ADCS 3V3 (pin 36) / GND | 3.3 V, so the encoder's outputs are 3.3 V logic |
| A4 | Encoder A / B | ADCS GP13 / GP14 | Swap them if the wheel runs away to full speed reading a negative speed ([INT-07](INTEGRATION.md#int-07-the-reaction-wheel)) |
| A5 | DRV8833 nSLEEP | EPS GP17 and ADCS GP18 | The EPS node drives it; the ADCS node only reads it |
| A6 | DRV8833 GND | Black bus | |
| A7 | ADCS RUN (pin 30) | EPS GP16 | The EPS node holds it low to keep the node in reset |

## 4. EPS node (power)

| # | From | To | Notes |
|---|---|---|---|
| E1 | EPS GP8 / GP9 | INA219 #0, #1, #2 SDA / SCL | One I2C bus; addresses 0x40, 0x41, 0x44 set with each breakout's A0/A1 pads (solder) |
| E2 | EPS 3V3 (pin 36) / GND | INA219 VCC / GND (all three) | |
| E3 | TP4056 CHRG / STDBY | EPS GP13 / GP14 | Open drain, active low; the Pico's pull-ups are enough |
| E4 | EPS GP16 | ADCS RUN | See A7 |
| E5 | EPS GP17 | DRV8833 nSLEEP | See A5 |

## 5. OBC

| # | From | To | Notes |
|---|---|---|---|
| O1 | OBC USB | The PC running NOS3 | The umbilical (ICD IF-01) |
| O2 | Pico-LoRa-SX1262 | Plugs onto the OBC, with its antenna fitted | Never transmit without the antenna |
| O3 | OBC GP8 / GP9 | OLED SDA / SCL | 3V3 and GND from the OBC |

The CAN board and the LoRa board both plug onto the OBC; the dual expander (BOM item 4) holds them side by side.

## 6. Ground modem

| # | From | To | Notes |
|---|---|---|---|
| G1 | Modem USB | The ground station PC | ICD 7.5 |
| G2 | Pico-LoRa-SX1262 | Plugs onto the modem Pico, with its antenna | |

## 7. Bring-up order

The [integration procedure](INTEGRATION.md) adds the boards and harnesses one at a time, each with a check and a pass criterion: each Pico alone, then its add-on boards, the radio link, the CAN bus, the power monitors and battery, the wheel and fault switches, everything on the battery, and the full system under NOS3.
