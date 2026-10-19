# The fuses of the sun252i-f101 target: 512 bits behind the SID controller, read only. The eFuse API
# (esp_efuse.h) reads them as one block; writing is refused.
idf_component_register(SRCS "sun252i-f101/efuse_f101.c"
                       INCLUDE_DIRS "../include" "include"
                       PRIV_REQUIRES soc)
