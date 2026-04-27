file(GLOB_RECURSE SRC_FILES "*.c")

idf_component_register(SRCS ${SRC_FILES}
                       INCLUDE_DIRS "include"
                       REQUIRES driver esp_driver_i2s esp_driver_gpio freertos
                       PRIV_REQUIRES espressif__esp-sr)
