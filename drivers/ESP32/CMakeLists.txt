# The following four lines of boilerplate have to be in your project's CMakeLists
# in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.5)

set(MAIN_SRCS main.c driver.c nvs.c esp32-hal-uart.c bluetooth.c wifi.c TCPStream.c sdcard.c keypad.c
 GRBL/grbllib.c
 GRBL/coolant_control.c
 GRBL/eeprom_emulate.c
 GRBL/gcode.c
 GRBL/limits.c
 GRBL/motion_control.c
 GRBL/nuts_bolts.c
 GRBL/override.c
 GRBL/planner.c
 GRBL/protocol.c
 GRBL/report.c
 GRBL/settings.c
 GRBL/sleep.c
 GRBL/spindle_control.c
 GRBL/state_machine.c
 GRBL/stepper.c
 GRBL/system.c
)

#include_directories("D:/Projects/esp/esp-idf/components/freertos/include")

#file(GLOB GRBL_SOURCE "GRBL/*.c")
    
#list (APPEND GRBL_SOURCE MAIN_SRCS)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(grbl)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#add_subdirectory (GRBL)
