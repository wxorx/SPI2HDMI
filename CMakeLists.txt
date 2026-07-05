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

# Pull in PICO SDK (assuming it's already in environment)
include(pico_sdk_import.cmake)

project(spi_to_hdmi C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# --- BYPASS PIOASM BUILD ---
# Force the SDK to use the pre-compiled pioasm tool instead of building it from scratch.
if(NOT TARGET pioasm)
    add_executable(pioasm IMPORTED GLOBAL)
    set_property(TARGET pioasm PROPERTY IMPORTED_LOCATION "C:/Users/dynam/.pico-sdk/tools/2.2.0/pioasm/pioasm/pioasm.exe")
endif()
# ---------------------------

pico_sdk_init()

add_executable(spi_to_hdmi
    src/main.c
    src/hstx_data_island_queue.c
    src/hstx_packet.c
    src/video_output.c
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

# Enable USB stdio to expose a virtual COM port over USB
pico_enable_stdio_usb(spi_to_hdmi 0)
pico_enable_stdio_uart(spi_to_hdmi 0)
