/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include "ve_compat.h"
#include <errno.h>
#include "esp_intr_alloc.h"
#include "freertos/task.h"

#include "ve_hw.h"


#define VE_BASE		0x01c0e000U
#define VE_IRQN		66
#define VE_REG(off)	(VE_BASE + (off))

/*
 * Common block (register guide: https://linux-sunxi.org/VE_Register_guide). The decoder number at the bottom of the mode register chooses which decoder block
 * answers at 0x100 and 0x200; 7 puts the decoders in reset. Pictures wider than 2047 pixels need
 * the memory mode bits above it.
 */
#define TOP_MODE	0x00	/* [3:0] decoder number, [22:21] memory mode */
#define TOP_CTRL	0x04	/* [16] decoder reset (pulsed), [9] idle */
#define TOP_FLAGS	0x1c	/* engine status; two bits cleared by a reset */

/*
 * 0xe8 and 0xec control the format the decoders write (tiled macroblocks unless set otherwise)
 * and are cleared by a reset, so the pictures come out tiled and ve_hw_convert() makes them
 * linear. Read back after the identification word shows, they hold the version of the engine.
 */
#define TOP_OUT_CTRL	0xe8
#define TOP_OUT_FORMAT	0xec
#define TOP_VERSION_A	0xe8
#define TOP_VERSION_B	0xec
#define TOP_VERSION_ID	0xf0
#define TOP_VERSION_MAGIC 0x19370040U

/*
 * Where the line buffers of the H.264 decoder (deblocking, intra prediction) live is chosen at
 * 0x50, their memory addresses are at 0x54 and 0x58 (written by ve_h264_hw.c); 0xc4..0xce hold the
 * offset of the second chroma plane and the line strides of the picture. A reset puts all of
 * them back to zero.
 */
#define TOP_LINE_BUF_CTRL	0x50
#define TOP_LINE_BUF_ADDR	0x54

/*
 * Picture conversion block: reads a picture in tiled macroblocks (luma at CV_IN_Y, chroma at
 * CV_IN_C) and writes it as linear NV12 (CV_OUT_Y, the chroma plane follows the luma plane).
 * Sizes are in macroblocks of 16x16 pixels. It raises the same interrupt as the decoders.
 */
#define CV_SIZE		0xa00
#define CV_STRIDE	0xa04
#define CV_ENABLE	0xa08
#define CV_MODE		0xa0c
#define CV_STATUS	0xa10
#define CV_LINE		0xa14
#define CV_CONFIG	0xa2c
#define CV_OUT_Y	0xa70
#define CV_OUT_C	0xa74
#define CV_IN_Y		0xa78
#define CV_IN_C		0xa7c

/*
 * Control and status of the decoder blocks, to tell which block raised the interrupt. Status
 * bits (write 1 to clear): 0 finished, 1 error, 2 wants more bit stream; bits 3..5 of the
 * control register enable the matching interrupts.
 */
#define H264_CTRL	0x220
#define H264_STATUS	0x228
#define JPEG_CTRL	0x114
#define JPEG_STATUS	0x11c

/* decoder numbers of the top mode register; this chip inverts bit 3 */
#define TOP_DECODER_JPEG	0U
#define TOP_DECODER_H264	1U
#define TOP_DECODER_OFF		7U
#define TOP_DECODER_INVERT	8U

/* on chip memory the engine may use */
#define SRAM_CTRL_BASE	0x03000000U

/* clocks: the module clock register of the clock controller and the PLL it is fed from */
#define CCU_BASE	0x02001000U
#define CLK_REG		(CCU_BASE + 0x690)
#define BUS_REG		(CCU_BASE + 0x69c)	/* bus gate bit 0, reset bit 16 */
#define MBUS_REG	(CCU_BASE + 0x804)	/* memory bus gate bit 1 */
#define ALLWINNER_CCU_PLL_PERI_N_MASK		(0xffu << 8)
#define ALLWINNER_CCU_PLL_PERI_P0_MASK		(0x7u << 16)
#define ALLWINNER_CCU_PLL_PERI_INPUT_DIV2	(1u << 1)
#define ALLWINNER_CCU_PLL_PERI_DIV_MASK		(0x7u << 2)
#define PLL_PERI_REG	(CCU_BASE + 0x20)
#define HOSC_RATE	24000000U
#define MOD_GATE	BIT(31)
#define MOD_MUX_MASK	GENMASK(26, 24)
#define MOD_DIV_MASK	GENMASK(4, 0)
#define MOD_MUX_PERI_480M 0

static struct {
	struct k_mutex mutex;
	struct k_mutex init_lock;
	SemaphoreHandle_t done;
	int users;
	bool irq_ready;
	intr_handle_t intr;
	uint64_t version;
} ve;

/* ---- interrupt ---- */

static void ve_isr(void *arg)
{
	bool done = false;
	BaseType_t woken = pdFALSE;

	ARG_UNUSED(arg);
	if ((sys_read32(VE_REG(CV_ENABLE)) & 1U) && (sys_read32(VE_REG(CV_STATUS)) & 1U)) {
		sys_write32(sys_read32(VE_REG(CV_ENABLE)) & ~1U, VE_REG(CV_ENABLE));
		done = true;
	} else if ((sys_read32(VE_REG(H264_CTRL)) & 0xfU) &&
		   (sys_read32(VE_REG(H264_STATUS)) & 0xfU)) {
		sys_write32(sys_read32(VE_REG(H264_CTRL)) & ~0xfU, VE_REG(H264_CTRL));
		done = true;
	} else if ((sys_read32(VE_REG(JPEG_CTRL)) & 0x7cU) &&
		   (sys_read32(VE_REG(JPEG_STATUS)) & 0xfU)) {
		sys_write32(sys_read32(VE_REG(JPEG_CTRL)) & ~0x7dU, VE_REG(JPEG_CTRL));
		done = true;
	}
	if (done && uxSemaphoreGetCount(ve.done) == 0) {
		xSemaphoreGiveFromISR(ve.done, &woken);
	}
	if (woken) {
		portYIELD_FROM_ISR();
	}
}

int ve_hw_wait(void)
{
	if (xSemaphoreTake(ve.done, pdMS_TO_TICKS(1000)) != pdTRUE) {
		LOG_WRN("interrupt timeout: top %08x %08x %08x", sys_read32(VE_REG(TOP_MODE)),
			sys_read32(VE_REG(TOP_CTRL)), sys_read32(VE_REG(TOP_FLAGS)));
		return -ETIMEDOUT;
	}

	return 0;
}

/* ---- clock ---- */

/* Rate of the 480 MHz output of the peripheral PLL */
static uint32_t pll_peri_480m(void)
{
	uint32_t pll = sys_read32(PLL_PERI_REG);
	uint32_t n = FIELD_GET(ALLWINNER_CCU_PLL_PERI_N_MASK, pll) + 1;
	uint32_t p0 = FIELD_GET(ALLWINNER_CCU_PLL_PERI_P0_MASK, pll) + 1;
	uint32_t m = (pll & ALLWINNER_CCU_PLL_PERI_INPUT_DIV2) ? 2 : 1;
	uint32_t raw = HOSC_RATE / m / p0 * n;

	return raw / (FIELD_GET(ALLWINNER_CCU_PLL_PERI_DIV_MASK, pll) + 1);
}

static void module_clock_gate(bool on)
{
	uint32_t reg = sys_read32(CLK_REG);

	sys_write32(on ? reg | MOD_GATE : reg & ~MOD_GATE, CLK_REG);
}

static void bus_gates(bool on)
{
	if (on) {
		sys_write32(sys_read32(BUS_REG) | BIT(0), BUS_REG);
		sys_write32(sys_read32(MBUS_REG) | BIT(1), MBUS_REG);
	} else {
		sys_write32(sys_read32(BUS_REG) & ~BIT(0), BUS_REG);
		sys_write32(sys_read32(MBUS_REG) & ~BIT(1), MBUS_REG);
	}
}

static void ve_reset_deassert(void)
{
	sys_write32(sys_read32(BUS_REG) | BIT(16), BUS_REG);
}

static void ve_reset_assert(void)
{
	sys_write32(sys_read32(BUS_REG) & ~BIT(16), BUS_REG);
}

static int set_clock(unsigned int mhz)
{
	uint32_t parent = pll_peri_480m();
	uint32_t freq = mhz * 1000000U;
	uint32_t div, reg;

	if (parent == 0U || freq == 0U) {
		return -EIO;
	}
	div = CLAMP((parent + freq / 2) / freq, 1U, MOD_DIV_MASK + 1U);
	module_clock_gate(false);
	reg = sys_read32(CLK_REG) & ~(MOD_MUX_MASK | MOD_DIV_MASK);
	sys_write32(reg | FIELD_PREP(MOD_MUX_MASK, MOD_MUX_PERI_480M) | (div - 1U), CLK_REG);
	module_clock_gate(true);

	return 0;
}

int ve_hw_set_clock(unsigned int mhz)
{
	int ret;

	k_mutex_lock(&ve.mutex, K_FOREVER);
	ret = set_clock(mhz);
	k_mutex_unlock(&ve.mutex);

	return ret;
}

/* ---- registers of the top block ---- */

/* The version the chip reports in its top block; zero when it does not answer */
static uint64_t read_version(void)
{
	for (int i = 0; i < 4; i++) {
		if (sys_read32(VE_REG(TOP_VERSION_ID)) == TOP_VERSION_MAGIC) {
			uint32_t lo = sys_read32(VE_REG(TOP_VERSION_B)) & 0x7ffffU;
			uint32_t hi = sys_read32(VE_REG(TOP_VERSION_A)) & 0x7ffffU;

			if (lo != 0 && hi != 0) {
				return ((uint64_t)hi << 32) | lo;
			}
		}
		vTaskDelay(1);
	}

	return 0;
}

uint64_t ve_hw_version(void)
{
	return ve.version;
}

volatile uint8_t *ve_hw_regs(uint32_t offset)
{
	return (volatile uint8_t *)(uintptr_t)(VE_BASE + offset);
}

static void set_top_decoder(uint32_t number)
{
	sys_write32((sys_read32(VE_REG(TOP_MODE)) & ~0xfU) | number, VE_REG(TOP_MODE));
}

static void select_decoder(enum ve_hw_decoder decoder)
{
	switch (decoder) {
	case VE_HW_JPEG:
		set_top_decoder(TOP_DECODER_JPEG ^ TOP_DECODER_INVERT);
		break;
	case VE_HW_H264:
		set_top_decoder(TOP_DECODER_H264 ^ TOP_DECODER_INVERT);
		break;
	default:
		break;
	}
}

/* memory mode bits of the top register depend on the picture width */
static void set_memory_mode(int width)
{
	uint32_t v = sys_read32(VE_REG(TOP_MODE));

	if (width >= 4096) {
		v |= BIT(21) | BIT(22);
	} else if (width > 2047) {
		v = (v | BIT(21)) & ~BIT(22);
	} else {
		v &= ~(BIT(21) | BIT(22));
	}
	sys_write32(v, VE_REG(TOP_MODE));
}

void ve_hw_select(enum ve_hw_decoder decoder)
{
	k_mutex_lock(&ve.mutex, K_FOREVER);
	select_decoder(decoder);
	k_mutex_unlock(&ve.mutex);
}

void ve_hw_idle(void)
{
	k_mutex_lock(&ve.mutex, K_FOREVER);
	set_top_decoder(TOP_DECODER_OFF);
	k_mutex_unlock(&ve.mutex);
}

void ve_hw_reset(enum ve_hw_decoder decoder, int width)
{
	k_mutex_lock(&ve.mutex, K_FOREVER);
	set_top_decoder(TOP_DECODER_OFF);
	sys_write32(sys_read32(VE_REG(TOP_CTRL)) | BIT(16), VE_REG(TOP_CTRL));	/* reset pulse */
	sys_write32(sys_read32(VE_REG(TOP_CTRL)) & ~BIT(16), VE_REG(TOP_CTRL));
	select_decoder(decoder);
	sys_write32(sys_read32(VE_REG(TOP_FLAGS)) & ~(BIT(8) | BIT(10)), VE_REG(TOP_FLAGS));
	/* output side registers back to their idle values */
	sys_write32(0, VE_REG(0xc4));
	sys_write16(0, VE_REG(0xc8));
	sys_write16(0, VE_REG(0xca));
	sys_write16(0, VE_REG(0xcc));
	sys_write16(0, VE_REG(0xce));
	sys_write32(0, VE_REG(TOP_OUT_CTRL));
	sys_write32(sys_read32(VE_REG(TOP_OUT_FORMAT)) & ~0x77U, VE_REG(TOP_OUT_FORMAT));
	sys_write32(sys_read32(VE_REG(TOP_LINE_BUF_CTRL)) & ~0xfU, VE_REG(TOP_LINE_BUF_CTRL));
	sys_write32(0, VE_REG(TOP_LINE_BUF_ADDR));
	set_memory_mode(width);
	k_mutex_unlock(&ve.mutex);
}

void ve_hw_lock(void)
{
	k_mutex_lock(&ve.mutex, K_FOREVER);
}

void ve_hw_unlock(void)
{
	k_mutex_unlock(&ve.mutex);
}

/* ---- power ---- */

static int power_up(void)
{
	if (!ve.irq_ready) {
		ve.done = xSemaphoreCreateBinary();
		if (!ve.done || esp_intr_alloc(VE_IRQN, 0, ve_isr, NULL, &ve.intr) != ESP_OK) {
			LOG_ERR("no interrupt for the video engine");
			return -ENOMEM;
		}
		ve.irq_ready = true;
	}
	xSemaphoreTake(ve.done, 0);

	ve_reset_deassert();
	module_clock_gate(true);
	/* the engine may use the on chip memory */
	sys_write32(sys_read32(SRAM_CTRL_BASE) | 0x7fffffffU, SRAM_CTRL_BASE);
	sys_write32(sys_read32(SRAM_CTRL_BASE + 4) & ~BIT(24), SRAM_CTRL_BASE + 4);
	bus_gates(true);
	ve.version = read_version();
	if (ve.version == 0) {
		LOG_ERR("the video engine does not report its version");
	}
	/* reset the engine once, then set the clock */
	ve_reset_assert();
	ve_reset_deassert();
	if (set_clock(CONFIG_VDEC_SUNXI_CLOCK_MHZ) != 0) {
		LOG_ERR("cannot set the video engine clock");
	}

	return 0;
}

int ve_hw_get(void)
{
	int ret = 0;

	k_mutex_lock(&ve.init_lock, K_FOREVER);
	if (ve.users == 0) {
		ret = power_up();
	}
	if (ret == 0) {
		ve.users++;
	}
	k_mutex_unlock(&ve.init_lock);

	return ret;
}

void ve_hw_put(void)
{
	k_mutex_lock(&ve.init_lock, K_FOREVER);
	if (ve.users > 0) {
		ve.users--;
	}
	k_mutex_unlock(&ve.init_lock);
}

/* ---- picture conversion ---- */

int ve_hw_convert(uint32_t in_y, uint32_t in_c, uint32_t out_y, int width, int height)
{
	uint32_t mbw = (uint32_t)width / 16U;
	uint32_t mbh = (uint32_t)height / 16U;

	k_mutex_lock(&ve.mutex, K_FOREVER);
	sys_write32(1, VE_REG(TOP_CTRL));
	sys_write32(0, VE_REG(TOP_CTRL));
	sys_write32(2, VE_REG(TOP_MODE));
	sys_write32(1, VE_REG(CV_STATUS));
	sys_write32((mbw << 16) | mbh, VE_REG(CV_SIZE));
	sys_write32((mbw << 16) | mbw, VE_REG(CV_STRIDE));
	sys_write32(mbw, VE_REG(CV_LINE));
	sys_write32(0x50080001U, VE_REG(CV_ENABLE));
	sys_write32((mbh << 16) | mbw, VE_REG(CV_CONFIG));
	sys_write32(0, VE_REG(0xa30));
	sys_write32(0, VE_REG(0xa34));
	sys_write32(0, VE_REG(0xa38));
	sys_write32(out_y, VE_REG(CV_OUT_Y));
	/* the chroma plane follows the luma plane */
	sys_write32(out_y + (uint32_t)(width * height), VE_REG(CV_OUT_C));
	sys_write32(in_y, VE_REG(CV_IN_Y));
	sys_write32(in_c, VE_REG(CV_IN_C));
	sys_write32(1, VE_REG(CV_MODE));
	if (ve_hw_wait() != 0) {
		LOG_ERR("picture conversion timed out");
		k_mutex_unlock(&ve.mutex);
		return -ETIMEDOUT;
	}
	sys_write32(1, VE_REG(CV_STATUS));
	k_mutex_unlock(&ve.mutex);
	ve_hw_reset(VE_HW_NONE, width);

	return 0;
}
