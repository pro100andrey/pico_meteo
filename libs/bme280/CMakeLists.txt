set(SRCS
    ./bme280.c
    ./bme280.h
)

add_library(bme280 ${SRCS})

target_link_libraries(bme280
    pico_stdlib
    hardware_i2c
)
