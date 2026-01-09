set(MODULE_BRIGHTNESS_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/task_brightness.c"
)

set(MODULE_BRIGHTNESS_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
)

set(MODULE_BRIGHTNESS_REQUIRES
    esp_adc
    esp_driver_ledc
    freertos
)
