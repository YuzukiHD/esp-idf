/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pixel processing accelerator API (driver/ppa.h) on the 2D accelerator of the F101.
 *
 * A transaction is validated and turned into a register command list when it is submitted (g2d/); a
 * worker task feeds the lists to the hardware one at a time and the task-end interrupt completes them.
 * Scale-rotate-mirror is the accelerator's blit, blend is its compose, fill is its fill.
 */
#include <string.h>
#include <stdlib.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include <errno.h>
#include "esp_heap_caps.h"
#include "esp_intr_alloc.h"
#include "esp_cache.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "driver/ppa.h"
#include "soc/sun252i_f101_ll.h"
#include "g2d/g2d_types.h"
#include "g2d/g2d_sunxi_hw.h"
#include "g2d/g2d_sunxi_regs.h"

static const char *TAG = "ppa";

#define G2D_BASE            0x05410000u
#define G2D_IRQ_SRC         111
#define CCU_MOD_REG         0x0630u     /* module clock: mux [26:24], divider [4:0], gate bit 31 */
#define CCU_BUS_REG         0x063cu     /* bus gate bit 0, reset bit 16 */
#define CCU_MBUS_REG        0x0804u     /* memory bus gate bit 10 */
#define PLL_PERI_REG        0x0020u
#define MODULE_RATE         300000000u
#define THREAD_ID           0
#define CMDLIST_BYTES       2560u
#define CACHE_SIZE          0x40u       /* internal cache of the accelerator */
#define DDR_LIMIT           0x90u       /* bandwidth limiter */
#define POOL                8
#define TIMEOUT_MS          3000
#define MAX_SIZE            8192u

typedef struct ppa_client_t {
    ppa_operation_t type;
    ppa_event_callbacks_t cbs;
    uint32_t max_pending;
    volatile uint32_t pending;
    struct ppa_client_t *next;
} ppa_client_t;

typedef struct {
    struct g2d_op op;
    struct g2d_cmdlist cl;
    size_t head_len;
    ppa_client_t *client;
    void *user_data;
    SemaphoreHandle_t done;         /* blocking transactions wait here */
    volatile int status;
    uint8_t cmd[CMDLIST_BYTES] __attribute__((aligned(64)));
} job_t;

static struct {
    bool ready;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t hw_done;
    QueueHandle_t queue;            /* job_t * waiting for the hardware */
    QueueHandle_t free_jobs;        /* job_t * not in use */
    job_t *jobs;
    uint32_t hw_status;
    ppa_client_t *clients;
    intr_handle_t intr;
} s;

static inline uint32_t rd(uint32_t off)
{
    return F101_REG32(G2D_BASE + off);
}

static inline void wr(uint32_t off, uint32_t v)
{
    F101_REG32(G2D_BASE + off) = v;
}

static void upd(uint32_t off, uint32_t mask, uint32_t val)
{
    wr(off, (rd(off) & ~mask) | (val & mask));
}

static inline uint32_t ccu_rd(uint32_t off)
{
    return F101_REG32(F101_CCU_BASE + off);
}

static inline void ccu_wr(uint32_t off, uint32_t v)
{
    F101_REG32(F101_CCU_BASE + off) = v;
}

/* ---- hardware ----------------------------------------------------------------------------- */

static void pulse_reset(uint32_t bits)
{
    upd(G2D_RESET, bits, 0);
    upd(G2D_RESET, bits, bits);
}

static void hw_open(void)
{
    upd(G2D_CLK_GATE, G2D_CLK_GATE_CORE, G2D_CLK_GATE_CORE);
    upd(G2D_MBUS_GATE, G2D_MBUS_GATE_CLK | G2D_MBUS_GATE_RESET, G2D_MBUS_GATE_CLK | G2D_MBUS_GATE_RESET);
    upd(G2D_RESET, G2D_RESET_CORE, G2D_RESET_CORE);
    upd(G2D_CLK_GATE, G2D_CLK_GATE_THREAD(THREAD_ID), G2D_CLK_GATE_THREAD(THREAD_ID));
    upd(G2D_RESET, G2D_RESET_THREAD(THREAD_ID), G2D_RESET_THREAD(THREAD_ID));
    upd(G2D_CACHE_CTRL, G2D_CACHE_CTRL_EN, G2D_CACHE_CTRL_EN);
    upd(G2D_CACHE_SIZE0, G2D_CACHE_SIZE_MASK, CACHE_SIZE);
    upd(G2D_CACHE_SIZE1, G2D_CACHE_SIZE_MASK, CACHE_SIZE);
    /* keep the accelerator from starving the display engine and the CPU of memory bandwidth */
    upd(G2D_DDR_LIMIT, G2D_DDR_LIMIT_EN | G2D_DDR_LIMIT_MASK, G2D_DDR_LIMIT_EN | DDR_LIMIT);
}

static void hw_recover(void)
{
    pulse_reset(G2D_RESET_CORE);
    pulse_reset(G2D_RESET_THREAD(THREAD_ID));
}

static void hw_start(const job_t *job)
{
    const uint32_t thread = G2D_THREAD(THREAD_ID);

    upd(G2D_MODE, G2D_MODE_MASTER, G2D_MODE_MASTER);
    wr(thread + G2D_THREAD_IRQ_EN, G2D_THREAD_IRQ_TASK_END | G2D_THREAD_IRQ_TIMEOUT);
    wr(thread + G2D_THREAD_HEAD_LOW, (uint32_t)(uintptr_t)job->cmd);
    wr(thread + G2D_THREAD_HEAD_HIGH_LEN, FIELD_PREP(G2D_THREAD_HEAD_LEN_MASK, job->head_len));
    wr(thread + G2D_THREAD_ATTR, FIELD_PREP(G2D_THREAD_ATTR_CMD_NUM_MASK, 0) | G2D_THREAD_ATTR_END_IRQ);
    wr(thread + G2D_THREAD_UPDATE, 1);
}

static void isr(void *arg)
{
    const uint32_t reg = G2D_THREAD(THREAD_ID) + G2D_THREAD_IRQ_STATUS;
    uint32_t status = rd(reg);
    BaseType_t woken = pdFALSE;

    wr(reg, status);                    /* write 1 to clear */
    if (status & (G2D_THREAD_IRQ_TASK_END | G2D_THREAD_IRQ_TIMEOUT)) {
        pulse_reset(G2D_RESET_THREAD(THREAD_ID));
        s.hw_status = status;
        xSemaphoreGiveFromISR(s.hw_done, &woken);
    }
    if (woken) {
        portYIELD_FROM_ISR();
    }
}

/* ---- cache maintenance ---------------------------------------------------------------------- */

static void cache_rect(const struct g2d_surface *surf, const struct g2d_rect *rect, bool invalidate_too, bool invalidate_only)
{
    const struct g2d_fmt_info *fmt = g2d_fmt_get(surf->format);

    for (unsigned p = 0; fmt && p < fmt->planes; p++) {
        void *start;
        size_t len;

        if (g2d_surface_rect_span(surf, rect, p, &start, &len)) {
            continue;
        }
        if (invalidate_only) {
            esp_cache_msync(start, len, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
        } else {
            esp_cache_msync(start, len, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED |
                                            (invalidate_too ? ESP_CACHE_MSYNC_FLAG_INVALIDATE : 0));
        }
    }
}

static void cache_before(const struct g2d_op *op)
{
    /* dirty lines of the destination must not be written back over the result */
    cache_rect(&op->dst, &op->dst_rect, true, false);
    if (op->type != G2D_OP_FILL) {
        cache_rect(&op->src, &op->src_rect, false, false);
    }
    if (op->type == G2D_OP_BLEND) {
        cache_rect(&op->bg, &op->bg_rect, false, false);
    }
}

/* ---- worker --------------------------------------------------------------------------------- */

static int run_job(job_t *job)
{
    int ret = 0;

    cache_before(&job->op);
    xSemaphoreTake(s.hw_done, 0);
    hw_start(job);
    if (xSemaphoreTake(s.hw_done, pdMS_TO_TICKS(TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "operation %d timed out", job->op.type);
        hw_recover();
        ret = -1;
    } else if (s.hw_status & G2D_THREAD_IRQ_TIMEOUT) {
        ESP_LOGE(TAG, "hardware timeout in operation %d", job->op.type);
        hw_recover();
        ret = -1;
    }
    cache_rect(&job->op.dst, &job->op.dst_rect, false, true);
    return ret;
}

static void worker(void *arg)
{
    job_t *job;

    for (;;) {
        if (xQueueReceive(s.queue, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        job->status = run_job(job);
        ppa_client_t *c = job->client;

        if (job->done) {
            xSemaphoreGive(job->done);          /* the caller releases the job */
        } else {
            if (c->cbs.on_trans_done) {
                ppa_event_data_t ev = {};
                c->cbs.on_trans_done(c, &ev, job->user_data);
            }
            c->pending--;
            xQueueSend(s.free_jobs, &job, 0);
        }
    }
}

static uint32_t pll_peri_rate(void)
{
    uint32_t pll = ccu_rd(PLL_PERI_REG);
    uint32_t n = ((pll >> 8) & 0xff) + 1;
    uint32_t p0 = ((pll >> 16) & 0x7) + 1;
    uint32_t m = (pll & BIT(1)) ? 2 : 1;

    return 24000000u / m / p0 * n;
}

static esp_err_t hw_init(void)
{
    if (s.ready) {
        return ESP_OK;
    }
    uint32_t src = pll_peri_rate();
    uint32_t div = (src + MODULE_RATE - 1) / MODULE_RATE;
    uint32_t reg;

    if (div < 1) {
        div = 1;
    }
    if (div > 32) {
        div = 32;
    }
    /* bus and memory bus gates, reset released, module clock PLL_PERI_2X / div */
    ccu_wr(CCU_BUS_REG, ccu_rd(CCU_BUS_REG) & ~BIT(16));
    ccu_wr(CCU_BUS_REG, ccu_rd(CCU_BUS_REG) | BIT(0));
    ccu_wr(CCU_BUS_REG, ccu_rd(CCU_BUS_REG) | BIT(16));
    ccu_wr(CCU_MBUS_REG, ccu_rd(CCU_MBUS_REG) | BIT(10));
    reg = ccu_rd(CCU_MOD_REG) & ~(BIT(31) | GENMASK(26, 24) | GENMASK(4, 0));
    reg |= div - 1;
    ccu_wr(CCU_MOD_REG, reg);
    ccu_wr(CCU_MOD_REG, reg | BIT(31));

    s.jobs = heap_caps_aligned_calloc(64, POOL, sizeof(job_t), MALLOC_CAP_DEFAULT);
    s.lock = xSemaphoreCreateMutex();
    s.hw_done = xSemaphoreCreateBinary();
    s.queue = xQueueCreate(POOL, sizeof(job_t *));
    s.free_jobs = xQueueCreate(POOL, sizeof(job_t *));
    ESP_RETURN_ON_FALSE(s.jobs && s.lock && s.hw_done && s.queue && s.free_jobs, ESP_ERR_NO_MEM, TAG, "no memory");
    for (int i = 0; i < POOL; i++) {
        job_t *j = &s.jobs[i];

        xQueueSend(s.free_jobs, &j, 0);
    }
    hw_open();
    ESP_RETURN_ON_ERROR(esp_intr_alloc(G2D_IRQ_SRC, 0, isr, NULL, &s.intr), TAG, "interrupt");
    ESP_RETURN_ON_FALSE(xTaskCreate(worker, "ppa", 3072, NULL, 5, NULL) == pdPASS, ESP_ERR_NO_MEM, TAG, "task");
    s.ready = true;
    ESP_LOGI(TAG, "module clock %u Hz", (unsigned)(src / div));
    return ESP_OK;
}

/* ---- clients ------------------------------------------------------------------------------- */

esp_err_t ppa_register_client(const ppa_client_config_t *config, ppa_client_handle_t *ret_client)
{
    ESP_RETURN_ON_FALSE(config && ret_client, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(config->oper_type < PPA_OPERATION_INVALID, ESP_ERR_INVALID_ARG, TAG, "invalid operation type");
    ESP_RETURN_ON_ERROR(hw_init(), TAG, "init");
    ppa_client_t *c = calloc(1, sizeof(*c));

    ESP_RETURN_ON_FALSE(c, ESP_ERR_NO_MEM, TAG, "no memory");
    c->type = config->oper_type;
    c->max_pending = config->max_pending_trans_num ? config->max_pending_trans_num : 1;
    if (c->max_pending > POOL) {
        c->max_pending = POOL;
    }
    xSemaphoreTake(s.lock, portMAX_DELAY);
    c->next = s.clients;
    s.clients = c;
    xSemaphoreGive(s.lock);
    *ret_client = c;
    return ESP_OK;
}

esp_err_t ppa_unregister_client(ppa_client_handle_t client)
{
    ESP_RETURN_ON_FALSE(client, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(client->pending == 0, ESP_ERR_INVALID_STATE, TAG, "transactions are pending");
    xSemaphoreTake(s.lock, portMAX_DELAY);
    for (ppa_client_t **p = &s.clients; *p; p = &(*p)->next) {
        if (*p == client) {
            *p = client->next;
            break;
        }
    }
    xSemaphoreGive(s.lock);
    free(client);
    return ESP_OK;
}

esp_err_t ppa_client_register_event_callbacks(ppa_client_handle_t client, const ppa_event_callbacks_t *cbs)
{
    ESP_RETURN_ON_FALSE(client && cbs, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    client->cbs = *cbs;
    return ESP_OK;
}

/* ---- conversion of the PPA descriptions ------------------------------------------------------ */

static int g2d_format_of(uint32_t cm, bool *ok)
{
    *ok = true;
    switch (cm) {
    case PPA_SRM_COLOR_MODE_ARGB8888:
        return G2D_PIXFMT_ARGB8888;
    case PPA_SRM_COLOR_MODE_RGB888:
        return G2D_PIXFMT_RGB888;
    case PPA_SRM_COLOR_MODE_RGB565:
        return G2D_PIXFMT_RGB565;
    default:
        *ok = false;
        return 0;
    }
}

static esp_err_t surface_of(const void *buffer, uint32_t w, uint32_t h, uint32_t cm, struct g2d_surface *out)
{
    bool ok;
    int fmt = g2d_format_of(cm, &ok);

    ESP_RETURN_ON_FALSE(ok, ESP_ERR_NOT_SUPPORTED, TAG, "color mode 0x%x is not supported", (unsigned)cm);
    ESP_RETURN_ON_FALSE(buffer && w && h && w <= MAX_SIZE && h <= MAX_SIZE, ESP_ERR_INVALID_ARG, TAG, "invalid picture");
    memset(out, 0, sizeof(*out));
    out->format = fmt;
    out->width = w;
    out->height = h;
    out->plane[0] = (void *)buffer;
    out->pitch[0] = w * g2d_format_bytes_per_pixel(fmt);
    return ESP_OK;
}

static esp_err_t block_of(uint32_t pic_w, uint32_t pic_h, uint32_t ox, uint32_t oy, uint32_t bw, uint32_t bh, struct g2d_rect *r)
{
    ESP_RETURN_ON_FALSE(bw && bh && ox + bw <= pic_w && oy + bh <= pic_h, ESP_ERR_INVALID_ARG, TAG, "the block is outside the picture");
    *r = (struct g2d_rect){ ox, oy, bw, bh };
    return ESP_OK;
}

/* Take a job, build its command list, and run it (blocking) or queue it. */
static esp_err_t submit(ppa_client_t *c, const struct g2d_op *op, ppa_trans_mode_t mode, void *user_data)
{
    job_t *job;

    ESP_RETURN_ON_FALSE(c->pending < c->max_pending, ESP_ERR_NO_MEM, TAG, "too many pending transactions");
    ESP_RETURN_ON_FALSE(xQueueReceive(s.free_jobs, &job, 0) == pdTRUE, ESP_ERR_NO_MEM, TAG, "no free transaction");
    job->op = *op;
    job->client = c;
    job->user_data = user_data;
    job->done = NULL;
    g2d_cmdlist_init(&job->cl, job->cmd, sizeof(job->cmd));
    int ret = g2d_compose(&job->cl, &job->op);

    if (ret) {
        xQueueSend(s.free_jobs, &job, 0);
        ESP_LOGE(TAG, "the accelerator cannot do this operation (%d)", ret);
        return ret == -ENOTSUP || ret == -EINVAL ? ESP_ERR_NOT_SUPPORTED : ESP_ERR_INVALID_ARG;
    }
    job->head_len = g2d_cmdlist_finish(&job->cl);
    esp_cache_msync(job->cmd, (job->head_len + 63u) & ~63u, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
    c->pending++;
    if (mode == PPA_TRANS_MODE_NON_BLOCKING) {
        xQueueSend(s.queue, &job, 0);
        return ESP_OK;
    }
    job->done = xSemaphoreCreateBinary();
    if (!job->done) {
        c->pending--;
        xQueueSend(s.free_jobs, &job, 0);
        return ESP_ERR_NO_MEM;
    }
    xQueueSend(s.queue, &job, 0);
    xSemaphoreTake(job->done, portMAX_DELAY);
    int status = job->status;

    vSemaphoreDelete(job->done);
    job->done = NULL;
    c->pending--;
    ppa_event_data_t ev = {};

    if (c->cbs.on_trans_done) {
        c->cbs.on_trans_done(c, &ev, user_data);
    }
    xQueueSend(s.free_jobs, &job, 0);
    return status == 0 ? ESP_OK : ESP_FAIL;
}

/* ---- operations ------------------------------------------------------------------------------ */

esp_err_t ppa_do_scale_rotate_mirror(ppa_client_handle_t client, const ppa_srm_oper_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(client && cfg && client->type == PPA_OPERATION_SRM, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!cfg->rgb_swap && !cfg->byte_swap && cfg->alpha_update_mode == PPA_ALPHA_NO_CHANGE, ESP_ERR_NOT_SUPPORTED, TAG,
                        "swaps and alpha updates are not supported");
    ESP_RETURN_ON_FALSE(cfg->scale_x > 0 && cfg->scale_y > 0 && cfg->rotation_angle <= PPA_SRM_ROTATION_ANGLE_270, ESP_ERR_INVALID_ARG, TAG, "invalid scale or angle");
    struct g2d_op op = { .type = G2D_OP_BLIT };

    ESP_RETURN_ON_ERROR(surface_of(cfg->in.buffer, cfg->in.pic_w, cfg->in.pic_h, cfg->in.srm_cm, &op.src), TAG, "input");
    ESP_RETURN_ON_ERROR(surface_of(cfg->out.buffer, cfg->out.pic_w, cfg->out.pic_h, cfg->out.srm_cm, &op.dst), TAG, "output");
    ESP_RETURN_ON_ERROR(block_of(cfg->in.pic_w, cfg->in.pic_h, cfg->in.block_offset_x, cfg->in.block_offset_y, cfg->in.block_w,
                                 cfg->in.block_h, &op.src_rect), TAG, "input block");
    /* the output block: the input block scaled, then rotated (a quarter turn swaps the sides) */
    bool quarter = cfg->rotation_angle == PPA_SRM_ROTATION_ANGLE_90 || cfg->rotation_angle == PPA_SRM_ROTATION_ANGLE_270;
    uint32_t ow = (uint32_t)((quarter ? cfg->in.block_h * cfg->scale_y : cfg->in.block_w * cfg->scale_x) + 0.5f);
    uint32_t oh = (uint32_t)((quarter ? cfg->in.block_w * cfg->scale_x : cfg->in.block_h * cfg->scale_y) + 0.5f);

    ESP_RETURN_ON_ERROR(block_of(cfg->out.pic_w, cfg->out.pic_h, cfg->out.block_offset_x, cfg->out.block_offset_y, ow, oh, &op.dst_rect),
                        TAG, "output block");
    /* the accelerator rotates clockwise, the API counter clockwise */
    static const enum g2d_rotation rot[4] = { G2D_ROTATE_0, G2D_ROTATE_270, G2D_ROTATE_180, G2D_ROTATE_90 };

    op.rotation = rot[cfg->rotation_angle];
    op.flags = (cfg->mirror_x ? G2D_FLIP_H : 0) | (cfg->mirror_y ? G2D_FLIP_V : 0);
    return submit(client, &op, cfg->mode, cfg->user_data);
}

esp_err_t ppa_do_blend(ppa_client_handle_t client, const ppa_blend_oper_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(client && cfg && client->type == PPA_OPERATION_BLEND, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_FALSE(!cfg->bg_rgb_swap && !cfg->bg_byte_swap && !cfg->fg_rgb_swap && !cfg->fg_byte_swap && !cfg->fg_ck_en,
                        ESP_ERR_NOT_SUPPORTED, TAG, "swaps and foreground color keying are not supported");
    struct g2d_op op = { .type = G2D_OP_BLEND };
    struct g2d_blend *b = &op.blend;

    ESP_RETURN_ON_ERROR(surface_of(cfg->in_fg.buffer, cfg->in_fg.pic_w, cfg->in_fg.pic_h, cfg->in_fg.blend_cm, &op.src), TAG, "foreground");
    ESP_RETURN_ON_ERROR(surface_of(cfg->in_bg.buffer, cfg->in_bg.pic_w, cfg->in_bg.pic_h, cfg->in_bg.blend_cm, &op.bg), TAG, "background");
    ESP_RETURN_ON_ERROR(surface_of(cfg->out.buffer, cfg->out.pic_w, cfg->out.pic_h, cfg->out.blend_cm, &op.dst), TAG, "output");
    ESP_RETURN_ON_ERROR(block_of(cfg->in_fg.pic_w, cfg->in_fg.pic_h, cfg->in_fg.block_offset_x, cfg->in_fg.block_offset_y,
                                 cfg->in_fg.block_w, cfg->in_fg.block_h, &op.src_rect), TAG, "foreground block");
    ESP_RETURN_ON_ERROR(block_of(cfg->in_bg.pic_w, cfg->in_bg.pic_h, cfg->in_bg.block_offset_x, cfg->in_bg.block_offset_y,
                                 cfg->in_bg.block_w, cfg->in_bg.block_h, &op.bg_rect), TAG, "background block");
    ESP_RETURN_ON_ERROR(block_of(cfg->out.pic_w, cfg->out.pic_h, cfg->out.block_offset_x, cfg->out.block_offset_y,
                                 cfg->in_bg.block_w, cfg->in_bg.block_h, &op.dst_rect), TAG, "output block");
    b->mode = G2D_BLEND_SRC_OVER;
    const struct {
        ppa_alpha_update_mode_t mode;
        uint32_t fix;
        float scale;
        enum g2d_alpha_mode *out_mode;
        uint8_t *out_alpha;
    } layers[2] = {
        { cfg->fg_alpha_update_mode, cfg->fg_alpha_fix_val, cfg->fg_alpha_scale_ratio, &b->fg_alpha_mode, &b->fg_alpha },
        { cfg->bg_alpha_update_mode, cfg->bg_alpha_fix_val, cfg->bg_alpha_scale_ratio, &b->bg_alpha_mode, &b->bg_alpha },
    };

    for (int i = 0; i < 2; i++) {
        switch (layers[i].mode) {
        case PPA_ALPHA_NO_CHANGE:
            *layers[i].out_mode = G2D_ALPHA_PIXEL;
            *layers[i].out_alpha = 0xff;
            break;
        case PPA_ALPHA_FIX_VALUE:
            *layers[i].out_mode = G2D_ALPHA_GLOBAL;
            *layers[i].out_alpha = layers[i].fix > 255 ? 255 : layers[i].fix;
            break;
        case PPA_ALPHA_SCALE:
            *layers[i].out_mode = G2D_ALPHA_MIXED;
            *layers[i].out_alpha = (uint8_t)(layers[i].scale * 255.0f + 0.5f);
            break;
        default:
            return ESP_ERR_NOT_SUPPORTED;
        }
    }
    if (cfg->bg_ck_en) {
        b->color_key = true;
        b->color_key_min = 0xff000000u | (cfg->bg_ck_rgb_low_thres.r << 16) | (cfg->bg_ck_rgb_low_thres.g << 8) | cfg->bg_ck_rgb_low_thres.b;
        b->color_key_max = 0xff000000u | (cfg->bg_ck_rgb_high_thres.r << 16) | (cfg->bg_ck_rgb_high_thres.g << 8) | cfg->bg_ck_rgb_high_thres.b;
    }
    return submit(client, &op, cfg->mode, cfg->user_data);
}

esp_err_t ppa_do_fill(ppa_client_handle_t client, const ppa_fill_oper_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(client && cfg && client->type == PPA_OPERATION_FILL, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    struct g2d_op op = { .type = G2D_OP_FILL };
    uint32_t argb;

    ESP_RETURN_ON_ERROR(surface_of(cfg->out.buffer, cfg->out.pic_w, cfg->out.pic_h, cfg->out.fill_cm, &op.dst), TAG, "output");
    ESP_RETURN_ON_ERROR(block_of(cfg->out.pic_w, cfg->out.pic_h, cfg->out.block_offset_x, cfg->out.block_offset_y,
                                 cfg->fill_block_w, cfg->fill_block_h, &op.dst_rect), TAG, "block");
    /* the accelerator fills with ARGB8888: expand the color of the output format */
    switch (cfg->out.fill_cm) {
    case PPA_FILL_COLOR_MODE_RGB565: {
        uint32_t v = cfg->fill_color_val & 0xffff;
        uint32_t r = (v >> 11) & 0x1f, g = (v >> 5) & 0x3f, bl = v & 0x1f;

        argb = 0xff000000u | (((r << 3) | (r >> 2)) << 16) | (((g << 2) | (g >> 4)) << 8) | ((bl << 3) | (bl >> 2));
        break;
    }
    case PPA_FILL_COLOR_MODE_RGB888:
        argb = 0xff000000u | (cfg->fill_argb_color.r << 16) | (cfg->fill_argb_color.g << 8) | cfg->fill_argb_color.b;
        break;
    default:
        argb = cfg->fill_argb_color.val;
        break;
    }
    op.color = argb;
    return submit(client, &op, cfg->mode, cfg->user_data);
}
