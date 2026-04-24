idf_component_register(SRCS "wakeword.c"
                       INCLUDE_DIRS "include"
                       REQUIRES audio freertos
                       PRIV_REQUIRES espressif__esp-sr)
