cmake_minimum_required(VERSION 3.13)

# linux: software-in-the-loop executables and unit tests; pico: Raspberry Pi Pico 2 firmware (tools/pico/build.sh)
set(FLATSAT_PLATFORM linux CACHE STRING "Target platform: linux or pico")

if(FLATSAT_PLATFORM STREQUAL "pico")
    set(PICO_BOARD pico2 CACHE STRING "Pico SDK board")
    include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
    project(flatsat_firmware C CXX ASM)
    pico_sdk_init()
else()
    project(flatsat_firmware C)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Our code is built with every warning as an error; on the Pico only our sources, not the SDK's
set(FLATSAT_WARNINGS -Wall -Wextra -Wpedantic -Werror)
if(NOT FLATSAT_PLATFORM STREQUAL "pico")
    add_compile_options(${FLATSAT_WARNINGS})
endif()

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

set(OBC_SOURCES
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
    add_executable(obc ${OBC_SOURCES})
    target_include_directories(obc PRIVATE obc)
    target_link_libraries(obc obc_devices adcs_law fs_common fs_hal m)

    # Creates the SIL CAN bus (vcan0) inside the container
    add_executable(vcan_up tools/vcan_up.c)

    # Needs the SIL environment (sil/sil.sh), so it isn't registered with ctest
    add_executable(test_devices tests/sil/test_devices.c)
    target_link_libraries(test_devices obc_devices fs_common fs_hal m)
endif()

if(FLATSAT_PLATFORM STREQUAL "pico")
    foreach(lib fs_common obc_devices adcs_law)
        target_compile_options(${lib} PRIVATE ${FLATSAT_WARNINGS})
    endforeach()

    # OBC flight software on the Pico 2: the umbilical is the USB port (hal/pico/fs_hal_pico.c)
    add_executable(obc ${OBC_SOURCES} hal/pico/fs_hal_pico.c)
    set_source_files_properties(${OBC_SOURCES} PROPERTIES COMPILE_OPTIONS "${FLATSAT_WARNINGS}")
    set_source_files_properties(hal/pico/fs_hal_pico.c PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
    target_include_directories(obc PRIVATE obc)
    target_link_libraries(obc obc_devices adcs_law fs_common pico_stdlib hardware_watchdog hardware_uart)
    # The name the PC shows for the USB device. USB buffers of 4 KB instead of the SDK's 64 bytes: with 64, every
    # telemetry packet waited for the host to collect it 64 bytes at a time, blocking the OBC (NCR-018)
    target_compile_definitions(obc PRIVATE "USBD_PRODUCT=\"FlatSat OBC\"" "USBD_MANUFACTURER=\"HITL FlatSat\""
        CFG_TUD_CDC_TX_BUFSIZE=4096 CFG_TUD_CDC_RX_BUFSIZE=4096)
    # The C library's printf: the SDK's formats -0.0 and the smallest doubles as "nane-308" (NCR-017)
    pico_set_printf_implementation(obc compiler)
    pico_enable_stdio_usb(obc 1)
    pico_enable_stdio_uart(obc 0)
    pico_add_extra_outputs(obc) # obc.uf2: drag onto the Pico's USB drive
endif()


if(FLATSAT_PLATFORM STREQUAL "pico")
    # On-target diagnostics, flashed only by hand: number formatting on the RP2350 (NCR-017)
    add_executable(test_target_format tests/pico/test_target_format.c)
    target_link_libraries(test_target_format pico_stdlib)
    pico_enable_stdio_usb(test_target_format 1)
    pico_enable_stdio_uart(test_target_format 0)
    pico_add_extra_outputs(test_target_format)
    add_executable(test_target_format_newlib tests/pico/test_target_format.c)
    target_link_libraries(test_target_format_newlib pico_stdlib)
    pico_set_printf_implementation(test_target_format_newlib compiler)
    pico_enable_stdio_usb(test_target_format_newlib 1)
    pico_enable_stdio_uart(test_target_format_newlib 0)
    pico_add_extra_outputs(test_target_format_newlib)
endif()
