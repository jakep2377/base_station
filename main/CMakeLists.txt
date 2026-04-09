idf_component_register(SRCS "salt_base_station.c" "display.c"
                    INCLUDE_DIRS "."
<<<<<<< HEAD
                    REQUIRES mdns driver esp_driver_gpio esp_driver_i2c esp_wifi nvs_flash esp_http_server esp_event freertos ra01s ssd1306)
=======
                    REQUIRES mdns driver esp_driver_gpio esp_wifi nvs_flash esp_http_server esp_event freertos esp_http_client esp-tls ra01s)
>>>>>>> a35027d7654c6a0f56e5bba4a608925e6073f0eb
