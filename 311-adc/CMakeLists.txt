cmake_minimum_required(VERSION 3.15)

set(PICO_BOARD pico)
set(PICO_PLATFORM rp2040)
include(pico_sdk_import.cmake)
project(311_adc)
pico_sdk_init()

add_compile_options(-Wall)
add_subdirectory(../libs ${CMAKE_BINARY_DIR}/libs)
# Выключатель профилирования: заменить 1 на 0 в этой строке.
target_compile_definitions(profiling PUBLIC PROFILING_ENABLED=1)

add_library(platform STATIC platform/platform.c)
target_include_directories(platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(platform PRIVATE pico_stdlib hardware_adc)

add_library(logic STATIC
    logic/api/api.c
    logic/api/api-callbacks.c
    logic/led-task/led-task.c
    logic/button-task/button-task.c
    logic/pi-task/pi-task.c
    logic/telemetry-task/telemetry-task.c
)
target_include_directories(logic PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/logic)
target_link_libraries(logic PUBLIC stdio_text_protocol
    PRIVATE platform systime profiling device logging)

add_executable(${PROJECT_NAME} main.c)
target_link_libraries(${PROJECT_NAME} PRIVATE
    logic platform systime profiling logging pico_stdlib)
pico_enable_stdio_usb(${PROJECT_NAME} 1)
pico_enable_stdio_uart(${PROJECT_NAME} 0)
pico_add_extra_outputs(${PROJECT_NAME})
