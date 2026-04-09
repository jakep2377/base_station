idf_component_register(SRCS "salt_base_station.c" "display.c"
                    INCLUDE_DIRS "."
                    REQUIRES mdns driver esp_driver_gpio esp_driver_i2c esp_wifi nvs_flash esp_http_server esp_event freertos ra01s ssd1306)
