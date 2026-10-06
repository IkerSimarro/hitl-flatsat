#!/bin/bash
#
# Builds the firmware (Linux) and runs its unit tests, printing test procedure step results (TC-02,
# docs/test/verification.yaml). Runs in the NOS3 build image (tests/run_all.sh stage unit:firmware).
#
cd "$(dirname "$0")/../.." || exit 1
if ! { cmake -S firmware -B firmware/build > /dev/null && make -C firmware/build -j4 > /dev/null; }; then
    echo "FAIL [TC-02.1] firmware build"
    exit 1
fi
rc=0
run()
{
    if "firmware/build/$2"; then echo "PASS [$1] $3"; else echo "FAIL [$1] $3"; rc=1; fi
}
run TC-02.1 test_common "common flight libraries"
run TC-02.2 test_wheel_ctrl "wheel speed loop in closed loop"
run TC-02.3 test_adcs "ADCS laws in closed loop"
exit $rc
