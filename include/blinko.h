/*
 * blinko.h — Blinko optical logger, Zephyr port (nRF52840 / Arduino Nano 33 BLE).
 *
 * Same wire protocol and message model as the Arduino library (core/):
 * a hardware timer streams RLL(2,7) line-code chips to the LEDs; a phone camera
 * decodes them through its rolling shutter.
 *
 * "Red LED of death": on a fatal error (Zephyr k_sys_fatal_error_handler,
 * blinko_fatal()) the record is written to noinit RAM and to flash, then the
 * fault LED set blinks the record forever with a bit-bang loop that needs no
 * interrupts and no kernel.
 */
#ifndef BLINKO_H
#define BLINKO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <zephyr/sys/util.h>
#include <zephyr/autoconf.h>
#include "rs_proto.h"
#include "rs_tx.h"

#define BLINKO_MAX_LEDS 4
#define BLINKO_CHECKPOINT_LEN 16

struct blinko_config {
	uint32_t chip_us;          /* minimum run T of the line code in us (the timer runs T/3), default 60 */
	uint8_t  repeat;           /* copies of every packet, 1..100 (default 1): 2-3 for 30 fps phones, 40-80 for far lights */
	uint16_t burst_on_ms;      /* visible blink: transmit for burst_on_ms ... */
	uint16_t burst_off_ms;     /* ... then dark for burst_off_ms (0 = continuous) */
	uint8_t channels;          /* 3 = RGB streams on led0/1/2 (led3 mirrors led0), 1 = same stream everywhere */
	uint16_t pilot_ms;         /* RGB pilot interval */
	uint8_t fault_led;         /* led alias index for the death loop (red); always pulsed */
	bool persist_faults;       /* keep the record in flash across power cycles */
	bool announce_boot;        /* STATUS slot = boot count + reset cause */
};

#define BLINKO_CONFIG_DEFAULT { .chip_us = CONFIG_BLINKO_CHIP_US, .repeat = 1, .channels = CONFIG_BLINKO_CHANNELS, \
			       .pilot_ms = CONFIG_BLINKO_PILOT_MS, \
			       .burst_on_ms = CONFIG_BLINKO_BURST_ON_MS, .burst_off_ms = CONFIG_BLINKO_BURST_OFF_MS, \
			       .fault_led = CONFIG_BLINKO_FAULT_LED, \
			       .persist_faults = IS_ENABLED(CONFIG_BLINKO_PERSIST), \
			       .announce_boot = IS_ENABLED(CONFIG_BLINKO_ANNOUNCE_BOOT) }

int blinko_init(const struct blinko_config *cfg);

void blinko_log(uint8_t level, const char *fmt, ...);
#define blinko_debug(...) blinko_log(RS_LVL_DEBUG, __VA_ARGS__)
#define blinko_info(...)  blinko_log(RS_LVL_INFO, __VA_ARGS__)
#define blinko_warn(...)  blinko_log(RS_LVL_WARN, __VA_ARGS__)
#define blinko_error(...) blinko_log(RS_LVL_ERROR, __VA_ARGS__)
void blinko_status(const char *fmt, ...);
/* 16-bit id derived from the SoC factory id (announced as "id=xxxx" in the boot STATUS). */
uint16_t blinko_board_id(void);

/* Record the reason and blink it forever on the fault LEDs. Never returns. */
void blinko_fatal(uint8_t code, const char *fmt, ...) __attribute__((noreturn));

/* Name the current phase; reported after a watchdog reset ("WDT reset @name"). */
void blinko_checkpoint(const char *name);

bool blinko_has_fault(void);
const char *blinko_fault_text(void);
void blinko_clear_fault(void);

void blinko_set_chip_us(uint32_t us);
void blinko_set_burst(uint16_t on_ms, uint16_t off_ms);
void blinko_set_channels(uint8_t n);   /* 3 = RGB, 1 = single stream */
uint32_t blinko_chip_us(void);
void blinko_set_enabled(bool on);
void blinko_strobe(float hz);       /* calibration square wave; 0 = back to data */
void blinko_led_test(bool on);      /* steady LEDs (polarity check) */
uint32_t blinko_packets_sent(void);
uint32_t blinko_boot_count(void);
const char *blinko_reset_cause(void);
rs_tx_t *blinko_tx(void);

/* Used by the fatal handler; public so a custom handler can call it. */
void blinko_persist_and_loop(const char *text) __attribute__((noreturn));

#endif
