# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

# Prefer the Pico VS Code extension's prebuilt PIOASM when installed. Register
# it before importing the SDK; otherwise the SDK uses its normal build fallback.
if(WIN32)
    set(pioasmExecutable "${USERHOME}/.pico-sdk/tools/${sdkVersion}/pioasm/pioasm/pioasm.exe")
else()
    set(pioasmExecutable "${USERHOME}/.pico-sdk/tools/${sdkVersion}/pioasm/pioasm/pioasm")
endif()
if(EXISTS "${pioasmExecutable}")
    add_executable(pioasm IMPORTED GLOBAL)
    set_property(TARGET pioasm PROPERTY IMPORTED_LOCATION "${pioasmExecutable}")
endif()

# Pull in PICO SDK (assuming it's already in environment)
include(pico_sdk_import.cmake)

project(spi_to_hdmi C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Single switch for diagnostic code and its USB serial transport. Keep this OFF
# for normal real-time operation; enable with -DSPI_TO_HDMI_DIAGNOSTICS=ON.
option(SPI_TO_HDMI_DIAGNOSTICS "Enable event-driven USB diagnostics" OFF)

pico_sdk_init()

add_executable(spi_to_hdmi
    src/main.c
    src/audio_profiles.c
    src/hstx_data_island_queue.c
    src/hstx_packet.c
    src/video_output.c
)

# Keep warnings scoped to this project's C sources rather than applying them
# globally to the Pico SDK and generated support libraries.
target_compile_options(spi_to_hdmi PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall;-Wextra;-Wshadow;-Wformat=2>
)

# Include directories
target_include_directories(spi_to_hdmi PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${CMAKE_CURRENT_LIST_DIR}/include
)

# Generate PIO headers
pico_generate_pio_header(spi_to_hdmi ${CMAKE_CURRENT_LIST_DIR}/src/spi_capture.pio)
pico_generate_pio_header(spi_to_hdmi ${CMAKE_CURRENT_LIST_DIR}/src/i2s_audio.pio)

# Pull in common dependencies
target_link_libraries(spi_to_hdmi
    pico_stdlib
    hardware_pio
    hardware_dma
    hardware_clocks
    pico_multicore
)

# Enable extra output formats
pico_add_extra_outputs(spi_to_hdmi)

# Enable USB stdio only when its diagnostic consumer is compiled in.
if(SPI_TO_HDMI_DIAGNOSTICS)
    target_compile_definitions(spi_to_hdmi PRIVATE
        ENABLE_DIAGNOSTICS=1
        PICO_STDIO_USB_STDOUT_TIMEOUT_US=1000
    )
    pico_enable_stdio_usb(spi_to_hdmi 1)
else()
    target_compile_definitions(spi_to_hdmi PRIVATE ENABLE_DIAGNOSTICS=0)
    pico_enable_stdio_usb(spi_to_hdmi 0)
endif()
pico_enable_stdio_uart(spi_to_hdmi 0)
