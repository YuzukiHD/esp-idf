#ifndef AIC_IDF_EFUSE_H_
#define AIC_IDF_EFUSE_H_

#include "esp_efuse.h"
#include "esp_efuse_table.h"

/* the 128 bit chip identifier the MAC address is derived from; stays zero when unreadable */
static inline int hal_efuse_get_chipid(unsigned char *buf)
{
    return esp_efuse_read_field_blob(ESP_EFUSE_CHIP_ID, buf, 128) == 0 ? 0 : -1;
}

#endif
