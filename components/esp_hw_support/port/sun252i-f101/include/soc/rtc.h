/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* the time base is the 24 MHz CLINT counter, exposed with microsecond resolution */
static inline uint32_t rtc_clk_slow_freq_get_hz(void) { return 1000000; }
#ifdef __cplusplus
}
#endif
