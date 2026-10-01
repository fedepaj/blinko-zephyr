#include <blinko.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/printk.h>
#include <zephyr/sys/util.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

#include <zephyr/drivers/counter.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/storage/flash_map.h>
#ifdef CONFIG_BLINKO_NRF_FLASH_IN_FATAL
#include <nrfx_nvmc.h>
#endif

#define BLINKO_FAULT_MAGIC 0x52534641u /* "RSFA" */
#define BLINKO_BOOT_MAGIC  0x52534254u /* "RSBT" */
#define BLINKO_FLASH_MAGIC 0x52534545u /* "RSEE" */
#define BLINKO_LOAD_MAGIC  0x4C4F4144u /* "LOAD" */

/* ------------------------------------------------------------ records */

struct rs_ram_record {
	uint32_t magic;       /* BLINKO_FAULT_MAGIC when a fault record is pending */
	uint32_t boot_magic;  /* BLINKO_BOOT_MAGIC once init ran (warm reset detection) */
	uint32_t boot_count;
	uint32_t loading;     /* boot-loop guard while reading flash */
	uint32_t fw_id;
	char checkpoint[BLINKO_CHECKPOINT_LEN];
	char text[RS_MSG_MAX_LEN + 1];
};

struct rs_flash_record {
	uint32_t magic;
	uint32_t boot_count;
	char text[RS_MSG_MAX_LEN + 1];
	uint32_t pad;
};

/* Reset-surviving record. Zephyr's .noinit sits in low RAM, which the Nano 33
 * BLE bootloader clears on every reset (verified on hardware); RAM from
 * 0x20020000 up survives. Fixed address, far above anything Zephyr links
 * (app uses ~21 KB of the 256 KB) and below the bootloader's top-of-RAM flags. */
#if CONFIG_BLINKO_RAM_RECORD_ADDR != 0
#define ram_rec (*(struct rs_ram_record *)CONFIG_BLINKO_RAM_RECORD_ADDR)
#else
static struct rs_ram_record ram_rec_noinit __noinit;
#define ram_rec ram_rec_noinit
#endif

/* Record page: last 4 KB of the storage partition. */
#define BLINKO_FLASH_PAGE 4096
#define BLINKO_FLASH_OFF  (PARTITION_SIZE(storage_partition) - BLINKO_FLASH_PAGE)
#define BLINKO_FLASH_ADDR (PARTITION_OFFSET(storage_partition) + BLINKO_FLASH_OFF)

/* ---------------------------------------------------------------- state */

/* LEDs from the board's led0..led3 aliases (led0 = fault LED, red where available). */
#define LED_SPEC(alias) COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(alias)), (GPIO_DT_SPEC_GET(DT_ALIAS(alias), gpios)), ({ 0 }))
static const struct gpio_dt_spec leds[BLINKO_MAX_LEDS] = { LED_SPEC(led0), LED_SPEC(led1), LED_SPEC(led2), LED_SPEC(led3) };
static uint8_t led_present;                  /* bit i: leds[i] usable */

/* Chip clock: a Zephyr counter device (alias blinko-timer), top-value callback. */
#define BLINKO_TIMER_NODE DT_ALIAS(blinko_timer)
static const struct device *const timer_dev = DEVICE_DT_GET(BLINKO_TIMER_NODE);

static struct blinko_config cfg = BLINKO_CONFIG_DEFAULT;
static bool initialized;
static rs_tx_t tx;
static char fault_text[RS_MSG_MAX_LEN + 1];
static char reset_cause_str[12] = "?";
static bool running, enabled = true, strobe_mode;
static uint8_t strobe_level;

/* TIMER1 driven through its registers (1 MHz, 32-bit, compare0 -> clear). */
#define TIMER_NODE DT_NODELABEL(timer1)

/* ----------------------------------------------------------------- LEDs */

/* led0..led2 = channels 0..2, led3 mirrors channel 0 (orange LED_BUILTIN). */
static inline void write_chips(const uint8_t chips[RS_MAX_CHANNELS])
{
	static const uint8_t led_channel[BLINKO_MAX_LEDS] = { 0, 1, 2, 0 };
	for (int i = 0; i < BLINKO_MAX_LEDS; i++) {
		if (led_present & BIT(i)) {
			gpio_pin_set_dt(&leds[i], chips[led_channel[i]]);
		}
	}
}

static inline void write_all(uint8_t level)
{
	uint8_t v[RS_MAX_CHANNELS] = { level, level, level };
	write_chips(v);
}

static void timer_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(dev); ARG_UNUSED(user_data);
	if (!enabled) {
		return;
	}
	if (strobe_mode) {
		strobe_level ^= 1;
		write_all(strobe_level);
		return;
	}
	uint8_t chips[RS_MAX_CHANNELS];
	rs_tx_next_chips(&tx, chips);
	write_chips(chips);
}

static int timer_set_period(uint32_t period_us)
{
	uint32_t freq = counter_get_frequency(timer_dev);
	struct counter_top_cfg top = {
		.ticks = (uint32_t)(((uint64_t)freq * period_us) / 1000000u),
		.callback = timer_cb,
		.user_data = NULL,
		.flags = 0,
	};
	if (top.ticks < 2) {
		top.ticks = 2;
	}
	return counter_set_top_value(timer_dev, &top);
}

static int timer_start(uint32_t period_us)
{
	if (!device_is_ready(timer_dev)) {
		return -ENODEV;
	}
	int rc = timer_set_period(period_us);
	if (rc) {
		return rc;
	}
	return counter_start(timer_dev);
}

static void timer_stop(void)
{
	counter_stop(timer_dev);
}

/* ---------------------------------------------------------------- flash */

/* Thread context: portable flash_map API. */
static void flash_write_record(const struct rs_flash_record *r)
{
	const struct flash_area *fa;
	if (flash_area_open(PARTITION_ID(storage_partition), &fa) != 0) {
		return;
	}
	flash_area_erase(fa, BLINKO_FLASH_OFF, BLINKO_FLASH_PAGE);
	flash_area_write(fa, BLINKO_FLASH_OFF, r, sizeof(*r));
	flash_area_close(fa);
}

static void flash_read_record(struct rs_flash_record *r)
{
	const struct flash_area *fa;
	memset(r, 0, sizeof(*r));
	if (flash_area_open(PARTITION_ID(storage_partition), &fa) != 0) {
		return;
	}
	flash_area_read(fa, BLINKO_FLASH_OFF, r, sizeof(*r));
	flash_area_close(fa);
}

/* Fatal context (interrupts locked, no kernel): only an SoC-specific blocking
 * writer can persist immediately. Otherwise the RAM record is written to
 * flash at the next boot. */
static void flash_write_record_fatal(const struct rs_flash_record *r)
{
#ifdef CONFIG_BLINKO_NRF_FLASH_IN_FATAL
	nrfx_nvmc_page_erase(BLINKO_FLASH_ADDR);
	nrfx_nvmc_words_write(BLINKO_FLASH_ADDR, r, sizeof(*r) / 4);
	while (!nrfx_nvmc_write_done_check()) {
	}
#else
	ARG_UNUSED(r);
#endif
}

static uint32_t fw_build_id(void)
{
	const char *s = __DATE__ " " __TIME__;
	uint32_t h = 2166136261u;
	while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
	return h;
}

/* ---------------------------------------------------------- reset cause */

static void read_reset_cause(void)
{
	uint32_t c = 0;
	const char *s = "?";
	if (hwinfo_get_reset_cause(&c) == 0) {
		if (c & RESET_POR) s = "POR";
		else if (c & RESET_BROWNOUT) s = "BOR";
		else if (c & RESET_WATCHDOG) s = "WDT";
		else if (c & RESET_SOFTWARE) s = "SW";
		else if (c & RESET_PIN) s = "PIN";
		else if (c & RESET_DEBUG) s = "DBG";
		else if (c & RESET_LOW_POWER_WAKE) s = "LPW";
		else if (c & RESET_CPU_LOCKUP) s = "LOCKUP";
		else if (c) s = "OTHER";
		hwinfo_clear_reset_cause();
	}
	strncpy(reset_cause_str, s, sizeof(reset_cause_str) - 1);
}

/* -------------------------------------------------------------- logging */

static void set_slot(uint8_t id, uint8_t level, const char *text, size_t len)
{
	unsigned int key = irq_lock();
	rs_tx_set_slot(&tx, id, level, text, len);
	irq_unlock(key);
}

static void vlog(uint8_t level, const char *fmt, va_list ap)
{
	char buf[128];
	int n = vsnprintk(buf, sizeof(buf), fmt, ap);
	if (n < 0) {
		return;
	}
	if (n > (int)sizeof(buf) - 1) {
		n = sizeof(buf) - 1;
	}
	for (int off = 0; off < n; off += RS_MSG_MAX_LEN) {
		int len = MIN(n - off, RS_MSG_MAX_LEN);
		unsigned int key = irq_lock();
		rs_tx_log(&tx, level, buf + off, len);
		irq_unlock(key);
	}
}

void blinko_log(uint8_t level, const char *fmt, ...)
{
	va_list ap; va_start(ap, fmt); vlog(level, fmt, ap); va_end(ap);
}

uint16_t blinko_board_id(void)
{
	uint8_t id[16]; ssize_t n = hwinfo_get_device_id(id, sizeof(id));
	uint32_t h = 2166136261u;
	for (ssize_t i = 0; i < n; i++) { h ^= id[i]; h *= 16777619u; }   /* FNV-1a of the factory id */
	return (uint16_t)(h ^ (h >> 16));
}

void blinko_status(const char *fmt, ...)
{
	char buf[RS_MSG_MAX_LEN + 1];
	va_list ap; va_start(ap, fmt);
	int n = vsnprintk(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (n < 0) {
		return;
	}
	set_slot(RS_SLOT_STATUS, RS_LVL_STATUS, buf, MIN(n, RS_MSG_MAX_LEN));
}

void blinko_checkpoint(const char *name)
{
	strncpy(ram_rec.checkpoint, name, BLINKO_CHECKPOINT_LEN - 1);
	ram_rec.checkpoint[BLINKO_CHECKPOINT_LEN - 1] = 0;
}

bool blinko_has_fault(void) { return tx.slots[RS_SLOT_FAULT].valid; }
const char *blinko_fault_text(void) { return fault_text; }
uint32_t blinko_packets_sent(void) { return tx.packets_sent; }
uint32_t blinko_boot_count(void) { return ram_rec.boot_count; }
const char *blinko_reset_cause(void) { return reset_cause_str; }
rs_tx_t *blinko_tx(void) { return &tx; }
uint32_t blinko_chip_us(void) { return cfg.chip_us; }

void blinko_clear_fault(void)
{
	fault_text[0] = 0;
	unsigned int key = irq_lock();
	rs_tx_clear_slot(&tx, RS_SLOT_FAULT);
	irq_unlock(key);
	if (cfg.persist_faults) {
		struct rs_flash_record z = { 0 };
		flash_write_record(&z);
	}
}

static void apply_burst(rs_tx_t *t)
{
	uint32_t cell_us = cfg.chip_us / RS_CELLS_PER_T;      /* chip_us is T, the timer runs one code cell */
	rs_tx_set_burst(t, (uint32_t)cfg.burst_on_ms * 1000u / cell_us, (uint32_t)cfg.burst_off_ms * 1000u / cell_us);
	rs_tx_set_channels(t, cfg.channels, (uint32_t)cfg.pilot_ms * 1000u / cell_us);
	rs_tx_set_repeat(t, cfg.repeat ? cfg.repeat : 1);
}

void blinko_set_channels(uint8_t n)
{
	cfg.channels = (n == 3) ? 3 : 1;
	unsigned int key = irq_lock();
	apply_burst(&tx);
	irq_unlock(key);
}

void blinko_set_chip_us(uint32_t us)
{
	cfg.chip_us = MAX(us, 24u);                         /* T >= 24 us: cell >= 8 us */
	unsigned int key = irq_lock();
	apply_burst(&tx);
	irq_unlock(key);
	if (running && !strobe_mode) {
		timer_set_period(cfg.chip_us / RS_CELLS_PER_T);
	}
}

void blinko_set_burst(uint16_t on_ms, uint16_t off_ms)
{
	cfg.burst_on_ms = on_ms; cfg.burst_off_ms = off_ms;
	unsigned int key = irq_lock();
	apply_burst(&tx);
	irq_unlock(key);
}

void blinko_set_enabled(bool on)
{
	enabled = on;
	if (!on) {
		write_all(0);
	}
}

void blinko_strobe(float hz)
{
	if (hz <= 0) {
		strobe_mode = false;
		if (running) timer_set_period(cfg.chip_us / RS_CELLS_PER_T);
		return;
	}
	strobe_mode = true;
	if (running) timer_set_period((uint32_t)(500000.0f / hz));  /* toggle twice per period */
}

void blinko_led_test(bool on)
{
	enabled = false;
	write_all(on ? 1 : 0);
}

/* ------------------------------------------------------- death loop */

void blinko_persist_and_loop(const char *text)
{
	strncpy(ram_rec.text, text, RS_MSG_MAX_LEN);
	ram_rec.text[RS_MSG_MAX_LEN] = 0;
	ram_rec.magic = BLINKO_FAULT_MAGIC;

	(void)irq_lock();
	if (running) {
		timer_stop();
	}
	if (cfg.persist_faults) {
		struct rs_flash_record fr = { .magic = BLINKO_FLASH_MAGIC, .boot_count = ram_rec.boot_count };
		strncpy(fr.text, ram_rec.text, RS_MSG_MAX_LEN);
		flash_write_record_fatal(&fr);
	}

	/* fresh transmitter: FAULT + STATUS + recent logs */
	static rs_tx_t ftx;
	rs_tx_init(&ftx);
	for (int i = 0; i < RS_NUM_LOG_SLOTS; i++) {
		ftx.slots[i] = tx.slots[i];
	}
	ftx.slots[RS_SLOT_STATUS] = tx.slots[RS_SLOT_STATUS];
	ftx.seq_counter = tx.seq_counter;
	rs_tx_set_slot(&ftx, RS_SLOT_FAULT, RS_LVL_FAULT, ram_rec.text, strlen(ram_rec.text));
	/* Red LED of death: one stream on the fault LED only, always pulsed 150/50 ms. */
	uint32_t chip_us = cfg.chip_us ? cfg.chip_us : 30;
	rs_tx_set_channels(&ftx, 1, 0);
	rs_tx_set_fault_weight(&ftx, CONFIG_BLINKO_FAULT_WEIGHT);
	rs_tx_set_burst(&ftx, 150000u / chip_us, 50000u / chip_us);

	write_all(0);
	int fl = cfg.fault_led < BLINKO_MAX_LEDS ? cfg.fault_led : 0;
	if (!(led_present & BIT(fl))) fl = 0;

	/* Exact chip timing without interrupts: the counter device is restarted as a
	 * free-running clock and polled. The work per chip (encoder + GPIO) must not
	 * add to the period, or packets grow taller than the LED blob in the frame.
	 * Any counter width is handled by accumulating elapsed ticks. */
	uint32_t top = 0, ticks = 0, prev = 0, acc = 0;
	bool have_clock = false;
	if (device_is_ready(timer_dev)) {
		struct counter_top_cfg run = { .ticks = counter_get_max_top_value(timer_dev), .callback = NULL, .user_data = NULL, .flags = 0 };
		if (counter_set_top_value(timer_dev, &run) == 0 && counter_start(timer_dev) == 0) {
			top = run.ticks;
			ticks = (uint32_t)(((uint64_t)counter_get_frequency(timer_dev) * chip_us) / 1000000u);
			have_clock = ticks >= 4 && counter_get_value(timer_dev, &prev) == 0;
		}
	}
	uint8_t chip = rs_tx_next_chip(&ftx);
	for (;;) {
		if (have_clock) {
			for (;;) {
				uint32_t now;
				counter_get_value(timer_dev, &now);
				acc += (now >= prev) ? (now - prev) : (top - prev + 1u + now);
				prev = now;
				if (acc >= ticks) { acc -= ticks; break; }
			}
		} else {
			k_busy_wait(chip_us);
		}
		gpio_pin_set_dt(&leds[fl], chip);
		chip = rs_tx_next_chip(&ftx);           /* prepared while the chip is being shown */
	}
}

void blinko_fatal(uint8_t code, const char *fmt, ...)
{
	char buf[RS_MSG_MAX_LEN + 1];
	int p = snprintk(buf, sizeof(buf), "F%u:", code);
	va_list ap; va_start(ap, fmt);
	vsnprintk(buf + p, sizeof(buf) - p, fmt, ap);
	va_end(ap);
	blinko_persist_and_loop(buf);
}

#ifdef CONFIG_BLINKO_FATAL_HOOK
/* Zephyr fatal error hook: exceptions (bus/usage/mem faults), k_oops, k_panic,
 * stack overflows, spurious interrupts. reason: K_ERR_* */
void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	uint32_t pc = esf ? esf->basic.pc : 0;
	uint32_t lr = esf ? esf->basic.lr : 0;
	char t[RS_MSG_MAX_LEN + 1];
	snprintk(t, sizeof(t), "ZF%u p=%08x l=%08x", reason, (unsigned)pc, (unsigned)lr);
	/* mini backtrace: return addresses found on the stack above the exception frame
	 * (odd Thumb addresses inside flash), as an ERROR log copied into the death loop */
	if (esf) {
		const uint32_t *sp = (const uint32_t *)esf + sizeof(struct arch_esf) / 4;
		uint32_t bt[3]; int nb = 0;
		for (int i = 0; i < 96 && nb < 3; i++) {
			uint32_t w = sp[i];
			if ((w & 1) && w >= CONFIG_FLASH_BASE_ADDRESS + 0x100 &&
			    w < CONFIG_FLASH_BASE_ADDRESS + CONFIG_FLASH_SIZE * 1024u && w != lr && w != (pc | 1)) {
				bt[nb++] = w;
			}
		}
		if (nb > 0) {
			char b[RS_MSG_MAX_LEN + 1]; int n = snprintk(b, sizeof(b), "bt");
			for (int i = 0; i < nb; i++) { n += snprintk(b + n, sizeof(b) - n, " %05x", (unsigned)bt[i]); }
			blinko_log(RS_LVL_ERROR, "%s", b);
		}
	}
	blinko_persist_and_loop(t);
}
#endif

/* ------------------------------------------------------------------ init */

int blinko_init(const struct blinko_config *c)
{
	if (initialized) {
		return 0;
	}
	initialized = true;
	if (c) {
		cfg = *c;
	}
	rs_tx_init(&tx);
	led_present = 0;
	for (int i = 0; i < BLINKO_MAX_LEDS; i++) {
		if (leds[i].port == NULL || !gpio_is_ready_dt(&leds[i])) {
			continue;
		}
		if (gpio_pin_configure_dt(&leds[i], GPIO_OUTPUT_INACTIVE) == 0) {
			led_present |= BIT(i);
		}
	}
	if (!led_present) {
		return -ENODEV;
	}

	read_reset_cause();

	bool warm = ram_rec.boot_magic == BLINKO_BOOT_MAGIC;
	if (!warm) {
		memset(&ram_rec, 0, sizeof(ram_rec));
		ram_rec.boot_magic = BLINKO_BOOT_MAGIC;
	}
	if (ram_rec.fw_id != fw_build_id()) {
		ram_rec.fw_id = fw_build_id();
		ram_rec.checkpoint[0] = 0;
		warm = false;   /* fresh upload */
	}
	ram_rec.boot_count++;

	if (ram_rec.magic == BLINKO_FAULT_MAGIC) {
		ram_rec.text[RS_MSG_MAX_LEN] = 0;
		strncpy(fault_text, ram_rec.text, RS_MSG_MAX_LEN);
		ram_rec.magic = 0;
		if (cfg.persist_faults) {
			struct rs_flash_record fr = { .magic = BLINKO_FLASH_MAGIC, .boot_count = ram_rec.boot_count };
			strncpy(fr.text, fault_text, RS_MSG_MAX_LEN);
			flash_write_record(&fr);
		}
	} else if (warm && strcmp(reset_cause_str, "WDT") == 0) {
		ram_rec.checkpoint[BLINKO_CHECKPOINT_LEN - 1] = 0;
		snprintk(fault_text, sizeof(fault_text), "WDT reset @%s",
			 ram_rec.checkpoint[0] ? ram_rec.checkpoint : "?");
		if (cfg.persist_faults) {
			struct rs_flash_record fr = { .magic = BLINKO_FLASH_MAGIC, .boot_count = ram_rec.boot_count };
			strncpy(fr.text, fault_text, RS_MSG_MAX_LEN);
			flash_write_record(&fr);
		}
	} else if (cfg.persist_faults) {
		if (warm && ram_rec.loading == BLINKO_LOAD_MAGIC) {
			struct rs_flash_record z = { 0 };
			flash_write_record(&z);
			strncpy(fault_text, "boot-loop guard: record wiped", RS_MSG_MAX_LEN);
		} else {
			struct rs_flash_record fr;
			ram_rec.loading = BLINKO_LOAD_MAGIC;
			flash_read_record(&fr);
			if (fr.magic == BLINKO_FLASH_MAGIC) {
				fr.text[RS_MSG_MAX_LEN] = 0;
				strncpy(fault_text, fr.text, RS_MSG_MAX_LEN);
			}
		}
	}
	ram_rec.loading = 0;
	ram_rec.checkpoint[0] = 0;
	fault_text[RS_MSG_MAX_LEN] = 0;

	if (fault_text[0]) {
		set_slot(RS_SLOT_FAULT, RS_LVL_FAULT, fault_text, strlen(fault_text));
	}
	if (cfg.announce_boot) {
		blinko_status("boot#%u rst=%s id=%04x", ram_rec.boot_count, reset_cause_str, blinko_board_id());
	}
	apply_burst(&tx);
	int rc = timer_start(cfg.chip_us / RS_CELLS_PER_T);
	running = rc == 0;
	return rc;
}

#ifdef CONFIG_BLINKO_AUTO_INIT
static int blinko_sys_init(void)
{
	return blinko_init(NULL);
}
SYS_INIT(blinko_sys_init, APPLICATION, 0);
#endif
