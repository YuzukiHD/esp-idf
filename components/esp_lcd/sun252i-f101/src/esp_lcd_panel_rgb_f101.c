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
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_interface.h"
#include "esp_lcd_panel_io_interface.h"
#include "driver/gpio.h"
#include "soc/f101_periph_pins.h"
#include "f101_display_desc.h"
#include "esp_lcd_dpi_f101.h"

#include <hal/display/display_engine.h>
#include <dpy/dpy_os.h>

#include "f101_cache.h"

static const char *TAG = "lcd.rgb";

#define FB_PLANE        4          /* UI channel 0, layer 0 */
#define BPP             2
#define MBUS_BASE       0x03102000u

/* DCS write through the DSI device of the bound panel (panel_dsi.c) */
extern int display_dsi_dcs_write(uint8_t channel, uint8_t cmd, const void *data, size_t len);

typedef bool (*panel_cb_t)(esp_lcd_panel_handle_t panel, void *edata, void *user_ctx);

typedef struct {
    esp_lcd_panel_t base;
    f101_display_desc_t desc;       /* what the display stack is configured with */
    bool dsi;
    int disp_gpio;                  /* display enable line of an RGB panel, -1 if none */
    bool disp_active_low;
    esp_lcd_f101_backlight_cb_t backlight;
    void *backlight_ctx;
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
    panel_cb_t cb_trans_done, cb_frame_done, cb_vsync;
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
            /* no vsync while the display is blanked: do not spin at this priority */
            vTaskDelay(1);
            continue;
        }
        esp_lcd_rgb_panel_event_data_t ev = {};
        if (p->frame_done_pending) {
            p->frame_done_pending = false;
            if (p->cb_frame_done) {
                p->cb_frame_done((esp_lcd_panel_handle_t)p, &ev, p->user_ctx);
            }
        }
        if (p->cb_vsync) {
            p->cb_vsync((esp_lcd_panel_handle_t)p, &ev, p->user_ctx);
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
    if (p->inited) {
        display_blank(true);
        display_remove();
    }
    f101_display_graph_clear();
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
    struct display_mode mode;

    ESP_RETURN_ON_FALSE(!p->inited, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    ESP_RETURN_ON_FALSE(f101_display_graph_set(&p->desc) == 0, ESP_ERR_NO_MEM, TAG, "cannot describe the pipeline");
    mbus_setup();
    if (display_probe() || display_wait_ready(5000) || display_get_mode(&mode)) {
        ESP_LOGE(TAG, "display pipeline failed to come up");
        display_remove();
        return ESP_FAIL;
    }
    if (mode.width != p->h_res || mode.height != p->v_res) {
        ESP_LOGE(TAG, "panel is %ux%u, the configuration asks for %ux%u", (unsigned)mode.width, (unsigned)mode.height,
                 (unsigned)p->h_res, (unsigned)p->v_res);
        display_remove();
        return ESP_ERR_INVALID_ARG;
    }
    ESP_RETURN_ON_ERROR(show_framebuffer(p, p->fbs[0]), TAG, "cannot show the frame buffer");
    p->cur_fb = 0;
    p->inited = true;
    display_blank(false);
    if (p->disp_gpio >= 0) {
        gpio_set_direction(p->disp_gpio, GPIO_MODE_OUTPUT);
        gpio_set_level(p->disp_gpio, !p->disp_active_low);
    }
    if (p->backlight) {
        p->backlight(p->backlight_ctx, p->brightness);
    }
    ESP_LOGI(TAG, "%ux%u @ %u Hz, %u frame buffer(s)", (unsigned)p->h_res, (unsigned)p->v_res, (unsigned)mode.refresh_hz, (unsigned)p->num_fbs);
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
            if (p->cb_trans_done) {
                esp_lcd_rgb_panel_event_data_t ev = {};
                p->cb_trans_done(panel, &ev, p->user_ctx);
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
    if (p->cb_trans_done) {
        esp_lcd_rgb_panel_event_data_t ev = {};
        p->cb_trans_done(panel, &ev, p->user_ctx);
    }
    return ESP_OK;
}

static esp_err_t panel_rgb_disp_on_off(esp_lcd_panel_t *panel, bool on_off)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(p->inited, ESP_ERR_INVALID_STATE, TAG, "panel not initialized");
    if (p->disp_gpio >= 0) {
        gpio_set_level(p->disp_gpio, on_off != p->disp_active_low);
    }
    return display_blank(!on_off) == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t panel_rgb_set_brightness(esp_lcd_panel_t *panel, int brightness)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(brightness >= 0 && brightness <= 255, ESP_ERR_INVALID_ARG, TAG, "brightness is 0..255");
    p->brightness = brightness;
    return p->backlight ? p->backlight(p->backlight_ctx, brightness) : ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t panel_create(const f101_display_desc_t *desc, size_t num_fbs, void *const *user_fbs, bool refresh_on_demand,
                              f101_rgb_panel_t **out)
{
    f101_rgb_panel_t *p;
    esp_err_t err = ESP_OK;

    ESP_RETURN_ON_FALSE(!s_panel, ESP_ERR_INVALID_STATE, TAG, "the display engine drives one panel");
    ESP_RETURN_ON_FALSE(num_fbs <= ESP_RGB_LCD_PANEL_MAX_FB_NUM, ESP_ERR_INVALID_ARG, TAG, "too many frame buffers");
    p = heap_caps_calloc(1, sizeof(*p), MALLOC_CAP_DEFAULT);
    ESP_RETURN_ON_FALSE(p, ESP_ERR_NO_MEM, TAG, "no memory for the panel");

    p->desc = *desc;
    p->dsi = desc->dsi;
    p->disp_gpio = -1;
    p->h_res = desc->mode.hdisplay;
    p->v_res = desc->mode.vdisplay;
    p->stride = p->h_res * BPP;
    p->fb_size = (p->stride * p->v_res + 63u) & ~63u;
    p->num_fbs = num_fbs ? num_fbs : 1;
    p->brightness = 255;
    p->refresh_on_demand = refresh_on_demand;
    for (size_t i = 0; i < p->num_fbs; i++) {
        if (user_fbs && user_fbs[i]) {
            p->fbs[i] = user_fbs[i];
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
    *out = p;
    return ESP_OK;
}

static esp_err_t start_vsync_task(f101_rgb_panel_t *p)
{
    if (!p->vsync_task) {
        p->vsync_stop = false;
        ESP_RETURN_ON_FALSE(xTaskCreate(vsync_task, "lcd_vsync", 3072, p, configMAX_PRIORITIES - 2, &p->vsync_task) == pdPASS,
                            ESP_ERR_NO_MEM, TAG, "no memory for the vsync task");
    }
    return ESP_OK;
}

/* ---- parallel RGB panel ---------------------------------------------------------------------- */

esp_err_t esp_lcd_new_rgb_panel(const esp_lcd_rgb_panel_config_t *cfg, esp_lcd_panel_handle_t *ret_panel)
{
    static f101_display_desc_t desc;
    const esp_lcd_rgb_timing_t *t;
    f101_rgb_panel_t *p;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(cfg && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->in_color_format == LCD_COLOR_FMT_RGB565 || cfg->in_color_format == 0, ESP_ERR_NOT_SUPPORTED, TAG, "only RGB565 frame buffers");
    ESP_RETURN_ON_FALSE(!cfg->flags.no_fb, ESP_ERR_NOT_SUPPORTED, TAG, "a frame buffer is required");
    ESP_RETURN_ON_FALSE(cfg->data_width == 16 || cfg->data_width == 18 || cfg->data_width == 24, ESP_ERR_INVALID_ARG, TAG,
                        "the RGB bus is 16, 18 or 24 bits wide");
    t = &cfg->timings;
    ESP_RETURN_ON_FALSE(t->pclk_hz && t->h_res && t->v_res, ESP_ERR_INVALID_ARG, TAG, "the timing is not set");

    memset(&desc, 0, sizeof(desc));
    desc.reset_gpio = -1;
    desc.mode = (struct dpy_display_mode){
        .clock = t->pclk_hz / 1000,
        .hdisplay = t->h_res, .hsync_start = t->h_res + t->hsync_front_porch,
        .hsync_end = t->h_res + t->hsync_front_porch + t->hsync_pulse_width,
        .htotal = t->h_res + t->hsync_front_porch + t->hsync_pulse_width + t->hsync_back_porch,
        .vdisplay = t->v_res, .vsync_start = t->v_res + t->vsync_front_porch,
        .vsync_end = t->v_res + t->vsync_front_porch + t->vsync_pulse_width,
        .vtotal = t->v_res + t->vsync_front_porch + t->vsync_pulse_width + t->vsync_back_porch,
        .flags = (t->flags.hsync_idle_low ? 0 : DISPLAY_MODE_FLAG_NHSYNC) | (t->flags.vsync_idle_low ? 0 : DISPLAY_MODE_FLAG_NVSYNC) |
                 DISPLAY_MODE_FLAG_PREFERRED,
    };
    desc.bus_format = cfg->data_width == 24 ? DPY_BUS_FMT_RGB888_1X24 : cfg->data_width == 18 ? DPY_BUS_FMT_RGB666_1X18 : DPY_BUS_FMT_RGB565_1X16;
    desc.bus_flags = (t->flags.de_idle_high ? DPY_BUS_FLAG_DE_LOW : 0) | (t->flags.pclk_active_neg ? DPY_BUS_FLAG_PIXDATA_NEGEDGE : 0);

    /* the pads come from the configuration; the SoC says which ones the RGB bus can use */
    gpio_num_t sig[ESP_LCD_RGB_BUS_WIDTH_MAX + 4];
    size_t n = 0;

    for (size_t i = 0; i < cfg->data_width; i++) {
        sig[n++] = cfg->data_gpio_nums[i];
    }
    sig[n++] = cfg->pclk_gpio_num;
    sig[n++] = cfg->hsync_gpio_num;
    sig[n++] = cfg->vsync_gpio_num;
    sig[n++] = cfg->de_gpio_num;
    for (size_t i = 0; i < n; i++) {
        if (sig[i] < 0) {
            continue;
        }
        int mux = f101_periph_pin_mux(F101_SIG_LCD_RGB, 0, sig[i]);

        ESP_RETURN_ON_FALSE(mux >= 0, ESP_ERR_INVALID_ARG, TAG, "gpio %d is not a pad of the RGB bus", (int)sig[i]);
        ESP_RETURN_ON_FALSE(desc.npins < F101_DISP_MAX_PINS, ESP_ERR_INVALID_ARG, TAG, "too many pads");
        desc.pins[desc.npins++] = (struct dpy_pin)DPY_PIN(sig[i], mux);
    }

    err = panel_create(&desc, cfg->num_fbs ? cfg->num_fbs : (cfg->flags.double_fb ? 2 : 1), cfg->user_fbs, cfg->flags.refresh_on_demand, &p);
    if (err != ESP_OK) {
        return err;
    }
    p->disp_gpio = cfg->disp_gpio_num;
    p->disp_active_low = cfg->flags.disp_active_low;
    *ret_panel = &p->base;
    return ESP_OK;
}

esp_err_t esp_lcd_rgb_panel_register_event_callbacks(esp_lcd_panel_handle_t panel, const esp_lcd_rgb_panel_event_callbacks_t *callbacks, void *user_ctx)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(panel && callbacks, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!callbacks->on_bounce_empty, ESP_ERR_NOT_SUPPORTED, TAG, "no bounce buffers");
    p->cb_trans_done = (panel_cb_t)callbacks->on_color_trans_done;
    p->cb_frame_done = (panel_cb_t)callbacks->on_frame_buf_complete;
    p->cb_vsync = (panel_cb_t)callbacks->on_vsync;
    p->user_ctx = user_ctx;
    if (callbacks->on_vsync || callbacks->on_frame_buf_complete) {
        return start_vsync_task(p);
    }
    return ESP_OK;
}

esp_err_t esp_lcd_panel_f101_set_backlight(esp_lcd_panel_handle_t panel, esp_lcd_f101_backlight_cb_t set, void *ctx)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    p->backlight = set;
    p->backlight_ctx = ctx;
    return ESP_OK;
}

/* ---- MIPI DSI: bus, DBI command io, DPI panel ------------------------------------------------- */

struct esp_lcd_dsi_bus_t {
    uint8_t lanes;
    float lane_bit_rate_mbps;
    bool force_hs_clock;
};

static struct esp_lcd_dsi_bus_t s_dsi_bus;
static bool s_dsi_bus_used;

esp_err_t esp_lcd_new_dsi_bus(const esp_lcd_dsi_bus_config_t *cfg, esp_lcd_dsi_bus_handle_t *ret_bus)
{
    int n;

    ESP_RETURN_ON_FALSE(cfg && ret_bus, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->bus_id == 0, ESP_ERR_INVALID_ARG, TAG, "there is one DSI controller");
    ESP_RETURN_ON_FALSE(!s_dsi_bus_used, ESP_ERR_INVALID_STATE, TAG, "the DSI bus exists already");
    ESP_RETURN_ON_FALSE(cfg->num_data_lanes <= 4, ESP_ERR_INVALID_ARG, TAG, "at most 4 data lanes");
    (void)f101_periph_pin_options(F101_SIG_LCD_DSI, 0, &n);
    ESP_RETURN_ON_FALSE(n > 0, ESP_ERR_NOT_SUPPORTED, TAG, "no DSI pads");
    s_dsi_bus = (struct esp_lcd_dsi_bus_t){ .lanes = cfg->num_data_lanes ? cfg->num_data_lanes : 4,
                                            .lane_bit_rate_mbps = cfg->lane_bit_rate_mbps,
                                            .force_hs_clock = cfg->flags.clock_lane_force_hs };
    s_dsi_bus_used = true;
    *ret_bus = &s_dsi_bus;
    return ESP_OK;
}

esp_err_t esp_lcd_del_dsi_bus(esp_lcd_dsi_bus_handle_t bus)
{
    ESP_RETURN_ON_FALSE(bus == &s_dsi_bus && s_dsi_bus_used, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    s_dsi_bus_used = false;
    return ESP_OK;
}

/* DBI io: commands over the DSI link; they work once the DPI panel has been initialized */
typedef struct {
    esp_lcd_panel_io_t base;
    uint8_t channel;
} f101_dbi_io_t;

static esp_err_t dbi_tx_param(esp_lcd_panel_io_t *io, int lcd_cmd, const void *param, size_t param_size)
{
    f101_dbi_io_t *d = __containerof(io, f101_dbi_io_t, base);

    ESP_RETURN_ON_FALSE(s_panel && s_panel->inited && s_panel->dsi, ESP_ERR_INVALID_STATE, TAG, "the DSI link is not up yet");
    int ret = display_dsi_dcs_write(d->channel, (uint8_t)lcd_cmd, param, param_size);

    return ret == 0 ? ESP_OK : ESP_FAIL;
}

static esp_err_t dbi_del(esp_lcd_panel_io_t *io)
{
    free(__containerof(io, f101_dbi_io_t, base));
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_io_dbi(esp_lcd_dsi_bus_handle_t bus, const esp_lcd_dbi_io_config_t *cfg, esp_lcd_panel_io_handle_t *ret_io)
{
    ESP_RETURN_ON_FALSE(bus == &s_dsi_bus && cfg && ret_io, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    f101_dbi_io_t *d = calloc(1, sizeof(*d));

    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "no memory");
    d->channel = cfg->virtual_channel;
    d->base.tx_param = dbi_tx_param;
    d->base.del = dbi_del;
    *ret_io = &d->base;
    return ESP_OK;
}

esp_err_t esp_lcd_new_panel_dpi(esp_lcd_dsi_bus_handle_t bus, const esp_lcd_dpi_panel_config_t *cfg, esp_lcd_panel_handle_t *ret_panel)
{
    static f101_display_desc_t desc;
    const esp_lcd_video_timing_t *t;
    f101_rgb_panel_t *p;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(bus == &s_dsi_bus && cfg && ret_panel, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(cfg->in_color_format == LCD_COLOR_FMT_RGB565 || cfg->in_color_format == 0, ESP_ERR_NOT_SUPPORTED, TAG, "only RGB565 frame buffers");
    t = &cfg->video_timing;
    ESP_RETURN_ON_FALSE(t->h_size && t->v_size && cfg->dpi_clock_freq_mhz > 0, ESP_ERR_INVALID_ARG, TAG, "the timing is not set");

    memset(&desc, 0, sizeof(desc));
    desc.dsi = true;
    desc.reset_gpio = -1;
    desc.lanes = bus->lanes;
    desc.dsi_mode_flags = F101_DSI_MODE_VIDEO | F101_DSI_MODE_VIDEO_SYNC_PULSE | F101_DSI_MODE_LPM;
    desc.enable_delay_ms = 20;
    desc.mode = (struct dpy_display_mode){
        .clock = (uint32_t)(cfg->dpi_clock_freq_mhz * 1000.0f),
        .hdisplay = t->h_size, .hsync_start = t->h_size + t->hsync_front_porch,
        .hsync_end = t->h_size + t->hsync_front_porch + t->hsync_pulse_width,
        .htotal = t->h_size + t->hsync_front_porch + t->hsync_pulse_width + t->hsync_back_porch,
        .vdisplay = t->v_size, .vsync_start = t->v_size + t->vsync_front_porch,
        .vsync_end = t->v_size + t->vsync_front_porch + t->vsync_pulse_width,
        .vtotal = t->v_size + t->vsync_front_porch + t->vsync_pulse_width + t->vsync_back_porch,
        .flags = DISPLAY_MODE_FLAG_NHSYNC | DISPLAY_MODE_FLAG_NVSYNC | DISPLAY_MODE_FLAG_PREFERRED,
    };
    err = panel_create(&desc, cfg->num_fbs, NULL, false, &p);
    if (err != ESP_OK) {
        return err;
    }
    *ret_panel = &p->base;
    return ESP_OK;
}

esp_err_t esp_lcd_dpi_panel_f101_setup(esp_lcd_panel_handle_t panel, const esp_lcd_dpi_f101_extra_t *x)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(panel && x && p->dsi, ESP_ERR_INVALID_ARG, TAG, "not a DPI panel");
    ESP_RETURN_ON_FALSE(!p->inited, ESP_ERR_INVALID_STATE, TAG, "already initialized");
    p->desc.init_cmds = x->init_cmds;
    p->desc.n_init = x->init_cmds_size;
    p->desc.exit_cmds = x->exit_cmds;
    p->desc.n_exit = x->exit_cmds_size;
    p->desc.reset_gpio = x->reset_gpio_num;
    p->desc.reset_hold_ms = x->reset_hold_ms;
    if (x->mode_flags) {
        p->desc.dsi_mode_flags = x->mode_flags;
    }
    p->desc.width_mm = x->width_mm;
    p->desc.height_mm = x->height_mm;
    if (x->enable_delay_ms) {
        p->desc.enable_delay_ms = x->enable_delay_ms;
    }
    return ESP_OK;
}

esp_err_t esp_lcd_dpi_panel_register_event_callbacks(esp_lcd_panel_handle_t panel, const esp_lcd_dpi_panel_event_callbacks_t *cbs, void *user_ctx)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);

    ESP_RETURN_ON_FALSE(panel && cbs && p->dsi, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    p->cb_trans_done = (panel_cb_t)cbs->on_color_trans_done;
    p->cb_frame_done = (panel_cb_t)cbs->on_frame_buf_complete;
    p->cb_vsync = (panel_cb_t)cbs->on_vsync;
    p->user_ctx = user_ctx;
    if (cbs->on_vsync || cbs->on_frame_buf_complete) {
        return start_vsync_task(p);
    }
    return ESP_OK;
}

esp_err_t esp_lcd_dpi_panel_get_frame_buffer(esp_lcd_panel_handle_t panel, uint32_t fb_num, void **fb0, ...)
{
    f101_rgb_panel_t *p = __containerof(panel, f101_rgb_panel_t, base);
    void **fbs[ESP_RGB_LCD_PANEL_MAX_FB_NUM] = { fb0 };
    va_list args;

    ESP_RETURN_ON_FALSE(panel && fb0 && p->dsi, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
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

esp_err_t esp_lcd_dpi_panel_set_pattern(esp_lcd_panel_handle_t panel, mipi_dsi_pattern_type_t pattern)
{
    return ESP_ERR_NOT_SUPPORTED;
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
