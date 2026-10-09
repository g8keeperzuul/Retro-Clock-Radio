FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.c)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "." "${CMAKE_SOURCE_DIR}/include"
    REQUIRES nvs_flash esp_wifi esp_netif esp_event esp_app_format lwip
             esp_driver_spi esp_driver_gpio
             esp_http_client mbedtls json esp_timer esp_driver_i2c esp_driver_rmt mqtt
)
