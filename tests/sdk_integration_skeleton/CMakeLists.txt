cmake_minimum_required(VERSION 3.16)
project(zinf_skeleton C)

set(CMAKE_C_STANDARD 11)

# --------------------------------------------------------------------------
# Step 1: Point ZINF_ROOT at the ZINF repository root.
# --------------------------------------------------------------------------
set(ZINF_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.."
    CACHE PATH "Path to the ZINF repository root")

# --------------------------------------------------------------------------
# Step 2: Add your generated driver source below (from Studio SDK Generator).
# --------------------------------------------------------------------------
# Example: set(SENSOR_DRIVER my_sensor_driver.c)
set(SENSOR_DRIVER ""
    CACHE FILEPATH "Generated sensor driver .c file")

add_executable(zinf_app
    main.c
    # TODO: Add your generated zinf_main.c here
    # TODO: Add ${SENSOR_DRIVER} here

    # ZINF core sources
    ${ZINF_ROOT}/src/core/api/api.c
    ${ZINF_ROOT}/src/core/storage/storage.c
    ${ZINF_ROOT}/src/core/helper/helper.c
    ${ZINF_ROOT}/src/config/config.c

    # Driver: choose one of the following
    # For desktop sandbox testing:
    ${ZINF_ROOT}/src/drivers/mock/ram_driver.c
    ${ZINF_ROOT}/src/platform/platform_linux.c
    # For RP2350 MCU: use sd_driver.c + platform_embedded.c instead
)

target_include_directories(zinf_app PRIVATE
    ${ZINF_ROOT}/src/core/api
    ${ZINF_ROOT}/src/core/storage
    ${ZINF_ROOT}/src/core/helper
    ${ZINF_ROOT}/src/config
    ${ZINF_ROOT}/src/drivers/mock
    ${ZINF_ROOT}/src/drivers/embedded
    ${CMAKE_CURRENT_LIST_DIR}
)
