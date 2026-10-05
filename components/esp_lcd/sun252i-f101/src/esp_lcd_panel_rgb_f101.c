// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * RGB LCD panel of the esp_lcd API (esp_lcd_panel_rgb.h) on the display
 * engine: one UI plane scans out an RGB565 frame buffer in PSRAM. The panel
 * timing and the pins belong to the board description of the display stack, so
 * the timing and gpio members of esp_lcd_rgb_panel_config_t are only checked
 * (the resolution) and not used to program the pipeline.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "soc/sun252i_f101_ll.h"
#include "esp_lcd_panel_rgb.h"
#include "esp_lcd_panel_interface.h"

#include <hal/display/display_engine.h>
#include <dpy/dpy_os.h>

#include "f101_cache.h"

static const char *TAG = "lcd.rgb";

#define FB_PLANE        4          /* UI channel 0, layer 0 */
#define BPP             2
#define MBUS_BASE       0x03102000u

typedef struct {
    esp_lcd_panel_t base;
    uint32_t h_res;
    uint32_t v_res;
    size_t stride;                  /* bytes per line */
    size_t fb_size;
    size_t num_fbs;
    uint8_t *fbs[ESP_RGB_LCD_PANEL_MAX_FB_NUM];
    bool fb_owned[ESP_RGB_LCD_PANEL_MAX_FB_NUM];
    size_t cur_fb;                  /* frame buffer being scanned out */
    int brightness;
    bool refresh_on_demand;
    bool inited;
    bool frame_done_pending;
    esp_lcd_rgb_panel_event_callbacks_t cbs;
    void *user_ctx;
    TaskHandle_t vsync_task;
    volatile bool vsync_stop;
} f101_rgb_panel_t;

static f101_rgb_panel_t *s_panel;       /* the display engine drives one panel */

/* display engine at top MBUS priority, the CPU capped at 500 MB/s so scan-out is never starved */
static void mbus_setup(void)
{
    uint32_t msc = MBUS_BASE + 0x210 + 16 * 0x10;       /* master 16: display engine */
    uint32_t bwlr = MBUS_BASE + 0x218 + 39 * 0x10;      /* master 39: RISC-V */

    F101_REG32(msc) = (F101_REG32(msc) & ~(3u << 2)) | (3u << 2);
    F101_REG32(bwlr) = (F101_REG32(bwlr) & ~((0xfffu << 16) | (1u << 31))) | ((256u * 500u / 252u) << 16) | (1u << 31);
}

static esp_err_t show_framebuffer(f101_rgb_panel_t *p, const uint8_t *fb)
{
    struct display_pipeline_state st;
    struct display_plane_state *pl;

    display_pipeline_state_init(&st);
    st.plane_count = 0;
    pl = &st.planes[st.plane_count++];
    memset(pl, 0, sizeof(*pl));
    pl->enable = true;
    pl->plane_id = FB_PLANE;
    pl->alpha = 0xff;
    pl->blend_mode = DISPLAY_BLEND_NONE;
    pl->framebuffer.address = (uintptr_t)fb;
    pl->framebuffer.plane_address[0] = (uintptr_t)fb;
    pl->framebuffer.plane_stride[0] = p->stride;
    pl->framebuffer.plane_count = 1;
    pl->framebuffer.format = DISPLAY_FORMAT_RGB565;
    pl->framebuffer.width = p->h_res;
    pl->framebuffer.height = p->v_res;
    pl->framebuffer.stride = p->stride;
    pl->destination.width = p->h_res;
    pl->destination.height = p->v_res;
    return display_submit(&st) == 0 ? ESP_OK : ESP_FAIL;
}

/* vsync and frame buffer completion events; the callbacks run in this task, not in an isr */
static void vsync_task(void *arg)
{
    f101_rgb_panel_t *p = arg;

    while (!p->vsync_stop) {
        if (display_wait_vsync(100) != 0) {
            continue;
        }
        esp_lcd_rgb_panel_event_data_t ev = {};
        if (p->frame_done_pending) {
            p->frame_done_pending = false;
            if (p->cbs.on_frame_buf_complete) {
                p->cbs.on_frame_buf_complete((esp_lcd_panel_handle_t)p, &ev, p->user_ctx);
            }
        }
        if (p->cbs.on_vsync) {
            p->cbs.on_vsync((esp_lcd_panel_handle_t)p, &ev, p->user_ctx);
        }
    }
    p->vsync_task = NULL;
    vTaskDelete(NULL);
}

static esp_err_t panel_rgb_del(esp_lcd_panel_t *panel)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    if (p->vsync_task) {
        p->vsync_stop = true;
        while (p->vsync_task) {
            vTaskDelay(1);
        }
    }
    display_blank(true);
    for (size_t i = 0; i < p->num_fbs; i++) {
        if (p->fb_owned[i]) {
            free(p->fbs[i]);
        }
    }
    s_panel = NULL;
    free(p);
    return ESP_OK;
}

static esp_err_t panel_rgb_reset(esp_lcd_panel_t *panel)
{
    return ESP_OK;
}

static esp_err_t panel_rgb_init(esp_lcd_panel_t *panel)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(!p->inited, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    ESP_RETURN_ON_ERROR(show_framebuffer(p, p->fbs[0]), TAG, "cannot show the frame buffer");
    p->cur_fb = 0;
    p->inited = true;
    display_blank(false);
    display_set_backlight(p->brightness);
    return ESP_OK;
}

static esp_err_t panel_rgb_draw_bitmap(esp_lcd_panel_t *panel, int x_start, int y_start, int x_end, int y_end, const void *color_data)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(p->inited, ESP_ERR_INVALID_STATE, TAG, "panel not initialized");
    ESP_RETURN_ON_FALSE(color_data && x_start >= 0 && y_start >= 0 && x_end > x_start && y_end > y_start &&
                        (uint32_t)x_end <= p->h_res && (uint32_t)y_end <= p->v_res, ESP_ERR_INVALID_ARG, TAG, "invalid area");

    /* a bitmap that is one of the frame buffers is not copied, it becomes the scanned out one */
    for (size_t i = 0; i < p->num_fbs; i++) {
        if (color_data == p->fbs[i] && x_start == 0 && y_start == 0 && (uint32_t)x_end == p->h_res && (uint32_t)y_end == p->v_res) {
            f101_dcache_clean(p->fbs[i], p->fb_size);
            if (i != p->cur_fb) {
                ESP_RETURN_ON_ERROR(show_framebuffer(p, p->fbs[i]), TAG, "cannot flip");
                p->cur_fb = i;
                p->frame_done_pending = true;
            }
            if (p->cbs.on_color_trans_done) {
                esp_lcd_rgb_panel_event_data_t ev = {};
                p->cbs.on_color_trans_done(panel, &ev, p->user_ctx);
            }
            return ESP_OK;
        }
    }

    size_t row = (size_t)(x_end - x_start) * BPP;
    const uint8_t *src = color_data;
    uint8_t *fb = p->fbs[p->cur_fb];

    for (int y = y_start; y < y_end; y++, src += row) {
        uint8_t *dst = fb + (size_t)y * p->stride + (size_t)x_start * BPP;
        memcpy(dst, src, row);
        f101_dcache_clean(dst, row);
    }
    if (p->cbs.on_color_trans_done) {
        esp_lcd_rgb_panel_event_data_t ev = {};
        p->cbs.on_color_trans_done(panel, &ev, p->user_ctx);
    }
    return ESP_OK;
}

static esp_err_t panel_rgb_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    return display_blank(!on_off) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t panel_rgb_set_brightness(esp_lcd_panel_t *panel, int brightness)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(brightness >= 0 && brightness <= 255, ESP_ERR_INVALID_ARG, TAG, "brightness is 0..255");
    p->brightness = brightness;
    return display_set_backlight(brightness) == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t *cfg, esp_lcd_panel_handle_t *ret_panel)
{
    struct display_mode mode;
    f101_rgb_panel_t *p;
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(cfg && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!s_panel, ESP_ERR_INVALID_STATE, TAG, "the display engine drives one panel");
    ESP_RETURN_ON_FALSE(cfg->in_color_format == LCD_COLOR_FMT_RGB565 || cfg->in_color_format == 0, ESP_ERR_NOT_SUPPORTED, TAG, "only RGB565 frame buffers");
    ESP_RETURN_ON_FALSE(cfg->num_fbs <= ESP_RGB_LCD_PANEL_MAX_FB_NUM, ESP_ERR_INVALID_ARG, TAG, "too many frame buffers");
    ESP_RETURN_ON_FALSE(!cfg->flags.no_fb, ESP_ERR_NOT_SUPPORTED, TAG, "a frame buffer is required");

    p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no memory for the panel");

    mbus_setup();
    if (display_probe() || display_wait_ready(5000) || display_get_mode(&mode)) {
        ESP_LOGE(TAG, "display pipeline failed to come up");
        free(p);
        return ESP_FAIL;
    }
    if ((cfg->timings.h_res && cfg->timings.h_res != mode.width) || (cfg->timings.v_res && cfg->timings.v_res != mode.height)) {
        ESP_LOGE(TAG, "panel is %ux%u, the configuration asks for %ux%u", (unsigned)mode.width, (unsigned)mode.height,
                 (unsigned)cfg->timings.h_res, (unsigned)cfg->timings.v_res);
        free(p);
        return ESP_ERR_INVALID_ARG;
    }

    p->h_res = mode.width;
    p->v_res = mode.height;
    p->stride = mode.width * BPP;
    p->fb_size = (p->stride * mode.height + 63u) & ~63u;
    p->num_fbs = cfg->num_fbs ? cfg->num_fbs : (cfg->flags.double_fb ? 2 : 1);
    p->brightness = 255;
    p->refresh_on_demand = cfg->flags.refresh_on_demand;
    for (size_t i = 0; i < p->num_fbs; i++) {
        if (cfg->user_fbs[i]) {
            p->fbs[i] = cfg->user_fbs[i];
        } else {
            p->fbs[i] = heap_caps_aligned_calloc(64, 1, p->fb_size, MALLOC_CAP_DEFAULT);
            p->fb_owned[i] = true;
            if (!p->fbs[i]) {
                err = ESP_ERR_NO_MEM;
                break;
            }
            f101_dcache_clean(p->fbs[i], p->fb_size);
        }
    }
    if (err != ESP_OK) {
        for (size_t i = 0; i < p->num_fbs; i++) {
            if (p->fb_owned[i]) {
                free(p->fbs[i]);
            }
        }
        ESP_LOGE(TAG, "no memory for the %u byte frame buffers", (unsigned)p->fb_size);
        free(p);
        return err;
    }

    p->base.del = panel_rgb_del;
    p->base.reset = panel_rgb_reset;
    p->base.init = panel_rgb_init;
    p->base.draw_bitmap = panel_rgb_draw_bitmap;
    p->base.disp_on_off = panel_rgb_disp_on_off;
    p->base.set_brightness = panel_rgb_set_brightness;
    s_panel = p;
    *ret_panel = &p->base;
    ESP_LOGI(TAG, "%ux%u @ %u Hz, %u frame buffer(s)", (unsigned)p->h_res, (unsigned)p->v_res, (unsigned)mode.refresh_hz, (unsigned)p->num_fbs);
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_callbacks_t *callbacks, void *user_ctx)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(panel && callbacks, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!callbacks->on_bounce_empty, ESP_ERR_NOT_SUPPORTED, TAG, "no bounce buffers");
    p->cbs = *callbacks;
    p->user_ctx = user_ctx;
    if ((callbacks->on_vsync || callbacks->on_frame_buf_complete) && !p->vsync_task) {
        p->vsync_stop = false;
        ESP_RETURN_ON_FALSE(xTaskCreate(vsync_task, "lcd_vsync", 3072, p, configMAX_PRIORITIES - 2, &p->vsync_task) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "no memory for the vsync task");
    }
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_get_frame_buffer(esp_lcd_panel_handle_t panel, uint32_t fb_num, void **fb0, ...)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);
    void **fbs[ESP_RGB_LCD_PANEL_MAX_FB_NUM] = { fb0 };
    va_list args;

    ESP_RETURN_ON_FALSE(panel && fb0, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(fb_num >= 1 && fb_num <= p->num_fbs, ESP_ERR_INVALID_ARG, TAG, "invalid frame buffer count");
    va_start(args, fb0);
    for (uint32_t i = 1; i < fb_num; i++) {
        fbs[i] = va_arg(args, void **);
    }
    va_end(args);
    for (uint32_t i = 0; i < fb_num; i++) {
        *fbs[i] = p->fbs[i];
    }
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_refresh(esp_lcd_panel_handle_t panel)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    f101_dcache_clean(p->fbs[p->cur_fb], p->fb_size);
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_restart(esp_lcd_panel_handle_t panel)
{
    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_set_pclk(esp_lcd_panel_handle_t panel, uint32_t freq_hz)
{
    return ESP_ERR_NOT_SUPPORTED;   /* the pixel clock is derived from the panel timing */
}

void *esp_lcd_rgb_alloc_draw_buffer(esp_lcd_panel_handle_t panel, size_t size, uint32_t caps)
{
    return heap_caps_aligned_alloc(64, (size + 63u) & ~63u, caps ? caps : MALLOC_CAP_DEFAULT);
}
