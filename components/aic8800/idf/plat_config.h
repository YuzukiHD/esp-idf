#ifndef __PLAT_CONFIG_H__
#define __PLAT_CONFIG_H__

void platform_config_init(void);
int aic_wifi_reset_power(void);
void platform_config_destory(void);
int platform_get_sdc_index(void);
int platform_set_regon_en(int en);
/* level of the wakeup line, -1 when it is not wired */
int platform_get_wakeup_level(void);

#endif
