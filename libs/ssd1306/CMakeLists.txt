set(SRCS
    ./ssd1306.c
    ./ssd1306.h
)

add_library(ssd1306 ${SRCS})

target_link_libraries(ssd1306
    pico_stdlib
    hardware_i2c
)
