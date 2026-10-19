/* Force-included into every file of the component: what the driver sources take from the toolchain headers of the
 * original build. */
#ifndef AIC_IDF_COMPAT_H
#define AIC_IDF_COMPAT_H

#include <stdbool.h>
#include <stdint.h>

#ifndef __maybe_unused
#define __maybe_unused __attribute__((unused))
#endif

#endif
