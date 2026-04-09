set(component_srcs "ssd1306.c" "ssd1306_spi.c")
set(component_requires "driver esp_driver_spi esp_driver_i2c esp_driver_gpio")

# get IDF version for comparison
set(idf_version "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")

# Force legacy for compatibility
list(APPEND component_srcs "ssd1306_i2c_legacy.c")

idf_component_register(SRCS "${component_srcs}" PRIV_REQUIRES "${component_requires}" INCLUDE_DIRS ".")
