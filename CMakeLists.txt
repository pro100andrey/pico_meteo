cmake_minimum_required(VERSION 3.12)

# Pull in SDK (must be before project)
include(pico_sdk_import.cmake)

project(pico-meteo C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_C_FLAGS_DEBUG "-g -DDEBUG")
set(PICO_BOARD pico_w)
set(HOSTNAME "pico-meteo")
set(WIFI_SSID "...")
set(WIFI_PASSWORD "...")

if (PICO_SDK_VERSION_STRING VERSION_LESS "1.5.0")
    message(FATAL_ERROR "Raspberry Pi Pico SDK version 1.5.0 (or later) required. Your version is ${PICO_SDK_VERSION_STRING}")
endif()

pico_sdk_init()

add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/libs/ssd1306)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/libs/bme280)
add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/submodules/picow_http)

# If NTP_SERVER is defined in the cmake invocation, pass in its value
# to the preprocessor. See main.c
if (DEFINED NTP_SERVER)
	add_compile_definitions(NTP_SERVER=\"${NTP_SERVER}\")
endif()

# Optionally override the PicoW default hostname.
if (DEFINED HOSTNAME)
	add_compile_definitions(CYW43_HOST_NAME=\"${HOSTNAME}\")
endif()

set(SRCS
    ${CMAKE_CURRENT_LIST_DIR}/src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/src/utils.c
	${CMAKE_CURRENT_LIST_DIR}/src/handlers.c
	${CMAKE_CURRENT_LIST_DIR}/src/handlers.h
	${CMAKE_CURRENT_LIST_DIR}/submodules/picow_http/etc/lwipopts.h
)

# Include paths
# Must include the path at which lwipopts.h and is located.
set(INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/libs/ssd1306
    ${CMAKE_CURRENT_LIST_DIR}/libs/bme280
	${CMAKE_CURRENT_LIST_DIR}/submodules/picow_http/etc
)

# Static WWW resources to be embedded for the HTTP server.
set(WWWSRCS
	${CMAKE_CURRENT_LIST_DIR}/www/index.html
	${CMAKE_CURRENT_LIST_DIR}/www/img/favicon.png
	${CMAKE_CURRENT_LIST_DIR}/www/style.css
	${CMAKE_CURRENT_LIST_DIR}/www/main.js
)

set(LIBS
    pico_stdio
	pico_stdlib
	pico_multicore
	hardware_adc
	hardware_irq
	hardware_sync
    ssd1306
    bme280
)

add_executable(pico-meteo ${SRCS})

# PICOW_HTTPS indicates TLS support (used for binary info).
target_compile_definitions(pico-meteo PRIVATE
	WIFI_SSID=\"${WIFI_SSID}\"
	WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
	PICOW_HTTPS=0
)

target_include_directories(pico-meteo PRIVATE ${INCLUDES})

target_link_libraries(pico-meteo
    picow_http
	pico_cyw43_arch_lwip_poll
    ${LIBS}
)

picow_http_gen_handlers(pico-meteo
	${CMAKE_CURRENT_SOURCE_DIR}/www/www.yaml
	${CMAKE_CURRENT_SOURCE_DIR}/www
	${WWWSRCS}
)

pico_enable_stdio_usb(pico-meteo 1)
pico_enable_stdio_uart(pico-meteo 1)
pico_add_extra_outputs(pico-meteo)
