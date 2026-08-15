cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME motor_uart_pwm_nucleo)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Src/steering.c
    Core/Src/motor_dc.c
    Core/Src/i2c_bus.c
    # MAX17320 pack monitor -- shared with the standalone bench monitor,
    # kept in its own tree so both projects build the same sources.
    ${CMAKE_SOURCE_DIR}/../fw/max17320.c
    ${CMAKE_SOURCE_DIR}/../fw/max17320_monitor.c
    ${CMAKE_SOURCE_DIR}/../fw/bms_io.c
    ${CMAKE_SOURCE_DIR}/../fw/bms_dashboard.c
    ${CMAKE_SOURCE_DIR}/../fw/bms_app.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    Core/Inc
    ${CMAKE_SOURCE_DIR}/../fw
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # The pack monitor shares this loop with motor control, so the modes
    # that block for seconds (NVM provisioning, trip capture, the weak-bus
    # probe) are compiled out and the register poll is spread over calls.
    BMS_REALTIME_HOST=1

    # Sense resistor on the BMS board (R18, MFC0603-R005FT5).
    MAX17320_RSENSE_MOHM=5

    # The gauge is on I2C3 here: PC8 = SCL, PC9 = SDA.
    BMS_SCL_PORT=GPIOC
    BMS_SCL_PIN=GPIO_PIN_8
    BMS_SDA_PORT=GPIOC
    BMS_SDA_PIN=GPIO_PIN_9
    "BMS_SCL_NAME=\"PC8 (CN10-2)\""
    "BMS_SDA_NAME=\"PC9 (CN10-1)\""
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
