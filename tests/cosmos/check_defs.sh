#!/bin/bash
#
# Checks that COSMOS decodes and encodes FlatSat packets exactly as the Python codec does.
# Needs Docker and the ballaerospace/cosmos:4.5.0 image that NOS3 uses.
#
set -eo pipefail
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/../.." &> /dev/null && pwd )
WORK=$(mktemp -d)
trap 'rm -rf $WORK' EXIT

# Encode sample packets with the Python codec, recording the values COSMOS should read back
python3 - > $WORK/packets.txt <<'PY'
import json, sys
sys.path.insert(0, "ground")
import flatsat_icd as icd

def tlm(name, enum_items=(), **fields):
    pkt = icd.build_telemetry(name, seconds=814254200, **fields)
    expected = {k: (v.decode() if isinstance(v, bytes) else v) for k, v in fields.items()}
    for item, enum in enum_items:  # COSMOS reads enum items as their state name
        expected[item] = {v: k for k, v in enum.items()}[fields[item]]
    print("TLM", name, pkt.hex(), json.dumps(expected))

def cmd(name, **args):
    print("CMD", name, icd.build_command(name, **args).hex(), json.dumps(args))

tlm("OBC_HK", [("MODE", icd.MODE)], UPTIME=1234, MODE=2, CAN_TEC=7, NODE_ALIVE_MASK=0b1110)
tlm("BEACON", [("MODE", icd.MODE)], UPTIME=99, MODE=1, SIM_BATT_MV=24000, REAL_BATT_MV=3900)
tlm("EVENT", [("SEVERITY", icd.SEVERITY)], EVENT_ID=42, SEVERITY=3, TEXT=b"wheel fault")
tlm("ADCS_STATE", RATE_EST_1=-0.25, PHYS_RW_MEAS_SPEED=12.5, TRQ_DUTY_2=-5000)
tlm("ADCS_SENSORS", VALID_MASK=0x3F, ST_QUAT_3=1.0, GPS_SOW=12345.5)
tlm("EPS_SIM", BATT_V=24.0, SA_V=32.0, ECLIPSE=1)
tlm("EPS_REAL", RAIL_MA_2=-150, BATT_MV=3950, EPS_UPTIME=77)
tlm("COMMS_STATS", LAST_RSSI=-87, LAST_SNR=-12, DUTY_CYCLE=48)
tlm("PING_REPLY", TOKEN=0xDEADBEEF, RX_SUBSECONDS=0x8000)
cmd("OBC_SET_MODE", MODE=1)
cmd("OBC_SET_TIME", SECONDS=814254200, SUBSECONDS=0x4000)
cmd("ADCS_RW_MANUAL", WHEEL=2, SPEED=-3.5)
cmd("ADCS_TRQ_MANUAL", TORQUER=1, DUTY=-2500)
cmd("EPS_SWITCH", SWITCH_ID=1, STATE=0)
cmd("COMMS_SET_TX_POWER", POWER=-3)
PY

docker run --rm -v $ROOT:$ROOT -v $WORK:$WORK -w /tmp -e COSMOS_USERPATH=$WORK ballaerospace/cosmos:4.5.0 \
    ruby $ROOT/tests/cosmos/check_defs.rb $ROOT/ground/cosmos/FLATSAT/cmd_tlm $WORK/packets.txt 2>&1 \
    | grep -v '^[IW], '
