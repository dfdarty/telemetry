# Telemetry — an American Rocketry Challenge launch-day logbook for the
# FREE-WILi 2: flight stopwatch, ARC scoring, flight log on the SD card and
# a mass trim estimate. Builds for the emulator and, with the Pico SDK and
# Arm GCC, into a UF2 for the board.
add_executable(telemetry main.c)
target_link_libraries(telemetry freewili2_bsp onewili_fwgui)
# A PSRAM app: code and data in PSRAM, so the stack has the SRAM. The board
# clock calls are OneWili text commands, which need about 10 KB of stack.
fw2_psram_app(telemetry
    POWER_ZONES DISPLAY SDCARD SENSORS
    VERSION 001
    DESCRIPTION "ARC launch logbook: flight timer, scoring, mass trim")
