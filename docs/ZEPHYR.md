# The Zephyr module

How `blinko` is built on Zephyr. Using it is covered by the README; the wire
format by `docs/PROTOCOL.md` of the blinko-core repository (the `core/`
submodule).

## Parts

| File | |
|---|---|
| `src/blinko.c` | the module: LEDs, chip timer, messages, boot bookkeeping, fault path |
| `src/blinko_log_backend.c` | Zephyr log backend: `LOG_x()` lines become messages |
| `include/blinko.h` | the API |
| `Kconfig`, `CMakeLists.txt`, `zephyr/module.yml` | the module definition; `core/rs_tx.c` and `core/rs_pack.c` are the only core sources it compiles |
| `samples/blinko_demo` | a shell over USB CDC for the Arduino Nano 33 BLE |
| `west.yml` | makes this repository a west manifest (Zephyr v4.4.2) |

## The chip timer

The `counter` device aliased `blinko-timer` runs with a top value of one chip
(T/3) and calls the module at every wrap, in interrupt context. Each call
writes the LEDs with the chips worked out by the call before, so the delay
from the timer to the pins is constant, and then works out the next ones
(`rs_tx_next_chips`). `led0`..`led2` take channels 0..2 and `led3` mirrors
channel 0; LEDs the devicetree does not have are skipped.

Choosing and encoding the next packets (`rs_tx_prepare`) takes several chip
periods. The callback does it right after a packet has started, during the
three dark chips of its gap: the timer ticks that fall in that time are
skipped, so the gap is a few chips longer than three and the data field after
every sync stays intact.

## Messages

`blinko_log()` formats the text (127 characters at most), packs it into one or
more message slots with the timer interrupt running, and copies each slot
into the transmitter with interrupts locked. `blinko_status()` does the same
for the STATUS slot, unless the text is the one already on air. The calls may
come from any thread and from interrupt handlers.

The log backend receives each log line from the logging thread, formatted
without timestamp, level and source, and hands it to `blinko_log()` at the
matching level.

## Boot and faults

A record in RAM that survives resets (`.noinit`, or the fixed address
`CONFIG_BLINKO_RAM_RECORD_ADDR` on boards whose bootloader clears `.noinit`;
the Nano 33 BLE needs `0x2003F000`) holds the boot count, the build id, the
last checkpoint and a pending fault text. At boot the module:

- counts the boot and reads the reset cause (`hwinfo`);
- if the record holds a fault, makes it the FAULT message and writes it to
  flash (unless the same text is there already);
- otherwise, after a watchdog reset, reports `WDT reset @<checkpoint>`;
- otherwise loads the fault kept in flash, if any. A reset during that read is
  taken as a damaged record and the next boot wipes it instead of reading it.

A persisted fault is sent after every boot until `blinko_clear_fault()`.

The fault record lives in the last 4 KB page of `storage_partition`, validated
by a magic word. An application that uses the same partition for NVS or a file
system must leave that page out.

### The death loop

`k_sys_fatal_error_handler` (with `CONFIG_BLINKO_FATAL_HOOK`) and
`blinko_fatal()` end in `blinko_persist_and_loop()`:

1. the text goes into the RAM record;
2. interrupts are locked and the chip timer stopped;
3. on Nordic SoCs (`CONFIG_BLINKO_NRF_FLASH_IN_FATAL`) the record is written
   to flash at once with `nrfx_nvmc`; elsewhere the next boot writes it;
4. a fresh transmitter is filled with the fault, the STATUS and the last log
   messages, at the fault timing (`BLINKO_FAULT_CHIP_US`, `BLINKO_FAULT_REPEAT`,
   `BLINKO_FAULT_WEIGHT`), one stream, pulsed 150/50 ms;
5. the loop writes one LED chip by chip, timed by polling the counter device
   as a free-running clock (`k_busy_wait` if it cannot be started). It uses no
   interrupt and no kernel service.

The loop ends only with a reset. If the fault happens before the module has
started, no LED is configured and only the RAM record is kept for the next boot.

## Limits

- The timer callback and four GPIO writes run once per chip: how short T can
  be depends on the SoC. 60 µs works on the nRF52840; the lower bound accepted
  is 24 µs and is not guaranteed to be sustainable.
- The packet period is longer than 82 chips by the ticks the encoding takes
  (see "The chip timer"); a receiver's cyclic decode of repeated packets
  assumes 82.
- The fatal hook reads the program counter and the link register from the
  Cortex-M exception frame; other architectures need their own hook.
- The death loop relies on the counter and GPIO drivers working with
  interrupts locked, which holds for the nRF drivers.
