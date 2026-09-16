/*
 * rslog.h — RSLog optical logger, Zephyr port (nRF52840 / Arduino Nano 33 BLE).
 *
 * Same wire protocol and message model as the Arduino library (core/):
 * a hardware timer streams Manchester chips to the LEDs; a phone camera
 * decodes them through its rolling shutter.
 *
 * "Red LED of death": on a fatal error (Zephyr k_sys_fatal_error_handler,
 * rslog_fatal()) the record is written to noinit RAM and to flash, then the
 * fault LED set blinks the record forever with a bit-bang loop that needs no
 * interrupts and no kernel.
 */
#ifndef RSLOG_H
#define RSLOG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zephyr/sys/util.h>
#include <zephyr/autoconf.h>
#include "rs_proto.h"
#include "rs_tx.h"

#define RSLOG_MAX_LEDS 4
#define RSLOG_CHECKPOINT_LEN 16

struct rslog_config {
	uint32_t chip_us;          /* half-bit duration, default 30 */
	uint16_t burst_on_ms;      /* visible blink: transmit for burst_on_ms ... */
	uint16_t burst_off_ms;     /* ... then dark for burst_off_ms (0 = continuous) */
	uint8_t channels;          /* 3 = RGB streams on led0/1/2 (led3 mirrors led0), 1 = same stream everywhere */
	uint16_t pilot_ms;         /* RGB pilot interval */
	uint8_t fault_led;         /* led alias index for the death loop (red); always pulsed */
	bool persist_faults;       /* keep the record in flash across power cycles */
	bool announce_boot;        /* STATUS slot = boot count + reset cause */
};

#define RSLOG_CONFIG_DEFAULT { .chip_us = CONFIG_RSLOG_CHIP_US, .channels = CONFIG_RSLOG_CHANNELS, \
			       .pilot_ms = CONFIG_RSLOG_PILOT_MS, \
			       .burst_on_ms = CONFIG_RSLOG_BURST_ON_MS, .burst_off_ms = CONFIG_RSLOG_BURST_OFF_MS, \
			       .fault_led = CONFIG_RSLOG_FAULT_LED, \
			       .persist_faults = IS_ENABLED(CONFIG_RSLOG_PERSIST), \
			       .announce_boot = IS_ENABLED(CONFIG_RSLOG_ANNOUNCE_BOOT) }

int rslog_init(const struct rslog_config *cfg);

void rslog_log(uint8_t level, const char *fmt, ...);
#define rslog_debug(...) rslog_log(RS_LVL_DEBUG, __VA_ARGS__)
#define rslog_info(...)  rslog_log(RS_LVL_INFO, __VA_ARGS__)
#define rslog_warn(...)  rslog_log(RS_LVL_WARN, __VA_ARGS__)
#define rslog_error(...) rslog_log(RS_LVL_ERROR, __VA_ARGS__)
void rslog_status(const char *fmt, ...);

/* Record the reason and blink it forever on the fault LEDs. Never returns. */
void rslog_fatal(uint8_t code, const char *fmt, ...) __attribute__((noreturn));

/* Name the current phase; reported after a watchdog reset ("WDT reset @name"). */
void rslog_checkpoint(const char *name);

bool rslog_has_fault(void);
const char *rslog_fault_text(void);
void rslog_clear_fault(void);

void rslog_set_chip_us(uint32_t us);
void rslog_set_burst(uint16_t on_ms, uint16_t off_ms);
void rslog_set_channels(uint8_t n);   /* 3 = RGB, 1 = single stream */
uint32_t rslog_chip_us(void);
void rslog_set_enabled(bool on);
void rslog_strobe(float hz);       /* calibration square wave; 0 = back to data */
void rslog_led_test(bool on);      /* steady LEDs (polarity check) */
uint32_t rslog_packets_sent(void);
uint32_t rslog_boot_count(void);
const char *rslog_reset_cause(void);
rs_tx_t *rslog_tx(void);

/* Used by the fatal handler; public so a custom handler can call it. */
void rslog_persist_and_loop(const char *text) __attribute__((noreturn));

#endif
