/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * esp_intr_alloc() on the PLIC. The source number is the PLIC interrupt id,
 * levels 1..7 of the flags select the PLIC priority.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_intr_alloc.h"
#include "soc/sun252i_f101_ll.h"
#include "freertos/FreeRTOS.h"

#define PLIC_BASE           0x10000000u
#define PLIC_PRIORITY(id)   (PLIC_BASE + 4u * (id))
#define PLIC_ENABLE(id)     (PLIC_BASE + 0x2000u + 4u * ((id) / 32))
#define PLIC_THRESHOLD      (PLIC_BASE + 0x200000u)
#define PLIC_CLAIM          (PLIC_BASE + 0x200004u)
#define PLIC_NDEV           192

struct intr_handle_data_t {
    intr_handler_t handler;
    void *arg;
    int source;
    int priority;
    bool in_use;
    bool enabled;
};

static struct intr_handle_data_t s_intr[PLIC_NDEV];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void plic_enable_source(int id, bool en)
{
    uint32_t r = PLIC_ENABLE(id);

    if (en) {
        F101_REG32(r) |= 1u << (id % 32);
    } else {
        F101_REG32(r) &= ~(1u << (id % 32));
    }
}

void esp_intr_f101_dispatch(void)
{
    uint32_t id;

    while ((id = F101_REG32(PLIC_CLAIM)) != 0) {
        if (id < PLIC_NDEV && s_intr[id].in_use && s_intr[id].handler) {
            s_intr[id].handler(s_intr[id].arg);
        } else if (id < PLIC_NDEV) {
            plic_enable_source(id, false);
        }
        F101_REG32(PLIC_CLAIM) = id;
    }
}

static void plic_init_once(void)
{
    static bool done;

    if (done) {
        return;
    }
    done = true;
    F101_REG32(PLIC_THRESHOLD) = 0;
}

esp_err_t esp_intr_alloc_intrstatus(int source, int flags, uint32_t intrstatusreg, uint32_t intrstatusmask,
                                    intr_handler_t handler, void *arg, intr_handle_t *ret_handle)
{
    (void)intrstatusreg;
    (void)intrstatusmask;

    if (source <= 0 || source >= PLIC_NDEV || handler == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    plic_init_once();

    portENTER_CRITICAL(&s_lock);
    struct intr_handle_data_t *h = &s_intr[source];
    if (h->in_use) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    int level = 0;
    for (int i = 1; i <= 7; i++) {
        if (flags & (1 << i)) {
            level = i;
            break;
        }
    }
    h->handler = handler;
    h->arg = arg;
    h->source = source;
    h->priority = level ? level : 1;
    h->in_use = true;
    h->enabled = !(flags & ESP_INTR_FLAG_INTRDISABLED);
    F101_REG32(PLIC_PRIORITY(source)) = h->priority;
    plic_enable_source(source, h->enabled);
    portEXIT_CRITICAL(&s_lock);

    if (ret_handle) {
        *ret_handle = h;
    }
    return ESP_OK;
}

esp_err_t esp_intr_alloc(int source, int flags, intr_handler_t handler, void *arg, intr_handle_t *ret_handle)
{
    return esp_intr_alloc_intrstatus(source, flags, 0, 0, handler, arg, ret_handle);
}

esp_err_t esp_intr_free(intr_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    portENTER_CRITICAL(&s_lock);
    plic_enable_source(handle->source, false);
    memset(handle, 0, sizeof(*handle));
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}

esp_err_t esp_intr_enable(intr_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->enabled = true;
    plic_enable_source(handle->source, true);
    return ESP_OK;
}

esp_err_t esp_intr_disable(intr_handle_t handle)
{
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    handle->enabled = false;
    plic_enable_source(handle->source, false);
    return ESP_OK;
}

int esp_intr_get_cpu(intr_handle_t handle)
{
    (void)handle;
    return 0;
}

int esp_intr_get_intno(intr_handle_t handle)
{
    return handle ? handle->source : -1;
}

esp_err_t esp_intr_set_in_iram(intr_handle_t handle, bool is_in_iram)
{
    (void)handle;
    (void)is_in_iram;
    return ESP_OK;
}

void esp_intr_noniram_disable(void)
{
}

void esp_intr_noniram_enable(void)
{
}

void esp_intr_enable_source(int inum)
{
    plic_enable_source(inum, true);
}

void esp_intr_disable_source(int inum)
{
    plic_enable_source(inum, false);
}

bool esp_intr_ptr_in_isr_region(void *ptr)
{
    (void)ptr;
    return true;
}

esp_err_t esp_intr_mark_shared(int intno, int cpu, bool is_in_iram)
{
    (void)intno; (void)cpu; (void)is_in_iram;
    return ESP_OK;
}

esp_err_t esp_intr_reserve(int intno, int cpu)
{
    (void)intno; (void)cpu;
    return ESP_OK;
}

esp_err_t esp_intr_dump(FILE *stream)
{
    for (int i = 1; i < PLIC_NDEV; i++) {
        if (s_intr[i].in_use) {
            fprintf(stream ? stream : stdout, "plic %d prio %d %s\n", i, s_intr[i].priority,
                    s_intr[i].enabled ? "enabled" : "disabled");
        }
    }
    return ESP_OK;
}
