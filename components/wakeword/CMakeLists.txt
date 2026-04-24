idf_component_register(SRCS "wakeword.c"
                       INCLUDE_DIRS "include"
                       REQUIRES i2s freertos
                       PRIV_REQUIRES espressif__esp-sr)
