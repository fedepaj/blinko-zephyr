/*
 * rslog_demo (Zephyr) — Arduino Nano 33 BLE. Shell over USB CDC:
 *   rslog info <text> | warn | err | debug | status
 *   rslog fatal <text>   record and blink forever on the red LED
 *   rslog hf             real bus fault -> k_sys_fatal_error_handler -> red LED of death
 *   rslog oops           k_oops()
 *   rslog hang           stop feeding the watchdog -> WDT reset, "WDT reset @checkpoint"
 *   rslog clear | chip <us> | strobe <hz> | led on|off|data | stat | reset
 *
 * Hardware demo: short D2 to D3 -> a real bus fault is provoked -> the Zephyr
 * fatal handler (rslog core) blinks the reason on the red LED forever.
 * Recover with a double-tap on RESET (the record is persisted and re-sent).
 */
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/uart/cdc_acm.h>
#include <hal/nrf_power.h>
#include <stdlib.h>
#include <string.h>
#include <rslog.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(rslog_demo, LOG_LEVEL_DBG);

static const struct device *wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static const struct gpio_dt_spec short_sense = GPIO_DT_SPEC_GET(DT_NODELABEL(short_sense), gpios);
static const struct gpio_dt_spec short_drive = GPIO_DT_SPEC_GET(DT_NODELABEL(short_drive), gpios);
static int wdt_ch = -1;
static volatile bool hang;
static uint32_t counter;

static void enter_bootloader(void);

static void join_args(size_t argc, char **argv, char *out, size_t n)
{
	out[0] = 0;
	for (size_t i = 1; i < argc; i++) {
		if (i > 1) strncat(out, " ", n - strlen(out) - 1);
		strncat(out, argv[i], n - strlen(out) - 1);
	}
}

static int wdt_arm(uint32_t ms)
{
	if (wdt_ch >= 0) return 0;
	struct wdt_timeout_cfg tcfg = { .window = { .min = 0, .max = ms }, .flags = WDT_FLAG_RESET_SOC };
	wdt_ch = wdt_install_timeout(wdt, &tcfg);
	if (wdt_ch < 0) return wdt_ch;
	return wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);
}

#define LOG_CMD(name, level) \
	static int cmd_##name(const struct shell *sh, size_t argc, char **argv) \
	{ char b[128]; join_args(argc, argv, b, sizeof(b)); rslog_log(level, "%s", b); \
	  shell_print(sh, "ok"); return 0; }
LOG_CMD(info, RS_LVL_INFO)
LOG_CMD(warn, RS_LVL_WARN)
LOG_CMD(err, RS_LVL_ERROR)
LOG_CMD(debug, RS_LVL_DEBUG)

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{ char b[64]; join_args(argc, argv, b, sizeof(b)); rslog_status("%s", b); shell_print(sh, "ok"); return 0; }

static int cmd_fatal(const struct shell *sh, size_t argc, char **argv)
{
	char b[64]; join_args(argc, argv, b, sizeof(b));
	shell_print(sh, "fatal: red LED of death"); k_sleep(K_MSEC(50));
	rslog_fatal(42, "%s", b);
}

static int cmd_hf(const struct shell *sh, size_t argc, char **argv)
{
	wdt_arm(4000);
	rslog_checkpoint("hf-test");
	shell_print(sh, "bus fault now (WDT reboot in ~4 s)"); k_sleep(K_MSEC(50));
	volatile uint32_t *bad = (volatile uint32_t *)0xCFFFFFF0u;
	(void)*bad;
	return 0;
}

static int cmd_oops(const struct shell *sh, size_t argc, char **argv)
{
	wdt_arm(4000); rslog_checkpoint("oops-test");
	shell_print(sh, "k_oops"); k_sleep(K_MSEC(50));
	k_oops();
	return 0;
}

static int cmd_hang(const struct shell *sh, size_t argc, char **argv)
{
	wdt_arm(2000); rslog_checkpoint("hang-test");
	shell_print(sh, "hanging: WDT reset in ~2 s");
	hang = true;
	return 0;
}

static int cmd_clear(const struct shell *sh, size_t argc, char **argv) { rslog_clear_fault(); shell_print(sh, "fault cleared"); return 0; }
static int cmd_rgb(const struct shell *sh, size_t argc, char **argv)
{ int n = argc > 1 ? atoi(argv[1]) : 3; rslog_set_channels(n); shell_print(sh, n == 1 ? "1 channel" : "3 channels (RGB)"); return 0; }
static int cmd_burst(const struct shell *sh, size_t argc, char **argv)
{ if (argc > 2) rslog_set_burst(strtoul(argv[1], NULL, 10), strtoul(argv[2], NULL, 10)); shell_print(sh, "burst set"); return 0; }
static int cmd_chip(const struct shell *sh, size_t argc, char **argv)
{ if (argc > 1) rslog_set_chip_us(strtoul(argv[1], NULL, 10)); shell_print(sh, "chip_us=%u", rslog_chip_us()); return 0; }
static int cmd_strobe(const struct shell *sh, size_t argc, char **argv)
{ float hz = argc > 1 ? strtof(argv[1], NULL) : 0; rslog_strobe(hz); shell_print(sh, hz > 0 ? "strobe on" : "data mode"); return 0; }
static int cmd_led(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "on")) rslog_led_test(true);
	else if (argc > 1 && !strcmp(argv[1], "off")) rslog_led_test(false);
	else rslog_set_enabled(true);
	return 0;
}
static uint32_t gpregret_at_boot, gpregret2_at_boot;
static int cmd_dfu(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1 && !strcmp(argv[1], "ram")) { shell_print(sh, "RAM magic then reset"); k_sleep(K_MSEC(80)); enter_bootloader(); }
	uint32_t v = argc > 1 ? strtoul(argv[1], NULL, 16) : 0x4E;
	uint32_t reg = argc > 2 ? strtoul(argv[2], NULL, 10) : 0;
	shell_print(sh, "GPREGRET%s=0x%02x then reset", reg ? "2" : "", v); k_sleep(K_MSEC(80));
	nrf_power_gpregret_set(NRF_POWER, reg, v);
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}
static int cmd_reset(const struct shell *sh, size_t argc, char **argv) { shell_print(sh, "reset"); k_sleep(K_MSEC(50)); sys_reboot(SYS_REBOOT_COLD); return 0; }

static int cmd_stat(const struct shell *sh, size_t argc, char **argv)
{
	rs_tx_t *tx = rslog_tx();
	shell_print(sh, "packets_sent=%u chip_us=%u reset=%s boot#%u", rslog_packets_sent(), rslog_chip_us(),
		    rslog_reset_cause(), rslog_boot_count());
	shell_print(sh, "fault=%s", rslog_has_fault() ? rslog_fault_text() : "(none)");
	shell_print(sh, "gpregret at boot: 0x%02x / 0x%02x", gpregret_at_boot, gpregret2_at_boot);
	for (int i = 0; i < RS_NUM_SLOTS; i++) {
		if (!tx->slots[i].valid) continue;
		shell_print(sh, "  slot %d lvl %d: %s %d bytes", i, tx->slots[i].level, tx->slots[i].packed ? "packed" : "raw", tx->slots[i].len);
	}
	return 0;
}

/* zlog <err|wrn|inf|dbg> text...: a plain Zephyr LOG_x() call, forwarded by the log backend */
static int cmd_zlog(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) { shell_print(sh, "usage: zlog err|wrn|inf|dbg text"); return -EINVAL; }
	char text[96] = ""; size_t n = 0;
	for (size_t i = 2; i < argc && n < sizeof(text) - 2; i++) {
		if (i > 2) { text[n++] = ' '; }
		size_t l = strlen(argv[i]); if (l > sizeof(text) - 1 - n) { l = sizeof(text) - 1 - n; }
		memcpy(text + n, argv[i], l); n += l; text[n] = 0;
	}
	if (!strcmp(argv[1], "err")) { LOG_ERR("%s", text); }
	else if (!strcmp(argv[1], "wrn")) { LOG_WRN("%s", text); }
	else if (!strcmp(argv[1], "dbg")) { LOG_DBG("%s", text); }
	else { LOG_INF("%s", text); }
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(rslog_cmds,
	SHELL_CMD(zlog, NULL, "zlog err|wrn|inf|dbg text (Zephyr LOG_x -> LEDs)", cmd_zlog),
	SHELL_CMD(info, NULL, "log INFO", cmd_info),
	SHELL_CMD(warn, NULL, "log WARN", cmd_warn),
	SHELL_CMD(err, NULL, "log ERROR", cmd_err),
	SHELL_CMD(debug, NULL, "log DEBUG", cmd_debug),
	SHELL_CMD(status, NULL, "set STATUS slot", cmd_status),
	SHELL_CMD(fatal, NULL, "fatal: persist + red LED of death", cmd_fatal),
	SHELL_CMD(hf, NULL, "provoke a bus fault", cmd_hf),
	SHELL_CMD(oops, NULL, "k_oops()", cmd_oops),
	SHELL_CMD(hang, NULL, "hang until watchdog reset", cmd_hang),
	SHELL_CMD(clear, NULL, "clear persisted fault", cmd_clear),
	SHELL_CMD(chip, NULL, "chip <us>", cmd_chip),
	SHELL_CMD(burst, NULL, "burst <on_ms> <off_ms>", cmd_burst),
	SHELL_CMD(rgb, NULL, "rgb 3|1", cmd_rgb),
	SHELL_CMD(strobe, NULL, "strobe <hz> (0 = data)", cmd_strobe),
	SHELL_CMD(led, NULL, "led on|off|data", cmd_led),
	SHELL_CMD(stat, NULL, "transmitter state", cmd_stat),
	SHELL_CMD(reset, NULL, "reboot", cmd_reset),
	SHELL_CMD(dfu, NULL, "dfu [hexmagic] [reg]: set GPREGRET and reboot", cmd_dfu),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(rslog, &rslog_cmds, "RSLog optical logger", NULL);

/* Arduino-style "1200 baud touch": when the host sets the CDC line coding to
 * 1200 bps, ask the (Adafruit-derived) bootloader to stay in serial DFU mode
 * and reboot. The line-coding callback catches it even if the port is opened
 * and closed within milliseconds (bossac does exactly that). */
static volatile bool touch_1200;
static void dte_rate_cb(const struct device *dev, uint32_t rate)
{
	ARG_UNUSED(dev);
	if (rate == 1200) {
		touch_1200 = true;
	}
}
static void touch_1200_init(void)
{
	const struct device *cdc = DEVICE_DT_GET(DT_NODELABEL(cdc_acm_uart0));
	if (device_is_ready(cdc)) {
		cdc_acm_dte_rate_callback_set(cdc, dte_rate_cb);
	}
}
/* The Nano 33 BLE ships "Arduino Bootloader (SAM-BA extended) 2.0", a port of
 * the SAMD bootloader: it looks for the double-tap magic at 0x20007FFC (last word of a 32 KB
 * RAM, SAMD heritage). We also set the Adafruit-style GPREGRET magic. */
#define BOOT_DOUBLE_TAP_ADDR  0x20007FFCu   /* found in bootloader.bin: SAMD-style "end of 32 KB RAM" */
#define BOOT_DOUBLE_TAP_MAGIC 0x07738135u
static void enter_bootloader(void)
{
	*(volatile uint32_t *)BOOT_DOUBLE_TAP_ADDR = BOOT_DOUBLE_TAP_MAGIC;
	nrf_power_gpregret_set(NRF_POWER, 0, 0x4E);   /* DFU_MAGIC_SERIAL_ONLY_RESET */
	k_sleep(K_MSEC(50));
	sys_reboot(SYS_REBOOT_COLD);
}
static void check_touch_1200(void)
{
	if (touch_1200) {
		enter_bootloader();
	}
}

int main(void)
{
	gpregret_at_boot = nrf_power_gpregret_get(NRF_POWER, 0);
	gpregret2_at_boot = nrf_power_gpregret_get(NRF_POWER, 1);
	int rc = rslog_init(NULL);   /* already done by SYS_INIT when CONFIG_RSLOG_AUTO_INIT=y */
	rslog_info("boot ok zephyr");
	rslog_checkpoint("main");
	touch_1200_init();
	usb_enable(NULL);
	gpio_pin_configure_dt(&short_sense, GPIO_INPUT);
	gpio_pin_configure_dt(&short_drive, GPIO_OUTPUT_INACTIVE);   /* D3 low: shorting D2-D3 pulls D2 low */
	if (rc) {
		rslog_error("timer init %d", rc);
	}
	while (1) {
		if (hang) {
			(void)irq_lock();
			for (;;) { }
		}
		if (wdt_ch >= 0) wdt_feed(wdt, wdt_ch);
		if (gpio_pin_get_dt(&short_sense) == 1) {
			/* D2 shorted to D3: die for real (bus fault), no watchdog -> red LED of death */
			rslog_checkpoint("d2-d3 short");
			volatile uint32_t *bad = (volatile uint32_t *)0xCFFFFFF0u;
			(void)*bad;
		}
		if ((counter % 50) == 0) {
			rslog_status("up=%us rst=%s n=%u id=%04x", (unsigned)(k_uptime_get() / 1000), rslog_reset_cause(), counter / 50, rslog_board_id());
			rslog_checkpoint("main-loop");
		}
		counter++;
		check_touch_1200();
		k_sleep(K_MSEC(100));
	}
	return 0;
}
