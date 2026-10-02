# HITL FlatSat

A hardware-in-the-loop FlatSat: four real Raspberry Pi Pico 2 subsystem boards, tested against a simulated orbit from NASA's [NOS3](https://github.com/nasa/nos3) small-satellite testbed.

> **Status:** early development. The software-in-the-loop phase runs during October 2026 and the hardware build follows. This README will grow as the project does.

## Concept

| Subsystem | Real hardware | Simulated by NOS3 |
|---|---|---|
| Flight computer (OBC) | Pico 2: command handling, mode management, ADCS software | Sensor data, through the USB umbilical and the HIL bridge |
| ADCS actuator node | Pico 2 + encoder motor reaction wheel | Spacecraft attitude (42 dynamics) |
| Power node (EPS) | Pico 2 + INA219 rail monitors + 18650 cell | Solar input and eclipses |
| Comms | LoRa link between the FlatSat and the ground station | Ground-station contact windows |
| Inter-board bus | CAN (MCP2515) | – |

## Repository layout

| Path | Contents |
|---|---|
| `nos3/` | Submodule: [fork of NOS3](https://github.com/IkerSimarro/nos3) (`hitl` branch). Adds the HIL bridge component (`components/hil_bridge`) and the `HIL=1` launch mode. |
| `firmware/` | Subsystem firmware. `common/`: CCSDS packets, mission clock, scheduler, umbilical client and the generated interface header; `obc/`: flight computer software (commands, modes, events, telemetry, sensor acquisition); `obc/devices/`: drivers for the NOS3-simulated sensors and actuators; `hal/linux/`: software-in-the-loop backend (pty umbilical, SocketCAN); `tests/`: unit tests on a fake HAL and the SIL device test. Pico backend *(planned)*. |
| `ground/` | Ground station software *(planned)*; generated Python codec `flatsat_icd.py` and COSMOS definitions `cosmos/FLATSAT/` |
| `sil/` | Software-in-the-loop environment: NOS3 simulators, headless 42 and the HIL bridge in one container (`sil/sil.sh`) |
| `tests/` | Unit tests, the COSMOS definition cross-check and system tests (`tests/system/`); test procedures and the automated campaign *(planned)* |
| `docs/` | [Interface control document](docs/icd/ICD.md) (v1 draft) with generated [byte layouts](docs/icd/ICD_layouts.md); [non-conformance log](docs/ncr/NCR_LOG.md); test plan and reports, design notes *(planned)* |
| `tools/` | `icd_gen.py`: generates all interface code from `docs/icd/flatsat_icd.yaml` |
| `hardware/` | [Bill of materials](hardware/BOM.md); wiring diagram and harness definition *(planned)* |

## Getting started

```bash
git clone --recurse-submodules https://github.com/IkerSimarro/hitl-flatsat.git
cd hitl-flatsat/nos3
make prep && make config && make
```

See [`nos3/components/hil_bridge/README.md`](nos3/components/hil_bridge/README.md) for the bridge protocol and the end-to-end test.

### Tests

```bash
# Interface definitions: generated files up to date, codec unit tests, COSMOS cross-check
python3 tools/icd_gen.py --check
python3 -m unittest discover -s tests/unit
tests/cosmos/check_defs.sh

# Firmware unit tests (in the NOS3 build image)
docker run --rm -v $PWD:$PWD -w $PWD ivvitc/nos3-64:20260619 bash -c \
    'cmake -S firmware -B firmware/build && make -C firmware/build && firmware/build/test_common'

# Flight computer drivers against the live NOS3 simulators and 42 truth (software-in-the-loop)
sil/sil.sh firmware/build/test_devices --umb-pty '$SIL_UMB_PTY' --can none

# Flight computer commanding and telemetry over the umbilical (software-in-the-loop)
sil/sil.sh 'firmware/build/obc --umb-pty $SIL_UMB_PTY --can none > $SIL_LOG_DIR/obc.log 2>&1 &
            python3 tests/system/test_obc_umbilical.py'
```

## Licence

NOS3 is released by NASA under the NASA Open Source Agreement 1.3; see `nos3/LICENSE`.
