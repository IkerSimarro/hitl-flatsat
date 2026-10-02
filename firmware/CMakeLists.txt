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
)
target_include_directories(obc_devices PUBLIC obc/devices)
target_link_libraries(obc_devices PUBLIC fs_common)

if(FLATSAT_PLATFORM STREQUAL "linux")
    add_library(fs_hal STATIC hal/linux/fs_hal_linux.c)
    target_include_directories(fs_hal PUBLIC common/include)
    target_link_libraries(fs_hal PUBLIC util) # openpty

    enable_testing()
    add_executable(test_common tests/test_common.c tests/fake_hal.c)
    target_link_libraries(test_common fs_common)
    add_test(NAME test_common COMMAND test_common)

    # OBC flight software, software-in-the-loop build
    add_executable(obc
        obc/obc_main.c
        obc/obc_cmd.c
        obc/obc_events.c
        obc/obc_mode.c
        obc/obc_sensors.c
        obc/obc_tlm.c
    )
    target_include_directories(obc PRIVATE obc)
    target_link_libraries(obc obc_devices fs_common fs_hal m)

    # Needs the SIL environment (sil/sil.sh), so it isn't registered with ctest
    add_executable(test_devices tests/sil/test_devices.c)
    target_link_libraries(test_devices obc_devices fs_common fs_hal m)
endif()
