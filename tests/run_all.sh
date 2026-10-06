#!/bin/bash
#
# Regression campaign: runs every test stage in order and prints a summary table. Needs Docker, the NOS3
# image and a built NOS3 (make config && make, scripts/gsw/gsw_cosmos_build.sh). Takes about 30 minutes (2½ hours with the orbit).
#
# Every check prints "PASS [TC-nn.m] ..." or "FAIL [TC-nn.m] ...", the test procedure step it verifies
# (docs/test/verification.yaml). Each run leaves in tests/logs/run-<time>/: one log per stage, the stage's SIL
# logs and recorded data (<stage>.d/), and summary.json, from which tools/test_report.py writes the test report.
#
#   tests/run_all.sh            every stage except the long one (the one-orbit endurance test, ~100 min)
#   tests/run_all.sh all        every stage except the blocked one (TC-15: tests/run_all.sh reference)
#   tests/run_all.sh unit sil   only the stages whose name contains one of the words
#
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )
cd "$ROOT" || exit 1
IMAGE=ivvitc/nos3-64:20260619
LOG=$ROOT/tests/logs/run-$(date +%Y%m%d-%H%M%S)
mkdir -p "$LOG"
# The software under test, recorded before any stage runs (for the test report)
export SUT_COMMIT=$(git rev-parse --short HEAD) SUT_NOS3_COMMIT=$(git -C nos3 rev-parse --short HEAD)
export SUT_DIRTY=$(git status --porcelain --untracked-files=no | head -c1)
NODES='sil/start_nodes.sh && (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) &&'

# A procedure step run as a command (the unit stages): prints its PASS/FAIL line, remembers a failure
step()
{
    local id=$1 title=$2
    shift 2
    if "$@"; then echo "PASS [$id] $title"; else echo "FAIL [$id] $title"; STEP_FAILED=1; fi
}
stage_ok() { [ -z "${STEP_FAILED:-}" ]; }

STAGES=(
    "unit:icd         step TC-01.1 'generated files up to date' python3 tools/icd_gen.py --check; step TC-01.2 'codec and RF unit tests' python3 -m unittest discover -s tests/unit; stage_ok"
    "unit:cosmos-defs step TC-01.3 'COSMOS definitions match the codec' tests/cosmos/check_defs.sh; step TC-01.4 'COSMOS screens match the definitions' python3 tests/cosmos/check_screens.py; stage_ok"
    "unit:firmware    docker run --rm -v $ROOT:$ROOT -w $ROOT $IMAGE firmware/tests/run_unit_tests.sh"
    "e2e:bridge       cd nos3 && docker run --rm -v \$PWD:\$PWD --add-host nos-engine-server:127.0.0.1 --add-host sc01-nos-engine-server:127.0.0.1 $IMAGE \$PWD/components/hil_bridge/support/e2e_test.sh"
    "sil:devices      SIL_INIT_RATES='2 -3 4' sil/sil.sh firmware/build/test_devices --umb-pty '\$SIL_UMB_PTY' --can none"
    "sil:adcs         SIL_INIT_RATES='2 -3 4' sil/sil.sh '$NODES python3 tests/system/test_adcs.py'"
    "sil:umbilical    sil/sil.sh '$NODES python3 tests/system/test_obc_umbilical.py'"
    "sil:can-nodes    sil/sil.sh '$NODES python3 tests/system/test_can_nodes.py'"
    "sil:faults       sil/sil.sh tests/system/test_fault_persistence.sh"
    "sil:rf-link      SIL_GS_ARGS='--mode never --seed 1' sil/sil.sh '$NODES python3 tests/system/test_rf_link.py'"
    "sil:rf-pass      SIL_GS_ARGS='--lat 37.9402 --lon -75.4664 --alt 10 --seed 1' sil/sil.sh '$NODES python3 tests/system/test_rf_pass.py'"
    "sil:failures     sil/sil.sh '$NODES python3 tests/system/test_failures.py'"
    "sil:hardware-twin sil/sil.sh '$NODES python3 tests/system/test_hardware_twin.py'"
    "sil:reference    D=\$SIL_LOG_DIR; SIL_INIT_RATES='2 -3 4' SIL_LOG_DIR=\$D/flatsat sil/sil.sh '$NODES python3 tests/system/test_reference_adcs.py --fsw flatsat'; SIL_INIT_RATES='2 -3 4' SIL_NO_BRIDGE=1 SIL_DOCKER_ARGS='--sysctl fs.mqueue.msg_max=10000 --ulimit rtprio=99 --cap-add=sys_nice' SIL_LOG_DIR=\$D/cfs sil/sil.sh 'python3 tests/system/test_reference_adcs.py --fsw cfs'; python3 tests/system/test_reference_adcs.py --compare \$D"
    "sil:orbit        sil/sil.sh '$NODES python3 tests/system/test_orbit.py'"
    "e2e:cosmos       tests/cosmos/test_cosmos_e2e.sh"
)

LONG="sil:orbit"         # only with "all" or by name (about 100 minutes)
BLOCKED="sil:reference"  # only by name: its test case is blocked (TC-15, NCR-015)

selected()
{
    [ $# -le 1 ] && [[ " $LONG $BLOCKED " != *" $1 "* ]] && return 0
    [ "${2:-}" = all ] && [[ " $BLOCKED " != *" $1 "* ]] && return 0
    [ $# -le 1 ] && return 1
    local w
    for w in "${@:2}"; do [[ $1 == *$w* ]] && return 0; done
    return 1
}

declare -a NAMES RESULTS TIMES
for s in "${STAGES[@]}"; do
    name=${s%% *}
    cmd=${s#* }
    selected "$name" "$@" || continue
    echo "==== $name"
    start=$(date +%s)
    export SIL_LOG_DIR=$LOG/${name/:/-}.d # SIL logs and recorded data of this stage
    (eval "$cmd") > "$LOG/${name/:/-}.log" 2>&1
    rc=$?
    NAMES+=("$name"); TIMES+=($(( $(date +%s) - start )))
    RESULTS+=($([ $rc -eq 0 ] && echo PASS || echo FAIL))
    grep -E "^(PASS|FAIL)|passed|failed" "$LOG/${name/:/-}.log" | tail -3 | sed 's/^/    /'
done

echo
printf "%-18s %-6s %s\n" STAGE RESULT TIME
failed=0
for i in "${!NAMES[@]}"; do
    printf "%-18s %-6s %ss\n" "${NAMES[$i]}" "${RESULTS[$i]}" "${TIMES[$i]}"
    [ "${RESULTS[$i]}" = PASS ] || failed=$((failed + 1))
done
echo "logs in $LOG"

# Machine-readable summary for the test report (tools/test_report.py)
python3 - "$LOG" "${NAMES[@]}" -- "${RESULTS[@]}" -- "${TIMES[@]}" <<'EOF'
import json, os, platform, sys, datetime
log, rest = sys.argv[1], sys.argv[2:]
i = rest.index("--"); names, rest = rest[:i], rest[i + 1:]
j = rest.index("--"); results, times = rest[:j], rest[j + 1:]
summary = {
    "started": os.path.basename(log)[4:], "finished": datetime.datetime.now().isoformat(timespec="seconds"),
    "host": f"{platform.node()} ({platform.system()} {platform.release()}, {os.cpu_count()} CPUs)",
    "commit": os.environ["SUT_COMMIT"], "dirty": bool(os.environ["SUT_DIRTY"]), "nos3_commit": os.environ["SUT_NOS3_COMMIT"],
    "stages": [{"name": n, "result": r, "seconds": int(t), "log": n.replace(":", "-") + ".log"}
               for n, r, t in zip(names, results, times)],
}
json.dump(summary, open(os.path.join(log, "summary.json"), "w"), indent=2)
EOF

# Test report of this run, in $LOG/report (tools/test_report.py runs in its own image: tools/report/Dockerfile)
if docker image inspect flatsat-report > /dev/null 2>&1 || docker build -q -t flatsat-report tools/report > /dev/null; then
    docker run --rm -v "$ROOT:$ROOT" -w "$ROOT" flatsat-report python tools/test_report.py "$LOG"
fi
exit $failed
