// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - logging. The messages go through the IDF log
 * (tag "display"), the pipeline tag becomes the message prefix.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include <dpy/dpy_log.h>

static const char *TAG = "display";

/* extra runtime verbosity cap */
int dpy_log_level = DPY_LOG_INFO;

void dpy_log(int level, const char *tag, const char *fmt, ...)
{
	char msg[192];
	va_list ap;
	size_t len;

	if (level > dpy_log_level)
		return;

	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	/* the pipeline messages carry their own line ending */
	len = strlen(msg);
	while (len && (msg[len - 1] == '\n' || msg[len - 1] == '\r'))
		msg[--len] = '\0';

	switch (level) {
	case DPY_LOG_ERR:
		ESP_LOGE(TAG, "%s: %s", tag, msg);
		break;
	case DPY_LOG_WARN:
		ESP_LOGW(TAG, "%s: %s", tag, msg);
		break;
	case DPY_LOG_INFO:
		ESP_LOGI(TAG, "%s: %s", tag, msg);
		break;
	default:
		ESP_LOGD(TAG, "%s: %s", tag, msg);
		break;
	}
}
