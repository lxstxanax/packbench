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
set(CMAKE_PROJECT_NAME max17320_gui)

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
    # MAX17320 monitor -- kept outside the CubeMX tree so regenerating the
    # project can never touch it
    ${CMAKE_SOURCE_DIR}/../fw/max17320.c
    ${CMAKE_SOURCE_DIR}/../fw/max17320_monitor.c
    ${CMAKE_SOURCE_DIR}/../fw/max17320_provision.c
    ${CMAKE_SOURCE_DIR}/../fw/bms_io.c
    ${CMAKE_SOURCE_DIR}/../fw/bms_dashboard.c
    ${CMAKE_SOURCE_DIR}/../fw/bms_app.c
)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_SOURCE_DIR}/../fw
)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Current-sense shunt actually populated on the BMS board (R18,
    # MFC0603-R005FT5). Every current/capacity/power scale derives from it.
    MAX17320_RSENSE_MOHM=5

    # Enables the NVM commit path behind the '!' menu. The part allows 7
    # NVM writes for its entire life, so the commit still needs the word
    # BURN typed at the console and refuses below MAX17320_MIN_COMMIT_MV.
    MAX17320_I_KNOW_THIS_BURNS_NVM=1

    # Raises the four current-limit registers to the ceiling of the part's
    # own range at a 5 mOhm shunt: nIPrtTh1 = 0x7E80 (+10.08 A charge /
    # -10.24 A discharge) and nODSCTh = 0x0166 (OCTH +7.75 A, the real
    # charge ceiling). OCCP is 0x7E and not 0x7F on purpose -- see
    # ../fw/max17320_config.h, where these values live. This is only ever
    # stated here; max17320_config.h already defaults it to 1.
    MAX17320_MAX_CURRENT_LIMITS=1
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)
