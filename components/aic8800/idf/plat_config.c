/*
 * Platform side of the AIC8800: the power pin and the host slot the chip is wired to, both given by the
 * board with aic8800_wifi_register().
 */
#include "driver/gpio.h"
#include "aic8800.h"
#include "plat_config.h"
#include "rtos_al.h"
#include "aic_log.h"

extern const aic8800_config_t *aic8800_idf_config(void);

static int is_init;

int platform_get_wakeup_level(void)
{
    int pin = aic8800_idf_config()->wakeup_gpio;

    return pin >= 0 ? gpio_get_level(pin) : -1;
}

/* power off, then on; the chip needs time to boot before it answers on the bus */
int aic_wifi_reset_power(void)
{
    platform_config_init();
    gpio_set_level(aic8800_idf_config()->reg_on_gpio, 0);
    rtos_msleep(100);
    gpio_set_level(aic8800_idf_config()->reg_on_gpio, 1);
    rtos_msleep(300);
    AIC_LOG_PRINTF("aic_wifi_reset_power done\n");
    return 0;
}

void platform_config_init(void)
{
    const aic8800_config_t *c = aic8800_idf_config();

    if (is_init) {
        return;
    }
    /* gpio_config() masks are 64 bit: the pins of PC..PF need gpio_set_direction() */
    gpio_set_direction(c->reg_on_gpio, GPIO_MODE_OUTPUT);
    gpio_set_level(c->reg_on_gpio, 0);
    if (c->wakeup_gpio >= 0) {
        gpio_set_direction(c->wakeup_gpio, GPIO_MODE_INPUT);
    }
    is_init = 1;
}

void platform_config_destory(void)
{
    if (is_init) {
        gpio_set_level(aic8800_idf_config()->reg_on_gpio, 0);
    }
    is_init = 0;
}

int platform_get_sdc_index(void)
{
    return aic8800_idf_config()->sdmmc_slot;
}

int platform_set_regon_en(int en)
{
    platform_config_init();
    return gpio_set_level(aic8800_idf_config()->reg_on_gpio, en ? 1 : 0);
}
