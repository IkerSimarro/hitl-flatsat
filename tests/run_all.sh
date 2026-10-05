#!/bin/bash
#
# Regression campaign: runs every test stage in order and prints a summary table. Needs Docker, the NOS3
# image and a built NOS3 (make config && make, scripts/gsw/gsw_cosmos_build.sh). Takes about 35 minutes.
#
#   tests/run_all.sh            all stages
#   tests/run_all.sh unit sil   only the stages whose name contains one of the words
#
ROOT=$( cd -- "$( dirname -- "${BASH_SOURCE[0]}" )/.." &> /dev/null && pwd )
cd "$ROOT" || exit 1
IMAGE=ivvitc/nos3-64:20260619
LOG=$ROOT/tests/logs/run-$(date +%Y%m%d-%H%M%S)
mkdir -p "$LOG"
NODES='sil/start_nodes.sh && (firmware/build/obc --umb-pty $SIL_UMB_PTY > $SIL_LOG_DIR/obc.log 2>&1 &) &&'

STAGES=(
    "unit:icd         python3 tools/icd_gen.py --check && python3 -m unittest discover -s tests/unit"
    "unit:cosmos-defs tests/cosmos/check_defs.sh && python3 tests/cosmos/check_screens.py"
    "unit:firmware    docker run --rm -v $ROOT:$ROOT -w $ROOT $IMAGE bash -c 'cmake -S firmware -B firmware/build > /dev/null && make -C firmware/build -j4 > /dev/null && firmware/build/test_common && firmware/build/test_wheel_ctrl && firmware/build/test_adcs'"
    "e2e:bridge       cd nos3 && docker run --rm -v \$PWD:\$PWD --add-host nos-engine-server:127.0.0.1 --add-host sc01-nos-engine-server:127.0.0.1 $IMAGE \$PWD/components/hil_bridge/support/e2e_test.sh"
    "sil:devices      SIL_INIT_RATES='2 -3 4' sil/sil.sh firmware/build/test_devices --umb-pty '\$SIL_UMB_PTY' --can none"
    "sil:adcs         SIL_INIT_RATES='2 -3 4' sil/sil.sh '$NODES python3 tests/system/test_adcs.py'"
    "sil:umbilical    sil/sil.sh '$NODES python3 tests/system/test_obc_umbilical.py'"
    "sil:can-nodes    sil/sil.sh '$NODES python3 tests/system/test_can_nodes.py'"
    "sil:faults       sil/sil.sh tests/system/test_fault_persistence.sh"
    "sil:rf-link      SIL_GS_ARGS='--mode never --seed 1' sil/sil.sh '$NODES python3 tests/system/test_rf_link.py'"
    "sil:rf-pass      SIL_GS_ARGS='--lat 37.9402 --lon -75.4664 --alt 10 --seed 1' sil/sil.sh '$NODES python3 tests/system/test_rf_pass.py'"
    "e2e:cosmos       tests/cosmos/test_cosmos_e2e.sh"
)

selected()
{
    [ $# -le 1 ] && return 0
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
exit $failed
