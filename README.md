# blinko-zephyr

Zephyr module `blinko` (`CONFIG_BLINKO=y`): optical logging over the board LEDs
and the "red LED of death" fatal-error path, on portable Zephyr APIs
(`counter`, `gpio`, `hwinfo`, `flash_map`). Shared core in the git submodule
`core/`. `samples/blinko_demo` is a demo with a USB shell; `west.yml` makes
this repo a west manifest repository (see `docs/ZEPHYR.md`).

## Requirements

- Zephyr v4.4.2 (pulled by `west.yml`), a Zephyr SDK with `arm-zephyr-eabi`,
  `west`, `ninja` and `dtc`.
- A board whose devicetree provides at least one of the `led0`..`led3` aliases,
  a `blinko-timer` alias pointing at a `counter` device that can run at 1 MHz
  or more, and a `storage_partition` (the module uses its last 4 KB page for
  the fault record). `led0`..`led2` carry the three streams and `led3` mirrors
  `led0`; a board with a single LED sets `CONFIG_BLINKO_CHANNELS=1`. The
  death loop blinks `BLINKO_FAULT_LED`, or the first LED there is.
- The sample targets the Arduino Nano 33 BLE (nRF52840); its overlay, in
  `samples/blinko_demo/boards/`, is the template for another board:

  ```dts
  / { aliases { blinko-timer = &timer1; }; };
  &timer1 { status = "okay"; prescaler = <4>; };   /* 1 MHz */
  ```
- `make flash` is written for macOS with the Arduino `bossac` tool and
  `pyserial` installed.

## Build and run

```sh
git submodule update --init
# from the workspace top dir (parent of this repo):
west init -l zephyr-module && west update --narrow -o=--depth=1
cd zephyr-module && make build && make flash     # BOARD=... for another board
```

`make flash` runs `samples/blinko_demo/flash.sh` (1200-baud touch, then
`bossac`); double-tap RESET if the bootloader is not caught. The demo exposes
a shell over USB CDC at 115200 (all under `blinko`):

| Command | |
|---|---|
| `info`, `warn`, `err`, `debug` `<text>` | a log message at that level |
| `status <text>` | the STATUS message |
| `zlog err\|wrn\|inf\|dbg <text>` | a Zephyr `LOG_x` line, through the log backend |
| `fatal <text>` | record the text and blink it on the red LED until reset |
| `hf`, `oops`, `hang` | a real bus fault, `k_oops()`, a watchdog reset |
| `clear` | forget the persisted fault |
| `chip <us>` | T, the shortest run of the line code (the timer runs at T/3) |
| `rep <n>` | copies of every packet (1..100) |
| `rgb 3\|1` | three RGB streams, or one stream on every LED |
| `burst <on_ms> <off_ms>` | visible blink; off 0 = continuous |
| `strobe <hz>` | calibration square wave; 0 = back to data |
| `led on\|off\|data` | steady LEDs for a polarity check, or back to data |
| `stat`, `reset`, `dfu` | transmitter state, reboot, reboot into the bootloader |

Shorting D2 to D3 raises a real bus fault and the red LED blinks the reason
until the board is reset; the reason is sent again after every boot until
`blinko clear`.

## Use in your own application

```cmake
list(APPEND ZEPHYR_EXTRA_MODULES /path/to/zephyr-module)   # before find_package(Zephyr)
```

```conf
CONFIG_GPIO=y
CONFIG_HWINFO=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_COUNTER=y
CONFIG_BLINKO=y
```

```c
#include <blinko.h>
blinko_info("boot ok");              /* blinko_init() already ran at SYS_INIT */
blinko_checkpoint("init-sensors");   /* reported after a watchdog reset */
LOG_WRN("battery low");              /* the log backend sends this too */
if (err) blinko_fatal(3, "sensor init");   /* never returns */
```

With `CONFIG_BLINKO_AUTO_INIT=y` (default) the logger starts by itself and
`k_sys_fatal_error_handler` is already hooked, so any exception, `k_oops()`,
`k_panic()` or stack overflow ends up on the red LED without a line of code.
`CONFIG_BLINKO_LOG_BACKEND=y` forwards `LOG_ERR()`/`LOG_WRN()`/`LOG_INF()`
(and `LOG_DBG()` up to `BLINKO_LOG_BACKEND_LEVEL`) to the LEDs, one log line
per message, without module prefix and timestamp; use it with
`CONFIG_LOG_MODE_DEFERRED` (the default), where the lines are formatted in the
logging thread.

## Kconfig

| Option | Default | Meaning |
|---|---|---|
| `BLINKO` | n | enable the module (needs GPIO, HWINFO, FLASH, FLASH_MAP, COUNTER) |
| `BLINKO_CHIP_US` | 60 | T, the shortest run of the line code (µs, at least 24); keep it above the phone's exposure, the timer runs at T/3 |
| `BLINKO_CHANNELS` | 3 | 3 = RGB streams on led0/1/2 (led3 mirrors led0), 1 = one stream |
| `BLINKO_PILOT_MS` | 30 | RGB colour-calibration pilot interval |
| `BLINKO_FAULT_WEIGHT` | 3 | airtime of the FAULT message in the death loop (1–4): w − 1 extra FAULT visits after every other message, about three quarters of the packets at 3 |
| `BLINKO_FAULT_CHIP_US` | 120 | the death loop's T, independent of the running one: the conservative value every phone tried could read |
| `BLINKO_FAULT_REPEAT` | 3 | packet copies in the death loop |
| `BLINKO_BURST_ON_MS` | 150 | visible blink: transmit time |
| `BLINKO_BURST_OFF_MS` | 50 | visible blink: dark time (0 = continuous) |
| `BLINKO_FAULT_LED` | 0 | led alias index used by the death loop |
| `BLINKO_PERSIST` | y | keep fault records in the last page of `storage_partition` |
| `BLINKO_ANNOUNCE_BOOT` | y | boot count and reset cause in the STATUS slot |
| `BLINKO_AUTO_INIT` | y | initialize at boot (`SYS_INIT`, application level) |
| `BLINKO_RAM_RECORD_ADDR` | 0 | fixed address for the reset-surviving record (0 = `.noinit`); Nano 33 BLE needs `0x2003F000` |
| `BLINKO_NRF_FLASH_IN_FATAL` | y | write the record from the fatal handler via `nrfx_nvmc` (Nordic only) |
| `BLINKO_LOG_BACKEND` | y | forward Zephyr log lines to the LEDs |
| `BLINKO_LOG_BACKEND_LEVEL` | 3 | highest level forwarded (1 ERR … 4 DBG) |
| `BLINKO_FATAL_HOOK` | y | override `k_sys_fatal_error_handler` with the death loop |

## API (`include/blinko.h`)

| Call | |
|---|---|
| `blinko_init(cfg)` | start with a configuration; only with `CONFIG_BLINKO_AUTO_INIT=n` (otherwise the module has started at boot on its Kconfig settings and the call returns `-EALREADY`). `BLINKO_CONFIG_DEFAULT` fills a `struct blinko_config` from Kconfig |
| `blinko_log(level, fmt, …)`, `blinko_debug/info/warn/error(fmt, …)` | a log message: 31 bytes, up to 41 characters of ordinary text; a longer text is split (127 characters per call). Six are on air at a time. From threads and interrupt handlers |
| `blinko_status(fmt, …)` | the STATUS message (one message; the same text again changes nothing) |
| `blinko_fatal(code, fmt, …)` | record `F<code>:<text>` and blink it until reset; does not return |
| `blinko_checkpoint(name)` | name the current phase: a watchdog reset reports `WDT reset @name` |
| `blinko_has_fault()`, `blinko_fault_text()`, `blinko_clear_fault()` | the persisted fault, sent after every boot until cleared |
| `blinko_board_id()` | 16-bit id of the board, announced as `id=xxxx` in the boot STATUS |
| `blinko_set_chip_us(us)`, `blinko_chip_us()` | T at run time |
| `blinko_set_repeat(n)`, `blinko_set_channels(n)`, `blinko_set_burst(on_ms, off_ms)` | copies of every packet (1..100), 3 or 1 streams, visible blink |
| `blinko_set_enabled(on)`, `blinko_strobe(hz)`, `blinko_led_test(on)` | pause the LEDs, calibration square wave, polarity check |
| `blinko_packets_sent()`, `blinko_boot_count()`, `blinko_reset_cause()` | counters and the reset cause as text |
| `blinko_persist_and_loop(text)` | what the fatal hook calls, for a custom fatal handler |

`blinko_tx()` gives the transmitter's state for diagnostics (the sample's
`stat`). The header is usable from C++.

A fault text is `ZF<reason> p=<pc> l=<lr>` for a Zephyr fatal error
(`reason` is the `K_ERR_*` number), `F<code>:<text>` for `blinko_fatal`,
`WDT reset @<checkpoint>` after a watchdog reset.
