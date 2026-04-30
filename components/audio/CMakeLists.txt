set(srcs "codec/no_codec.c" "codec/es8311.c")

idf_component_register(
    SRCS
    ${srcs}
    INCLUDE_DIRS
    "include"
    PRIV_REQUIRES
    driver
    esp_driver_i2s
    esp_driver_i2c
    esp_driver_gpio
    freertos
    espressif__esp_codec_dev
    espressif__esp-sr
)
