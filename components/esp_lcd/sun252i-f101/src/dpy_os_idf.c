// SPDX-License-Identifier: Apache-2.0 OR GPL-2.0-or-later
/*
 * Display pipeline framework - OS port for ESP-IDF on the Allwinner F101.
 *
 * Implements every function of sunxi/include/dpy/dpy_os.h. The display
 * clocks and resets (PLL_VIDEO0, DE, TCON, DPSS_TOP, DSI, combo PHY) and
 * the backlight PWM block are driven here directly through the registers.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_intr_alloc.h"
#include "esp_heap_caps.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#include <dpy/dpy_os.h>
#include <sunxi_ccu/sun252i-f101-ccu.h>
#include "soc/sun252i_f101_ll.h"
#include "f101_cache.h"

#define GENMASK(h, l)		((~0UL << (l)) & (~0UL >> (31 - (h))))
#define ARRAY_SIZE(a)		(sizeof(a) / sizeof((a)[0]))
#define CLAMP(v, lo, hi)	((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define DIV_ROUND_CLOSEST(a, b)	(((a) + (b) / 2) / (b))

#define DPY_CACHE_LINE		64
#define DPY_CCU_BASE		0x02001000UL
#define DPY_HOSC_HZ		24000000U

/* ------------------------------------------------------------------ */
/* Memory                                                              */
/* ------------------------------------------------------------------ */
void *dpy_os_zalloc(size_t size)
{
	return calloc(1, size);
}

void dpy_os_free(void *ptr)
{
	free(ptr);
}

void *dpy_os_dma_alloc(size_t size, size_t align, dpy_dma_addr_t *dma)
{
	void *p;

	if (align < DPY_CACHE_LINE) {
		align = DPY_CACHE_LINE;
	}
	/* round the size up so cache maintenance never touches neighbours */
	size = (size + DPY_CACHE_LINE - 1) & ~(size_t)(DPY_CACHE_LINE - 1);
	p = heap_caps_aligned_alloc(align, size, MALLOC_CAP_DEFAULT);
	if (!p) {
		return NULL;
	}
	memset(p, 0, size);
	f101_dcache_clean(p, size);
	if (dma) {
		*dma = (dpy_dma_addr_t)(uintptr_t)p;
	}
	return p;
}

void dpy_os_dma_free(void *ptr)
{
	free(ptr);
}

dpy_dma_addr_t dpy_os_virt_to_dma(const void *ptr)
{
	/* the SoC has no MMU: virtual == physical */
	return (dpy_dma_addr_t)(uintptr_t)ptr;
}

void dpy_os_dcache_clean(const void *ptr, size_t size)
{
	if (size) {
		f101_dcache_clean(ptr, size);
	}
}

void dpy_os_dcache_invalidate(void *ptr, size_t size)
{
	if (size) {
		f101_dcache_invalidate(ptr, size);
	}
}

uintptr_t dpy_os_ioremap(uintptr_t phys, size_t size)
{
	(void)size;
	return phys;
}

/* ------------------------------------------------------------------ */
/* Time                                                                */
/* ------------------------------------------------------------------ */
void dpy_os_udelay(uint32_t us)
{
	f101_delay_us(us);
}

void dpy_os_msleep(uint32_t ms)
{
	if (dpy_os_in_irq() || xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
		f101_delay_us(ms * 1000U);
		return;
	}
	vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

uint64_t dpy_os_time_us(void)
{
	return (uint64_t)esp_timer_get_time();
}

/* ------------------------------------------------------------------ */
/* Synchronisation                                                     */
/* ------------------------------------------------------------------ */
struct dpy_mutex {
	SemaphoreHandle_t m;
};

struct dpy_sem {
	SemaphoreHandle_t s;
};

struct dpy_mutex *dpy_os_mutex_create(void)
{
	struct dpy_mutex *m = calloc(1, sizeof(*m));

	if (m) {
		m->m = xSemaphoreCreateRecursiveMutex();
		if (!m->m) {
			free(m);
			return NULL;
		}
	}
	return m;
}

void dpy_os_mutex_destroy(struct dpy_mutex *m)
{
	if (m) {
		vSemaphoreDelete(m->m);
		free(m);
	}
}

void dpy_os_mutex_lock(struct dpy_mutex *m)
{
	xSemaphoreTakeRecursive(m->m, portMAX_DELAY);
}

void dpy_os_mutex_unlock(struct dpy_mutex *m)
{
	xSemaphoreGiveRecursive(m->m);
}

struct dpy_sem *dpy_os_sem_create(uint32_t initial)
{
	struct dpy_sem *s = calloc(1, sizeof(*s));

	if (s) {
		s->s = xSemaphoreCreateCounting(0x7fff, initial);
		if (!s->s) {
			free(s);
			return NULL;
		}
	}
	return s;
}

void dpy_os_sem_destroy(struct dpy_sem *s)
{
	if (s) {
		vSemaphoreDelete(s->s);
		free(s);
	}
}

void dpy_os_sem_post(struct dpy_sem *s)
{
	if (xPortInIsrContext()) {
		BaseType_t woken = pdFALSE;

		xSemaphoreGiveFromISR(s->s, &woken);
		portYIELD_FROM_ISR(woken);
	} else {
		xSemaphoreGive(s->s);
	}
}

int dpy_os_sem_wait(struct dpy_sem *s, uint32_t timeout_ms)
{
	TickType_t to;

	if (timeout_ms == DPY_WAIT_FOREVER) {
		to = portMAX_DELAY;
	} else if (!timeout_ms) {
		to = 0;
	} else {
		to = pdMS_TO_TICKS(timeout_ms);
		if (!to) {
			to = 1;
		}
	}
	return xSemaphoreTake(s->s, to) == pdTRUE ? 0 : -ETIMEDOUT;
}

void dpy_os_spin_init(dpy_spinlock_t *lock)
{
	memset(lock, 0, sizeof(*lock));
}

unsigned long dpy_os_spin_lock_irqsave(dpy_spinlock_t *lock)
{
	(void)lock;
	/* single core: masking interrupts is the whole critical section */
	return (unsigned long)portSET_INTERRUPT_MASK_FROM_ISR();
}

void dpy_os_spin_unlock_irqrestore(dpy_spinlock_t *lock, unsigned long flags)
{
	(void)lock;
	portCLEAR_INTERRUPT_MASK_FROM_ISR((UBaseType_t)flags);
}

bool dpy_os_in_irq(void)
{
	return xPortInIsrContext() != 0;
}

/* ------------------------------------------------------------------ */
/* Interrupts                                                          */
/* ------------------------------------------------------------------ */
#define DPY_OS_MAX_IRQS 4

static struct {
	int irq;
	dpy_irq_handler_t handler;
	void *data;
	intr_handle_t h;
} dpy_os_irqs[DPY_OS_MAX_IRQS];

static void dpy_os_irq_trampoline(void *arg)
{
	uintptr_t slot = (uintptr_t)arg;

	dpy_os_irqs[slot].handler(dpy_os_irqs[slot].data);
}

/* @irq is the PLIC source number */
int dpy_os_request_irq(int irq, dpy_irq_handler_t handler, const char *name, void *data)
{
	uintptr_t i;

	(void)name;
	for (i = 0; i < DPY_OS_MAX_IRQS; i++) {
		if (!dpy_os_irqs[i].handler) {
			break;
		}
	}
	if (i == DPY_OS_MAX_IRQS) {
		return -ENOSPC;
	}
	dpy_os_irqs[i].irq = irq;
	dpy_os_irqs[i].data = data;
	dpy_os_irqs[i].handler = handler;
	if (esp_intr_alloc(irq, ESP_INTR_FLAG_LEVEL1, dpy_os_irq_trampoline, (void *)i, &dpy_os_irqs[i].h) != ESP_OK) {
		dpy_os_irqs[i].handler = NULL;
		return -EIO;
	}
	return 0;
}

void dpy_os_free_irq(int irq, void *data)
{
	int i;

	for (i = 0; i < DPY_OS_MAX_IRQS; i++) {
		if (dpy_os_irqs[i].handler && dpy_os_irqs[i].irq == irq && dpy_os_irqs[i].data == data) {
			esp_intr_free(dpy_os_irqs[i].h);
			dpy_os_irqs[i].handler = NULL;
			return;
		}
	}
}

/* ------------------------------------------------------------------ */
/* Clocks and resets                                                   */
/* ------------------------------------------------------------------ */
struct dpy_clk {
	uint32_t id;
};

struct dpy_reset {
	uint32_t id;
};

#define PLL_PERI_REG		0x0020
/* PLL_VIDEO0 (0x40): N[15:8], LDO bit 30, enable 31, output gate 27, lock 29/28 */
#define PLL_VIDEO0_REG		0x0040
#define PLL_VIDEO0_N_SHIFT	8
#define PLL_VIDEO0_N_MASK	(0xffUL << PLL_VIDEO0_N_SHIFT)
#define PLL_VIDEO0_INPUT_DIV2	BIT(1)
#define PLL_VIDEO0_LDO		BIT(30)
#define PLL_VIDEO0_EN		BIT(31)
#define PLL_VIDEO0_OUT		BIT(27)
#define PLL_VIDEO0_LOCK_EN	BIT(29)
#define PLL_VIDEO0_LOCKED	BIT(28)
#define PLL_VIDEO0_MIN_HZ	288000000U
#define PLL_VIDEO0_MAX_HZ	2400000000U

#define MAX_PARENTS 4

enum dpy_clk_kind {
	CLK_KIND_PLL_PERI_2X,
	CLK_KIND_PLL_PERI_1X,
	CLK_KIND_PLL_VIDEO0_4X,
	CLK_KIND_MUXDIV,	/* gate 31, mux 26:24, linear div at bit 0 */
	CLK_KIND_MP,		/* gate 31, mux 26:24, P 9:8, M 3:0 (TCON) */
	CLK_KIND_BUS,		/* gate at bit 0 */
};

struct dpy_clk_desc {
	uint32_t id;
	uint8_t kind;
	uint8_t div_width;
	uint16_t reg;
	uint32_t parents[MAX_PARENTS];
};

static const struct dpy_clk_desc dpy_clks[] = {
	{ CLK_PLL_PERI_2X, CLK_KIND_PLL_PERI_2X },
	{ CLK_PLL_PERI_1X, CLK_KIND_PLL_PERI_1X },
	{ CLK_PLL_VIDEO0_4X, CLK_KIND_PLL_VIDEO0_4X },
	{ CLK_DE, CLK_KIND_MUXDIV, 5, 0x0600, { CLK_PLL_PERI_2X, CLK_PLL_VIDEO0_4X } },
	{ CLK_BUS_DE, CLK_KIND_BUS, 0, 0x060c },
	{ CLK_COMBOPHY0, CLK_KIND_MUXDIV, 5, 0x0aa0, { CLK_PLL_VIDEO0_4X, CLK_PLL_PERI_2X } },
	{ CLK_BUS_COMBOPHY0, CLK_KIND_BUS, 0, 0x0aa4 },
	{ CLK_BUS_DPSS_TOP, CLK_KIND_BUS, 0, 0x0abc },
	{ CLK_DSI, CLK_KIND_MUXDIV, 4, 0x0b24, { 0, CLK_PLL_PERI_1X } },
	{ CLK_BUS_DSI, CLK_KIND_BUS, 0, 0x0b4c },
	/* TCON: 0 = VIDEO0_1X (4X / 4), 1 = VIDEO0_4X, 2 = PERI_2X */
	{ CLK_TCONLCD, CLK_KIND_MP, 0, 0x0b60, { 0, CLK_PLL_VIDEO0_4X, CLK_PLL_PERI_2X } },
	{ CLK_BUS_TCONLCD, CLK_KIND_BUS, 0, 0x0b7c },
};

static struct dpy_clk dpy_clk_pool[ARRAY_SIZE(dpy_clks)];

struct dpy_reset_desc {
	uint32_t id;
	uint16_t reg;
};

static const struct dpy_reset_desc dpy_resets[] = {
	{ RST_BUS_DE, 0x060c },
	{ RST_BUS_DPSS_TOP, 0x0abc },
	{ RST_BUS_DSI, 0x0b4c },
	{ RST_BUS_TCONLCD, 0x0b7c },
	{ RST_BUS_LVDS0, 0x0bac },
};

static struct dpy_reset dpy_reset_pool[ARRAY_SIZE(dpy_resets)];

static uint32_t ccu_rd(uint32_t off)
{
	return F101_REG32(DPY_CCU_BASE + off);
}

static void ccu_wr(uint32_t off, uint32_t val)
{
	F101_REG32(DPY_CCU_BASE + off) = val;
}

static void ccu_upd(uint32_t off, uint32_t mask, uint32_t val)
{
	ccu_wr(off, (ccu_rd(off) & ~mask) | (val & mask));
}

static const struct dpy_clk_desc *clk_desc(const struct dpy_clk *clk)
{
	return clk ? &dpy_clks[clk - dpy_clk_pool] : NULL;
}

static struct dpy_clk *clk_by_id(uint32_t id)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(dpy_clks); i++) {
		if (dpy_clks[i].id == id) {
			return &dpy_clk_pool[i];
		}
	}
	return NULL;
}

static uint32_t pll_peri_rate(void)
{
	uint32_t pll = ccu_rd(PLL_PERI_REG);
	uint32_t n = ((pll >> 8) & 0xff) + 1;
	uint32_t p0 = ((pll >> 16) & 0x7) + 1;
	uint32_t m = (pll & BIT(1)) ? 2 : 1;

	return DPY_HOSC_HZ / m / p0 * n;
}

static uint32_t pll_video0_rate(void)
{
	uint32_t reg = ccu_rd(PLL_VIDEO0_REG);
	uint32_t n = ((reg & PLL_VIDEO0_N_MASK) >> PLL_VIDEO0_N_SHIFT) + 1;

	return DPY_HOSC_HZ * n / ((reg & PLL_VIDEO0_INPUT_DIV2) ? 2 : 1);
}

static uint32_t clk_parent_rate(const struct dpy_clk_desc *d, uint8_t mux)
{
	uint32_t pid = mux < MAX_PARENTS ? d->parents[mux] : 0;

	if (d->id == CLK_TCONLCD && mux == 0) {
		return pll_video0_rate() / 4;
	}
	switch (pid) {
	case CLK_PLL_PERI_2X:
		return pll_peri_rate();
	case CLK_PLL_PERI_1X:
		return pll_peri_rate() / 2;
	case CLK_PLL_VIDEO0_4X:
		return pll_video0_rate();
	default:
		return d->id == CLK_DSI && mux == 0 ? DPY_HOSC_HZ : 0;
	}
}

struct dpy_clk *dpy_os_clk_get(uint32_t controller, uint32_t id)
{
	if (controller != ALLWINNER_CCU_MAIN) {
		return NULL;
	}
	return clk_by_id(id);
}

void dpy_os_clk_put(struct dpy_clk *clk)
{
	(void)clk;
}

static void pll_video0_enable(void)
{
	uint32_t reg = ccu_rd(PLL_VIDEO0_REG);
	int tries = 10000;

	if ((reg & PLL_VIDEO0_EN) && (reg & PLL_VIDEO0_OUT)) {
		return;
	}
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_LDO, PLL_VIDEO0_LDO);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_EN, PLL_VIDEO0_EN);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_LOCK_EN, PLL_VIDEO0_LOCK_EN);
	while (tries-- && !(ccu_rd(PLL_VIDEO0_REG) & PLL_VIDEO0_LOCKED)) {
		f101_delay_us(10);
	}
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_OUT, PLL_VIDEO0_OUT);
}

static void pll_video0_disable(void)
{
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_OUT, 0);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_EN, 0);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_LDO, 0);
}

int dpy_os_clk_enable(struct dpy_clk *clk)
{
	const struct dpy_clk_desc *d = clk_desc(clk);

	if (!d) {
		return 0;
	}
	switch (d->kind) {
	case CLK_KIND_PLL_VIDEO0_4X:
		pll_video0_enable();
		break;
	case CLK_KIND_MUXDIV:
	case CLK_KIND_MP:
		{
			uint32_t mux = (ccu_rd(d->reg) >> 24) & 0x7;

			if (mux < MAX_PARENTS &&
			    (d->parents[mux] == CLK_PLL_VIDEO0_4X || (d->id == CLK_TCONLCD && mux == 0))) {
				pll_video0_enable();
			}
		}
		ccu_upd(d->reg, BIT(31), BIT(31));
		break;
	case CLK_KIND_BUS:
		ccu_upd(d->reg, BIT(0), BIT(0));
		break;
	default:
		break;
	}
	return 0;
}

void dpy_os_clk_disable(struct dpy_clk *clk)
{
	const struct dpy_clk_desc *d = clk_desc(clk);

	if (!d) {
		return;
	}
	switch (d->kind) {
	case CLK_KIND_MUXDIV:
	case CLK_KIND_MP:
		ccu_upd(d->reg, BIT(31), 0);
		break;
	case CLK_KIND_BUS:
		ccu_upd(d->reg, BIT(0), 0);
		break;
	default:
		break;
	}
}

int dpy_os_clk_set_parent(struct dpy_clk *clk, struct dpy_clk *parent)
{
	const struct dpy_clk_desc *d = clk_desc(clk);
	const struct dpy_clk_desc *p = clk_desc(parent);
	uint32_t mux;

	if (!d || !p) {
		return 0;
	}
	if (d->kind != CLK_KIND_MUXDIV && d->kind != CLK_KIND_MP) {
		return -EINVAL;
	}
	for (mux = 0; mux < MAX_PARENTS; mux++) {
		if (d->parents[mux] == p->id) {
			ccu_upd(d->reg, 0x7UL << 24, mux << 24);
			return 0;
		}
	}
	return -EINVAL;
}

static uint32_t pll_video0_pick(uint32_t hz)
{
	uint32_t n;

	hz = CLAMP(hz, PLL_VIDEO0_MIN_HZ, PLL_VIDEO0_MAX_HZ);
	n = DIV_ROUND_CLOSEST(hz, DPY_HOSC_HZ);
	return CLAMP(n, 1U, 256U);
}

static int pll_video0_set_rate(uint32_t hz)
{
	uint32_t n = pll_video0_pick(hz);
	bool was_on = ccu_rd(PLL_VIDEO0_REG) & PLL_VIDEO0_EN;

	if (was_on) {
		pll_video0_disable();
	}
	/* the input divider must be cleared: left set it halves the pixel clock */
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_INPUT_DIV2, 0);
	ccu_upd(PLL_VIDEO0_REG, PLL_VIDEO0_N_MASK, (n - 1) << PLL_VIDEO0_N_SHIFT);
	if (was_on) {
		pll_video0_enable();
	}
	return 0;
}

int dpy_os_clk_set_rate(struct dpy_clk *clk, uint32_t hz)
{
	const struct dpy_clk_desc *d = clk_desc(clk);
	uint32_t mux, parent, div, max_div;

	if (!d) {
		return -ENODEV;
	}
	if (d->kind == CLK_KIND_PLL_VIDEO0_4X) {
		return pll_video0_set_rate(hz);
	}
	if (d->kind != CLK_KIND_MUXDIV && d->kind != CLK_KIND_MP) {
		return -ENOTSUP;
	}

	mux = (ccu_rd(d->reg) >> 24) & 0x7;
	if (d->kind == CLK_KIND_MP && d->parents[mux < MAX_PARENTS ? mux : 0] == CLK_PLL_VIDEO0_4X) {
		int ret = pll_video0_set_rate(hz);

		ccu_upd(d->reg, 0xf | (0x3UL << 8), 0);
		return ret;
	}

	parent = clk_parent_rate(d, mux);
	if (!parent || !hz) {
		return -EINVAL;
	}
	if (d->kind == CLK_KIND_MP) {
		div = CLAMP(DIV_ROUND_CLOSEST(parent, hz), 1U, 16U);
		ccu_upd(d->reg, 0xf | (0x3UL << 8), div - 1);
		return 0;
	}
	max_div = BIT(d->div_width);
	div = CLAMP(DIV_ROUND_CLOSEST(parent, hz), 1U, max_div);
	ccu_upd(d->reg, max_div - 1, div - 1);
	return 0;
}

uint32_t dpy_os_clk_get_rate(struct dpy_clk *clk)
{
	const struct dpy_clk_desc *d = clk_desc(clk);
	uint32_t reg, mux, rate;

	if (!d) {
		return 0;
	}
	switch (d->kind) {
	case CLK_KIND_PLL_PERI_2X:
		return pll_peri_rate();
	case CLK_KIND_PLL_PERI_1X:
		return pll_peri_rate() / 2;
	case CLK_KIND_PLL_VIDEO0_4X:
		return pll_video0_rate();
	case CLK_KIND_BUS:
		return DPY_HOSC_HZ;
	default:
		break;
	}
	reg = ccu_rd(d->reg);
	mux = (reg >> 24) & 0x7;
	rate = clk_parent_rate(d, mux);
	if (d->kind == CLK_KIND_MP) {
		return rate / ((reg & 0xf) + 1) / BIT((reg >> 8) & 0x3);
	}
	return rate / ((reg & (BIT(d->div_width) - 1)) + 1);
}

uint32_t dpy_os_clk_round_rate(struct dpy_clk *clk, uint32_t hz)
{
	const struct dpy_clk_desc *d = clk_desc(clk);

	if (!d) {
		return 0;
	}
	if (d->kind == CLK_KIND_PLL_VIDEO0_4X ||
	    (d->kind == CLK_KIND_MP &&
	     d->parents[((ccu_rd(d->reg) >> 24) & 0x7) < MAX_PARENTS ? ((ccu_rd(d->reg) >> 24) & 0x7) : 0] ==
		     CLK_PLL_VIDEO0_4X)) {
		return pll_video0_pick(hz) * DPY_HOSC_HZ;
	}
	return 0;
}

struct dpy_reset *dpy_os_reset_get(uint32_t controller, uint32_t id)
{
	size_t i;

	if (controller != ALLWINNER_CCU_MAIN) {
		return NULL;
	}
	for (i = 0; i < ARRAY_SIZE(dpy_resets); i++) {
		if (dpy_resets[i].id == id) {
			dpy_reset_pool[i].id = id;
			return &dpy_reset_pool[i];
		}
	}
	return NULL;
}

void dpy_os_reset_put(struct dpy_reset *rst)
{
	(void)rst;
}

static uint32_t reset_reg(const struct dpy_reset *rst)
{
	return dpy_resets[rst - dpy_reset_pool].reg;
}

int dpy_os_reset_assert(struct dpy_reset *rst)
{
	if (rst) {
		ccu_upd(reset_reg(rst), BIT(16), 0);
	}
	return 0;
}

int dpy_os_reset_deassert(struct dpy_reset *rst)
{
	if (rst) {
		ccu_upd(reset_reg(rst), BIT(16), BIT(16));
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Pins and GPIO                                                       */
/* ------------------------------------------------------------------ */
int dpy_os_pin_set_function(uint32_t pin, uint32_t function)
{
	f101_pin_mux(pin, function);
	return 0;
}

int dpy_os_pin_set_drive(uint32_t pin, uint32_t level)
{
	f101_pin_drive(pin, level);
	return 0;
}

int dpy_os_pin_set_pull(uint32_t pin, uint32_t pull)
{
	f101_pin_pull(pin, pull);
	return 0;
}

int dpy_os_gpio_set_value(uint32_t pin, int value)
{
	f101_gpio_set_level(pin, value);
	return 0;
}

int dpy_os_gpio_direction_output(uint32_t pin, int value)
{
	/* latch the level first so the pin does not glitch when it turns output */
	f101_gpio_set_level(pin, value);
	f101_pin_mux(pin, F101_GPIO_OUT);
	return 0;
}

int dpy_os_gpio_direction_input(uint32_t pin)
{
	f101_pin_mux(pin, F101_GPIO_IN);
	return 0;
}

int dpy_os_gpio_get_value(uint32_t pin)
{
	return f101_gpio_get_level(pin);
}

/* ------------------------------------------------------------------ */
/* Backlight PWM block (PWM_BL): digital dimming into an analog current sink */
/* ------------------------------------------------------------------ */
#define PWMBL_BASE		0x0200a000UL
#define PWMBL_MOD_REG		0x0790U
#define PWMBL_BUS_REG		0x0794U
#define PWMBL_PORT_STRIDE	0x200U
#define PWMBL_CTRL		0x0000U
#define PWMBL_CLK_CFG		0x0004U
#define PWMBL_PRD		0x0008U
#define PWMBL_ACT_CYCLE		0x000cU
#define PWMBL_ACT_STEP		0x0010U
#define PWMBL_ACT_UP_CYCLE	0x0014U
#define PWMBL_OCP_FLT_V		0x0018U
#define PWMBL_INT_EN		0x0040U
#define PWMBL_ANA0		0x0050U
#define PWMBL_ANA1		0x0054U
#define PWMBL_ANA2		0x0058U
#define PWMBL_MAX_LOAD_CUR	200U
#define PWMBL_DIM_STEPS		256U
#define PWMBL_MOD_TARGET	400000000U

static bool s_pbl_ready;

static uint32_t pbl_rd(unsigned int port, uint32_t off)
{
	return F101_REG32(PWMBL_BASE + port * PWMBL_PORT_STRIDE + off);
}

static void pbl_wr(unsigned int port, uint32_t off, uint32_t val)
{
	F101_REG32(PWMBL_BASE + port * PWMBL_PORT_STRIDE + off) = val;
}

static void pbl_upd(unsigned int port, uint32_t off, uint32_t mask, uint32_t val)
{
	pbl_wr(port, off, (pbl_rd(port, off) & ~mask) | (val & mask));
}

static void pbl_init(void)
{
	uint32_t src = pll_peri_rate();
	uint32_t div = CLAMP(DIV_ROUND_CLOSEST(src, PWMBL_MOD_TARGET), 1U, 32U);
	uint32_t reg;

	/* bus clock + reset + module clock (PLL_PERI_2X / div) */
	ccu_upd(PWMBL_BUS_REG, BIT(16) | BIT(0), BIT(16) | BIT(0));
	reg = ccu_rd(PWMBL_MOD_REG);
	reg &= ~(BIT(31) | GENMASK(26, 24) | GENMASK(4, 0));
	reg |= div - 1;
	ccu_wr(PWMBL_MOD_REG, reg);
	ccu_wr(PWMBL_MOD_REG, reg | BIT(31));

	/* pins PB0..PB3 are the PWM_BL outputs (function 2), PB0 and PB2 drive level 3 */
	for (int n = 0; n < 4; n++) {
		f101_pin_mux(32 + n, 2);
	}
	f101_pin_drive(32 + 0, 3);
	f101_pin_drive(32 + 2, 3);

	/* port 1: pure analog, 750 kHz */
	pbl_upd(1, PWMBL_CTRL, BIT(2), 0);
	pbl_upd(1, PWMBL_ANA2, 0x3, 1);
	pbl_upd(1, PWMBL_INT_EN, BIT(0) | BIT(2) | BIT(3), BIT(0) | BIT(2) | BIT(3));
	pbl_upd(1, PWMBL_ANA1, BIT(31), 0);
	pbl_upd(1, PWMBL_OCP_FLT_V, 0xff, 0xff);
	pbl_upd(1, PWMBL_OCP_FLT_V, BIT(31), BIT(31));
	pbl_upd(1, PWMBL_ANA1, 7, 2);
	pbl_upd(1, PWMBL_ANA1, GENMASK(23, 16), 0);
	pbl_upd(1, PWMBL_ANA0, BIT(0), BIT(0));
	pbl_upd(1, PWMBL_ANA1, GENMASK(23, 16), 0xffUL << 16);

	/* port 0: digital + analog compare */
	pbl_upd(0, PWMBL_CTRL, BIT(2), BIT(2));
	pbl_upd(0, PWMBL_CLK_CFG, GENMASK(31, 30), 2UL << 30);
	pbl_upd(0, PWMBL_CLK_CFG, GENMASK(14, 8), 0);
	pbl_upd(0, PWMBL_CLK_CFG, GENMASK(3, 0), 0);
	pbl_upd(0, PWMBL_PRD, GENMASK(15, 0), 0x215);
	pbl_upd(0, PWMBL_ACT_CYCLE, GENMASK(15, 0), 0x140);
	pbl_upd(0, PWMBL_ACT_CYCLE, GENMASK(31, 16), 0x1aUL << 16);
	pbl_upd(0, PWMBL_ACT_STEP, GENMASK(15, 0), 1);
	pbl_upd(0, PWMBL_ACT_UP_CYCLE, GENMASK(7, 0), 1);
	pbl_upd(0, PWMBL_ANA1, BIT(31), 0);
	pbl_upd(0, PWMBL_OCP_FLT_V, 0xff, 0xff);
	pbl_upd(0, PWMBL_OCP_FLT_V, BIT(31), BIT(31));
	pbl_upd(0, PWMBL_INT_EN, BIT(0) | BIT(2) | BIT(3), BIT(0) | BIT(2) | BIT(3));
	pbl_upd(0, PWMBL_ANA1, 7, 3);
	pbl_upd(0, PWMBL_ANA1, GENMASK(23, 16), 0);
	pbl_upd(0, PWMBL_CTRL, BIT(1), BIT(1));
	pbl_upd(0, PWMBL_ANA0, BIT(0), BIT(0));
	s_pbl_ready = true;
}

int dpy_os_pwm_apply(const void *ctrl, uint32_t channel, uint32_t period_ns, uint32_t duty_ns, bool inverted,
		     bool enable)
{
	uint32_t level;

	(void)ctrl;
	(void)inverted;		/* the analog stage has no polarity */
	if (channel != 0 || !period_ns || duty_ns > period_ns) {
		return -EINVAL;
	}
	if (!s_pbl_ready) {
		pbl_init();
	}
	level = enable ? (uint32_t)(((uint64_t)duty_ns * PWMBL_DIM_STEPS) / period_ns) : 0;
	if (level > PWMBL_MAX_LOAD_CUR) {
		level = PWMBL_MAX_LOAD_CUR;
	}
	for (unsigned int port = 0; port < 2; port++) {
		pbl_upd(port, PWMBL_CTRL, BIT(0), level ? BIT(0) : 0);
	}
	pbl_upd(0, PWMBL_ANA1, GENMASK(23, 16), (uint32_t)level << 16);
	return 0;
}

int dpy_os_regulator_set(uint32_t id, uint32_t microvolt, bool enable)
{
	(void)id;
	(void)microvolt;
	(void)enable;
	return -ENOTSUP;
}

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
void dpy_os_vprintf(const char *fmt, va_list ap)
{
	vprintf(fmt, ap);
}

void dpy_os_printf(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}
