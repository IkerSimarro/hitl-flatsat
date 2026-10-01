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
    ${HIL_PROTOCOL_DIR}/hil_link.c
)
target_include_directories(fs_common PUBLIC common/include ${HIL_PROTOCOL_DIR})

if(FLATSAT_PLATFORM STREQUAL "linux")
    add_library(fs_hal STATIC hal/linux/fs_hal_linux.c)
    target_include_directories(fs_hal PUBLIC common/include)
    target_link_libraries(fs_hal PUBLIC util) # openpty

    enable_testing()
    add_executable(test_common tests/test_common.c tests/fake_hal.c)
    target_link_libraries(test_common fs_common)
    add_test(NAME test_common COMMAND test_common)
endif()
