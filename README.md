# blinko-zephyr

Zephyr module `blinko` (`CONFIG_BLINKO=y`): optical logging over the board LEDs
and the "red LED of death" fatal-error path, on portable Zephyr APIs
(`counter`, `gpio`, `hwinfo`, `flash_map`). Shared core in the git submodule
`core/`. `samples/blinko_demo` is a demo with a USB shell; `west.yml` makes
this repo a west manifest repository (see `docs/ZEPHYR.md`).

## Requirements

- Zephyr v4.4.2 (pulled by `west.yml`), a Zephyr SDK with `arm-zephyr-eabi`,
  `west`, `ninja` and `dtc`.
- A board whose devicetree provides the `led0`..`led3` aliases (only `led0` is
  required: it is the death-loop LED), a `blinko-timer` alias pointing at a
  `counter` device, and a `storage_partition`. The sample targets the Arduino
  Nano 33 BLE (nRF52840); its overlay is in `samples/blinko_demo/boards/`.

## Build and run

```sh
git submodule update --init
# from the workspace top dir (parent of this repo):
west init -l zephyr-module && west update --narrow -o=--depth=1
cd zephyr-module && make build && make flash     # BOARD=... for another board
```

`make flash` runs `samples/blinko_demo/flash.sh` (1200-baud touch, then
`bossac`); double-tap RESET if the bootloader is not caught. The demo exposes
a shell over USB CDC at 115200: `blinko info <text>`, `warn`, `err`, `status`,
`fatal <text>`, `hf`, `oops`, `hang`, `clear`, `chip <us>`, `strobe <hz>`,
`led on|off|data`, `stat`, `reset`. Shorting D2 to D3 raises a real bus fault
and the red LED blinks the reason forever.

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
per message, module prefix and timestamp stripped; it needs
`CONFIG_LOG_MODE_DEFERRED` (the default).

## Kconfig

| Option | Default | Meaning |
|---|---|---|
| `BLINKO` | n | enable the module (needs GPIO, HWINFO, FLASH, FLASH_MAP, COUNTER) |
| `BLINKO_CHIP_US` | 60 | T, the shortest run of the line code (µs); keep it above the phone's exposure, the timer runs at T/3 |
| `BLINKO_CHANNELS` | 3 | 3 = RGB streams on led0/1/2 (led3 mirrors led0), 1 = one stream |
| `BLINKO_PILOT_MS` | 30 | RGB colour-calibration pilot interval |
| `BLINKO_FAULT_WEIGHT` | 3 | FAULT visits per other visit in the death loop (1–4) |
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

`blinko_init(cfg)` · `blinko_log(level, …)` and `blinko_debug/info/warn/error`
· `blinko_status()` (STATUS slot) · `blinko_fatal(code, …)` (noreturn) ·
`blinko_checkpoint(name)` · `blinko_has_fault/fault_text/clear_fault()` ·
`blinko_board_id()` · `blinko_set_chip_us/chip_us/set_burst/set_channels/
set_enabled/strobe/led_test()` · `blinko_packets_sent/boot_count/reset_cause()`
· `blinko_tx()` for the raw transmitter state · `blinko_persist_and_loop(text)`
for a custom fatal handler. `struct blinko_config` mirrors the Kconfig options;
`BLINKO_CONFIG_DEFAULT` fills it from them; `.repeat` (1–4) sends every packet
several times for cameras whose window is shorter than a packet.
