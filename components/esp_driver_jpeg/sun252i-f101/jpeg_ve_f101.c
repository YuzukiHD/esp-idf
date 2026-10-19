/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * JPEG decoder API (driver/jpeg_decode.h) on the video engine of the F101.
 *
 * The engine decodes baseline JPEG pictures with 4:2:0 sampling to NV12. The API outputs RGB565, RGB888
 * or gray: the conversion is done here (JFIF, full range). The output is the visible picture without
 * padding, width * height pixels.
 */
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/jpeg_decode.h"
#include "ve/ve_compat.h"
#include "ve/ve_session.h"
#include "ve/ve_mem.h"

static const char *TAG = "jpeg";

#define IMAGE_MIN_VBV       (256 * 1024)
#define IMAGE_VBV_MARGIN    (64 * 1024)

struct jpeg_decoder_t {
    int timeout_ms;
    SemaphoreHandle_t lock;         /* the engine decodes one picture at a time */
};

/* ---- header ----------------------------------------------------------------------------------- */

typedef struct {
    int width, height;
    int components;
    int h[3], v[3];
    bool baseline;
} sof_t;

static esp_err_t parse_sof(const uint8_t *p, size_t len, sof_t *out)
{
    size_t i = 2;

    if (len < 4 || p[0] != 0xff || p[1] != 0xd8) {
        return ESP_ERR_INVALID_ARG;
    }
    while (i + 4 <= len) {
        uint8_t marker;
        size_t seg;

        if (p[i] != 0xff) {
            i++;
            continue;
        }
        marker = p[i + 1];
        if (marker == 0xff) {
            i++;
            continue;
        }
        if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01 || marker == 0x00) {
            i += 2;
            continue;
        }
        seg = ((size_t)p[i + 2] << 8) | p[i + 3];
        /* start of frame, but not the DHT (C4), JPG (C8) and DAC (CC) markers */
        if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
            if (i + 10 > len) {
                return ESP_ERR_INVALID_ARG;
            }
            memset(out, 0, sizeof(*out));
            out->baseline = marker == 0xc0;
            out->height = ((int)p[i + 5] << 8) | p[i + 6];
            out->width = ((int)p[i + 7] << 8) | p[i + 8];
            out->components = p[i + 9];
            for (int c = 0; c < out->components && c < 3; c++) {
                size_t o = i + 10 + (size_t)c * 3;

                if (o + 3 > len) {
                    return ESP_ERR_INVALID_ARG;
                }
                out->h[c] = p[o + 1] >> 4;
                out->v[c] = p[o + 1] & 15;
            }
            return ESP_OK;
        }
        i += 2 + seg;
    }
    return ESP_ERR_INVALID_ARG;
}

static jpeg_down_sampling_type_t sampling_of(const sof_t *s)
{
    if (s->components == 1) {
        return JPEG_DOWN_SAMPLING_GRAY;
    }
    if (s->h[0] == 1 && s->v[0] == 1) {
        return JPEG_DOWN_SAMPLING_YUV444;
    }
    if (s->h[0] == 2 && s->v[0] == 1) {
        return JPEG_DOWN_SAMPLING_YUV422;
    }
    return JPEG_DOWN_SAMPLING_YUV420;
}

esp_err_t jpeg_decoder_get_info(const uint8_t *bit_stream, uint32_t stream_size, jpeg_decode_picture_info_t *info)
{
    sof_t sof;

    ESP_RETURN_ON_FALSE(bit_stream && stream_size && info, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    ESP_RETURN_ON_ERROR(parse_sof(bit_stream, stream_size, &sof), TAG, "not a JPEG picture");
    info->width = sof.width;
    info->height = sof.height;
    info->sample_method = sampling_of(&sof);
    return ESP_OK;
}

/* ---- engine ----------------------------------------------------------------------------------- */

esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *cfg, jpeg_decoder_handle_t *ret)
{
    ESP_RETURN_ON_FALSE(cfg && ret, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    struct jpeg_decoder_t *d = calloc(1, sizeof(*d));

    ESP_RETURN_ON_FALSE(d, ESP_ERR_NO_MEM, TAG, "no memory");
    d->timeout_ms = cfg->timeout_ms;
    d->lock = xSemaphoreCreateMutex();
    if (!d->lock) {
        free(d);
        return ESP_ERR_NO_MEM;
    }
    *ret = d;
    return ESP_OK;
}

esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t d)
{
    ESP_RETURN_ON_FALSE(d, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    vSemaphoreDelete(d->lock);
    free(d);
    return ESP_OK;
}

void *jpeg_alloc_decoder_mem(size_t size, const jpeg_decode_memory_alloc_cfg_t *cfg, size_t *allocated)
{
    size_t n = (size + 63u) & ~63u;
    void *p = heap_caps_aligned_calloc(64, 1, n, MALLOC_CAP_DEFAULT);

    if (p && allocated) {
        *allocated = n;
    }
    return p;
}

/* ---- conversion ------------------------------------------------------------------------------- */

static inline uint8_t clamp8(int v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (uint8_t)v;
}

static void yuv_to_rgb(int y, int u, int v, jpeg_yuv_rgb_conv_std_t std, int *r, int *g, int *b)
{
    int cb = u - 128, cr = v - 128;

    if (std == JPEG_YUV_RGB_CONV_STD_BT709) {
        *r = y + ((1634 * cr) >> 10);
        *g = y - ((192 * cb + 479 * cr) >> 10);
        *b = y + ((1856 * cb) >> 10);
    } else {
        *r = y + ((1436 * cr) >> 10);
        *g = y - ((352 * cb + 731 * cr) >> 10);
        *b = y + ((1815 * cb) >> 10);
    }
}

static esp_err_t convert(const struct ve_picture *pic, const jpeg_decode_cfg_t *cfg, uint8_t *out, uint32_t *out_size)
{
    const int w = pic->right - pic->left, h = pic->bottom - pic->top;
    const uint8_t *yp = pic->plane[0] + (size_t)pic->top * pic->stride + pic->left;
    const uint8_t *cp = pic->plane[1] + (size_t)(pic->top / 2) * pic->stride + (pic->left & ~1);
    const bool bgr = cfg->rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    uint8_t *o = out;

    for (int y = 0; y < h; y++) {
        const uint8_t *yr = yp + (size_t)y * pic->stride;
        const uint8_t *cr = cp + (size_t)(y / 2) * pic->stride;

        for (int x = 0; x < w; x++) {
            int Y = yr[x];

            if (cfg->output_format == JPEG_DECODE_OUT_FORMAT_GRAY) {
                *o++ = (uint8_t)Y;
                continue;
            }
            int r, g, b;

            yuv_to_rgb(Y, cr[x & ~1], cr[(x & ~1) + 1], cfg->conv_std, &r, &g, &b);
            r = clamp8(r);
            g = clamp8(g);
            b = clamp8(b);
            if (cfg->output_format == JPEG_DECODE_OUT_FORMAT_RGB565) {
                uint16_t v = bgr ? ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3) : ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

                *o++ = v & 0xff;
                *o++ = v >> 8;
            } else if (bgr) {                /* RGB888 in memory order B, G, R ("small endian") */
                *o++ = b;
                *o++ = g;
                *o++ = r;
            } else {
                *o++ = r;
                *o++ = g;
                *o++ = b;
            }
        }
    }
    *out_size = (uint32_t)(o - out);
    return ESP_OK;
}

static size_t bytes_per_pixel(jpeg_dec_output_format_t f)
{
    switch (f) {
    case JPEG_DECODE_OUT_FORMAT_RGB888:
        return 3;
    case JPEG_DECODE_OUT_FORMAT_RGB565:
        return 2;
    case JPEG_DECODE_OUT_FORMAT_GRAY:
        return 1;
    default:
        return 0;
    }
}

/* ---- decode ----------------------------------------------------------------------------------- */

esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t d, const jpeg_decode_cfg_t *cfg, const uint8_t *bit_stream, uint32_t stream_size,
                               uint8_t *outbuf, uint32_t outbuf_size, uint32_t *out_size)
{
    sof_t sof;
    struct ve_session *session = NULL;
    struct ve_picture *pic;
    esp_err_t err;

    ESP_RETURN_ON_FALSE(d && cfg && bit_stream && stream_size && outbuf && out_size, ESP_ERR_INVALID_ARG, TAG, "invalid argument");
    size_t bpp = bytes_per_pixel(cfg->output_format);

    ESP_RETURN_ON_FALSE(bpp, ESP_ERR_NOT_SUPPORTED, TAG, "output format 0x%x is not supported", (unsigned)cfg->output_format);
    ESP_RETURN_ON_ERROR(parse_sof(bit_stream, stream_size, &sof), TAG, "not a JPEG picture");
    ESP_RETURN_ON_FALSE(sof.baseline, ESP_ERR_NOT_SUPPORTED, TAG, "only baseline JPEG is supported");
    ESP_RETURN_ON_FALSE(sof.components == 3 && sampling_of(&sof) == JPEG_DOWN_SAMPLING_YUV420, ESP_ERR_NOT_SUPPORTED, TAG,
                        "only 4:2:0 sampling is supported");
    ESP_RETURN_ON_FALSE((size_t)sof.width * sof.height * bpp <= outbuf_size, ESP_ERR_INVALID_SIZE, TAG, "the output buffer is too small");
    xSemaphoreTake(d->lock, portMAX_DELAY);

    struct ve_params params = { .codec = VE_CODEC_JPEG, .width = sof.width, .height = sof.height, .image = true,
                                .stream_size = MAX(stream_size + IMAGE_VBV_MARGIN, IMAGE_MIN_VBV) };

    if (ve_session_open(&params, &session) != 0) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto out;
    }
    if (ve_session_write(session, bit_stream, stream_size, -1) != 0) {
        err = ESP_ERR_NO_MEM;
        goto out;
    }
    enum ve_step step = ve_session_step(session, false);

    if (step != VE_STEP_PROGRESS && step != VE_STEP_NEED_PICTURE) {
        ESP_LOGE(TAG, "decoding failed (%d)", step);
        err = step == VE_STEP_UNSUPPORTED ? ESP_ERR_NOT_SUPPORTED : ESP_FAIL;
        goto out;
    }
    if (ve_session_ready(session) <= 0 || !(pic = ve_session_take(session))) {
        err = ESP_FAIL;
        goto out;
    }
    ve_mem_flush(pic->plane[0], pic->size);          /* the engine wrote the picture behind the cache */
    err = convert(pic, cfg, outbuf, out_size);
    ve_session_give_back(session, pic);
out:
    if (session) {
        ve_session_close(session);
    }
    xSemaphoreGive(d->lock);
    return err;
}
