set(MODULE_DISPLAY_SRCS
    "${CMAKE_CURRENT_LIST_DIR}/display.c"
    "${CMAKE_CURRENT_LIST_DIR}/task_display.c"
    "${CMAKE_CURRENT_LIST_DIR}/task_button_screen.c"
)

set(MODULE_DISPLAY_INCLUDE_DIRS
    "${CMAKE_CURRENT_LIST_DIR}"
)

set(MODULE_DISPLAY_REQUIRES
    st7735_driver
)
