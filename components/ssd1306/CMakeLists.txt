set(component_srcs "ssd1306.c" "ssd1306_spi.c")
set(component_requires driver esp_driver_spi esp_driver_i2c esp_driver_gpio)

if(IDF_VERSION_MAJOR GREATER_EQUAL 5 AND NOT CONFIG_LEGACY_DRIVER)
    list(APPEND component_srcs "ssd1306_i2c_new.c")
else()
    list(APPEND component_srcs "ssd1306_i2c_legacy.c")
endif()

idf_component_register(SRCS ${component_srcs} REQUIRES ${component_requires} INCLUDE_DIRS ".")
