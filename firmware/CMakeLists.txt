cmake_minimum_required(VERSION 3.13)

# linux: software-in-the-loop executables and unit tests; pico: RP2350 firmware (Phase 4)
set(FLATSAT_PLATFORM linux CACHE STRING "Target platform: linux or pico")

project(flatsat_firmware C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# The hil_link framing library is shared with the HIL bridge in the NOS3 fork
set(HIL_PROTOCOL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../nos3/components/hil_bridge/protocol)

add_library(fs_common STATIC
    common/src/fs_ccsds.c
    common/src/fs_time.c
    common/src/fs_sched.c
    common/src/fs_umbilical.c
    common/src/fs_persist.c
    common/src/fs_can.c
    common/src/fs_node.c
    common/src/fs_rf.c
    ${HIL_PROTOCOL_DIR}/hil_link.c
)
target_include_directories(fs_common PUBLIC common/include ${HIL_PROTOCOL_DIR})

# OBC drivers for the NOS3-simulated devices (ICD 8.3)
add_library(obc_devices STATIC
    obc/devices/dev_common.c
    obc/devices/dev_imu.c
    obc/devices/dev_mag.c
    obc/devices/dev_fss.c
    obc/devices/dev_css.c
    obc/devices/dev_st.c
    obc/devices/dev_gps.c
    obc/devices/dev_rw.c
    obc/devices/dev_eps.c
    obc/devices/dev_trq.c
    obc/devices/dev_radio.c
)
target_include_directories(obc_devices PUBLIC obc/devices)
target_link_libraries(obc_devices PUBLIC fs_common)

# ADCS control laws: pure maths, shared by the OBC and the closed-loop unit test
add_library(adcs_law STATIC obc/adcs/adcs_law.c)
target_include_directories(adcs_law PUBLIC obc/adcs)
target_link_libraries(adcs_law PUBLIC m)

if(FLATSAT_PLATFORM STREQUAL "linux")
    add_library(fs_hal STATIC hal/linux/fs_hal_linux.c hal/linux/sil_harness.c)
    target_include_directories(fs_hal PUBLIC common/include hal/linux)
    target_link_libraries(fs_hal PUBLIC util rt) # openpty, shm_open

    # ADCS and EPS nodes (their Linux device layers talk to the SIL harness and plant model)
    add_executable(adcs_node nodes/adcs/adcs_main.c nodes/adcs/wheel_ctrl.c hal/linux/dev_motor_linux.c)
    target_include_directories(adcs_node PRIVATE nodes/adcs)
    target_link_libraries(adcs_node fs_common fs_hal m)

    add_executable(eps_node nodes/eps/eps_main.c hal/linux/dev_power_linux.c)
    target_include_directories(eps_node PRIVATE nodes/eps)
    target_link_libraries(eps_node fs_common fs_hal)

    # Physics of the FlatSat hardware for software-in-the-loop
    add_executable(flatsat_plant sil_plant/flatsat_plant.c)
    target_link_libraries(flatsat_plant fs_hal m)

    enable_testing()
    add_executable(test_common tests/test_common.c tests/fake_hal.c)
    target_link_libraries(test_common fs_common)
    add_test(NAME test_common COMMAND test_common)
    add_executable(test_wheel_ctrl tests/test_wheel_ctrl.c nodes/adcs/wheel_ctrl.c)
    target_include_directories(test_wheel_ctrl PRIVATE nodes/adcs)
    target_link_libraries(test_wheel_ctrl m)
    add_test(NAME test_wheel_ctrl COMMAND test_wheel_ctrl)
    add_executable(test_adcs tests/test_adcs.c)
    target_link_libraries(test_adcs adcs_law)
    add_test(NAME test_adcs COMMAND test_adcs)

    # OBC flight software, software-in-the-loop build
    add_executable(obc
        obc/obc_main.c
        obc/obc_cmd.c
        obc/obc_events.c
        obc/obc_mode.c
        obc/obc_sensors.c
        obc/obc_tlm.c
        obc/obc_can.c
        obc/obc_adcs.c
        obc/obc_comms.c
    )
    target_include_directories(obc PRIVATE obc)
    target_link_libraries(obc obc_devices adcs_law fs_common fs_hal m)

    # Creates the SIL CAN bus (vcan0) inside the container
    add_executable(vcan_up tools/vcan_up.c)

    # Needs the SIL environment (sil/sil.sh), so it isn't registered with ctest
    add_executable(test_devices tests/sil/test_devices.c)
    target_link_libraries(test_devices obc_devices fs_common fs_hal m)
endif()
