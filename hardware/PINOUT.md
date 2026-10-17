# HITL FlatSat — Pin assignments

Which Raspberry Pi Pico 2 pin does what, on each of the four boards. The firmware uses these assignments (`firmware/hal/pico/`), and the wiring diagram follows them. Pins taken by the plug-on boards are fixed by those boards; everything else was chosen to stay clear of them.

## Plug-on boards (fixed by their design)

| Board | Pins | Notes |
|---|---|---|
| Waveshare Pico-CAN-B (MCP2515) | GP4 MISO, GP5 CS, GP6 SCK, GP7 MOSI (SPI0) | Also routes GP19 (alternative chip select) and GP21 or GP22 (interrupt, selected by a jumper). The firmware polls the controller, so those stay unused, and nothing else is put on them. |
| Waveshare Pico-LoRa-SX1262-868M | GP10 SCK, GP11 MOSI, GP12 MISO (SPI1), GP2, GP3, GP15, GP20, GP26 | The control lines (chip select, reset, busy, DIO1, antenna) are confirmed against the board's schematic with the radio driver. |

## OBC (flight computer)

| Pin | Function | Notes |
|---|---|---|
| USB | Umbilical to the PC (HIL bridge) | Also powers the board on the bench |
| GP0 / GP1 | UART0 TX / RX: debug log, 115200 baud | Optional USB–serial adapter |
| GP4–GP7 | CAN (Pico-CAN-B) | |
| GP2, GP3, GP10–GP12, GP15, GP20, GP26 | LoRa radio (Pico-LoRa-SX1262) | |
| GP8 / GP9 | I2C0 SDA / SCL: SH1106 OLED status display, address 0x3C | |
| GP25 | On-board LED: blinks while the software runs | |

## ADCS node (reaction wheel)

| Pin | Function | Notes |
|---|---|---|
| USB | Log, as plain text for any serial terminal | Kept from start-up until a terminal opens the port; also powers the board on the bench |
| GP4–GP7 | CAN (Pico-CAN-B) | |
| GP16 / GP17 | DRV8833 AIN1 / AIN2: motor PWM, 20 kHz | One PWM slice (0), both channels |
| GP13 / GP14 | Encoder A / B (Hall, quadrature) | Interrupt on every edge; the encoder runs from 3.3 V |
| GP18 | DRV8833 nSLEEP, sensed (input) | Driven by the EPS node; tells the node whether its driver is enabled |
| RUN | Held low by the EPS node to keep the node in reset | The Pico's own RUN pin, not a GPIO |
| GP0 / GP1 | UART0 debug log | |
| GP25 | On-board LED | |

## EPS node (power)

| Pin | Function | Notes |
|---|---|---|
| USB | Log, as plain text for any serial terminal | Kept from start-up until a terminal opens the port; also powers the board on the bench |
| GP4–GP7 | CAN (Pico-CAN-B) | |
| GP8 / GP9 | I2C0 SDA / SCL: three INA219 monitors | Battery output 0x40, OBC + radio rail 0x41, ADCS rail 0x44 (A0/A1 pads); 0.1 Ω shunts |
| GP16 | Switch 0: the ADCS node's RUN pin | Driven low to hold the node in reset; released by turning the pin into an input, so it never drives the other board's RUN pin high |
| GP17 | Switch 1: DRV8833 nSLEEP (wheel motor power) | Driven high to enable; the DRV8833's own pull-down disables it if this board is off |
| GP13 / GP14 | TP4056 CHRG / STDBY, active low | Inputs with pull-ups (open-drain outputs on the charger) |
| GP0 / GP1 | UART0 debug log | |
| GP25 | On-board LED | |

## Ground station modem

| Pin | Function | Notes |
|---|---|---|
| USB | To the ground station PC (`ground/flatsat_gs.py`) | |
| GP2, GP3, GP10–GP12, GP15, GP20, GP26 | LoRa radio (Pico-LoRa-SX1262) | |
| GP25 | On-board LED | |

## Free on every board

GP18 (except the ADCS node), GP27, GP28: kept for later use.
