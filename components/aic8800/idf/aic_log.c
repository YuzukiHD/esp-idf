/*
 * SPDX-FileCopyrightText: 2026 Yuzuki Tsuru
 * SPDX-License-Identifier: Apache-2.0
 *
 * The driver prints in pieces, so the text is collected until a newline and then logged as one line.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "aic_log_redirect.h"

#undef printf
#undef printk

#define LINE_MAX_LEN 255

static const char *TAG = "aic8800";
static char pending[LINE_MAX_LEN + 1];
static size_t pending_len;

static bool has_word(const char *line, const char *word)
{
    size_t n = strlen(word);

    for (; *line; line++) {
        if (strncasecmp(line, word, n) == 0) {
            return true;
        }
    }
    return false;
}

static void emit(unsigned int level, const char *line)
{
    if (level > AIC_LVL_ERR && (has_word(line, "fail") || has_word(line, "error"))) {
        level = AIC_LVL_ERR;
    }
    switch (level) {
    case AIC_LVL_ERR:
        ESP_LOGE(TAG, "%s", line);
        break;
    case AIC_LVL_WRN:
        ESP_LOGW(TAG, "%s", line);
        break;
    case AIC_LVL_DBG:
        ESP_LOGD(TAG, "%s", line);
        break;
    default:
        ESP_LOGI(TAG, "%s", line);
        break;
    }
}

int aic_log(unsigned int level, const char *fmt, ...)
{
    char text[LINE_MAX_LEN + 1];
    va_list ap;

    va_start(ap, fmt);
    int n = vsnprintf(text, sizeof(text), fmt, ap);

    va_end(ap);
    if (n < 0) {
        return n;
    }
    for (const char *p = text; *p; p++) {
        if (*p == '\n' || pending_len >= LINE_MAX_LEN) {
            pending[pending_len] = '\0';
            if (pending_len) {
                emit(level, pending);
            }
            pending_len = 0;
            if (*p == '\n') {
                continue;
            }
        }
        if (*p != '\r') {
            pending[pending_len++] = *p;
        }
    }
    return n;
}
