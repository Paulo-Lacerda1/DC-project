include("${CMAKE_CURRENT_LIST_DIR}/core/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/config/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/server/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/wifi/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/sensor/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/button/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/display/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/security/CMakeLists.txt")
include("${CMAKE_CURRENT_LIST_DIR}/brightness/CMakeLists.txt")

set(MAIN_SRCS
    ${MODULE_CORE_SRCS}
    ${MODULE_CONFIG_SRCS}
    ${MODULE_SERVER_SRCS}
    ${MODULE_WIFI_SRCS}
    ${MODULE_SENSOR_SRCS}
    ${MODULE_BUTTON_SRCS}
    ${MODULE_DISPLAY_SRCS}
    ${MODULE_SECURITY_SRCS}
    ${MODULE_BRIGHTNESS_SRCS}
)

set(MAIN_INCLUDE_DIRS
    ${MODULE_CORE_INCLUDE_DIRS}
    ${MODULE_CONFIG_INCLUDE_DIRS}
    ${MODULE_SERVER_INCLUDE_DIRS}
    ${MODULE_WIFI_INCLUDE_DIRS}
    ${MODULE_SENSOR_INCLUDE_DIRS}
    ${MODULE_BUTTON_INCLUDE_DIRS}
    ${MODULE_DISPLAY_INCLUDE_DIRS}
    ${MODULE_SECURITY_INCLUDE_DIRS}
    ${MODULE_BRIGHTNESS_INCLUDE_DIRS}
    "${CMAKE_CURRENT_LIST_DIR}/include"
    "${CMAKE_CURRENT_LIST_DIR}"
)

set(MAIN_REQUIRES
    ${MODULE_DISPLAY_REQUIRES}
    ${MODULE_BRIGHTNESS_REQUIRES}
)

idf_component_register(
    SRCS         ${MAIN_SRCS}
    INCLUDE_DIRS ${MAIN_INCLUDE_DIRS}
    REQUIRES     esp_driver_i2c esp_driver_gpio esp_driver_rmt esp_driver_spi esp_driver_sdspi esp_driver_ledc
                 esp_adc fatfs sdmmc json esp_event esp_netif esp_wifi nvs_flash esp_app_format
                 esp_timer esp_https_server mqtt mbedtls efuse
                 ${MAIN_REQUIRES}
    EMBED_TXTFILES "server/certs/servercert.pem" "server/certs/prvtkey.pem"
                   "server/html/index.html" "server/html/config_portal.html"
)
