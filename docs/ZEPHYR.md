# Port Zephyr (Arduino Nano 33 BLE Rev2, nRF52840)

Workspace west minimale dentro il repo (`zephyr-app/west.yml` è il manifest:
zephyr v4.4.2 + hal_nordic + cmsis). Toolchain: Zephyr SDK 1.0.1 minimal in
`toolchain/` (solo `arm-zephyr-eabi`).

## Setup (già fatto una volta)

```sh
brew install ninja dtc
.venv/bin/pip install west
.venv/bin/west init -l zephyr-app && .venv/bin/west update --narrow -o=--depth=1
# SDK: toolchain/zephyr-sdk-1.0.1/setup.sh -t arm-zephyr-eabi -h -c
```

## Build e flash

```sh
make zephyr           # build/zephyr/zephyr/zephyr.bin
make zephyr-flash     # bossac (bootloader Arduino, offset 0x10000), porta auto
```

Shell su USB CDC (115200): `blinko info ciao`, `blinko fatal x`, `blinko hf`,
`blinko oops`, `blinko hang`, `blinko clear`, `blinko chip 30`, `blinko rgb 3|1`,
`blinko burst 150 50`, `blinko strobe 2000`, `blinko led on|off|data`,
`blinko stat`, `blinko reset`. Il demo tiene il LED in continua
(`CONFIG_BLINKO_BURST_OFF_MS=0`); **corto D2–D3** → bus fault reale → red LED
of death (uscita: doppio reset).

## Uso in una qualsiasi app Zephyr

Il port è un **modulo Zephyr** (`zephyr-modules/blinko`): nel `CMakeLists.txt`
dell'app aggiungi `list(APPEND ZEPHYR_EXTRA_MODULES <repo>/zephyr-modules/blinko)`
prima di `find_package(Zephyr)` e in `prj.conf`:

```
CONFIG_GPIO=y
CONFIG_HWINFO=y
CONFIG_FLASH=y
CONFIG_FLASH_MAP=y
CONFIG_COUNTER=y
CONFIG_BLINKO=y
# Nano 33 BLE: CONFIG_BLINKO_RAM_RECORD_ADDR=0x2003F000, CONFIG_NRFX_NVMC=y
```

Board Arduino con supporto Zephyr in-tree (v4.4): Due, GIGA R1, MKR Zero,
Nano 33 BLE, Nano 33 IoT, Nano Matter, Nicla Sense ME/Vision, Opta, Portenta
C33/H7, UNO Q, UNO R4, Zero. Per una nuova board: overlay con gli alias, poi
`blinko stat` → `boot#` deve incrementare dopo `blinko reset` (altrimenti
impostare `BLINKO_RAM_RECORD_ADDR`).

Nel device tree servono gli alias `led0..led3` (solo `led0` obbligatorio: è il
LED del death loop), `blinko-timer` verso un timer con driver `counter`, e una
`storage_partition`. Con `CONFIG_BLINKO_AUTO_INIT=y` (default) il logger parte da solo (SYS_INIT) e
`k_sys_fatal_error_handler` è già agganciato: qualsiasi eccezione, `k_oops`,
`k_panic` o stack overflow finisce nel **red LED of death** senza scrivere una
riga di codice. Opzioni: `BLINKO_CHIP_US`, `BLINKO_LED_MASK`,
`BLINKO_FAULT_LED_MASK`, `BLINKO_PERSIST`, `BLINKO_RAM_RECORD_ADDR`,
`BLINKO_FATAL_HOOK`. API: `blinko_info/warn/error/status`, `blinko_fatal`,
`blinko_checkpoint`, `blinko_clear_fault`.

## Architettura del port (`zephyr-modules/blinko`)

| Blocco | Nano R4 (Arduino) | Nano 33 BLE (Zephyr) |
|---|---|---|
| chip clock | FspTimer GPT ISR | `counter` API (alias DT `blinko-timer`, callback sul top value) |
| LED | `digitalWrite` | `gpio` API sugli alias `led0..led3` (polarità dal device tree) |
| fault hook | `HardFault_Handler` (naked asm) | `k_sys_fatal_error_handler(reason, esf)` |
| record RAM | indirizzo fisso 0x20007A00 | `.noinit` oppure indirizzo fisso (`BLINKO_RAM_RECORD_ADDR`, 0x2003F000 sulla Nano 33 BLE) |
| record flash | data flash grezza, ultimo blocco | `flash_map` API al boot (portabile); su nRF anche subito nel fatal handler con `nrfx_nvmc` |
| reset cause | `RSTSR0/1/2` | `hwinfo_get_reset_cause` |
| watchdog | `WDT.begin` | `wdt_install_timeout` + `wdt_feed` |
| death loop | `R_BSP_SoftwareDelay` | `k_busy_wait` (nrf, senza kernel) |

**Canali e LED**: `led0/led1/led2` = canali R/G/B (tre flussi indipendenti,
`CONFIG_BLINKO_CHANNELS=3`), `led3` (arancione) ripete il canale 0. Le raffiche
di lampeggio visibile sono a scelta dell'utente (`BLINKO_BURST_OFF_MS=0` =
continuo). **Red LED of death**: in caso di fatal error il logger passa a un
solo flusso sul `led0` (rosso), sempre pulsato 150/50 ms: un LED rosso che
lampeggia significa "scheda morta, inquadrami e ti dico perché".

Il core C (`core/rs_tx.c`) è identico a quello del Nano R4: il telefono non
distingue le due schede.

## Risultati sull'hardware (2026-09-15)

- `blinko hf` → `ZF25 p=00011abc l=0001c167` (25 = bus fault preciso) riportato
  dopo il riavvio da watchdog (RAM) e dopo reset software (flash, scritta dal
  fatal handler con `nrfx_nvmc`); `blinko hang` → `WDT reset @hang-test`.
- `blinko fatal vreg 2.9V brownout` → scheda "morta" (USB spento), solo LED
  rosso lampeggiante: l'iPhone ha decodificato `F42:vreg 2.9V brownout`
  (6.4 righe/chip nel loop `k_busy_wait`).
- Bootloader Nano 33 BLE = "Arduino Bootloader (SAM-BA extended) 2.0": ignora
  GPREGRET; entra in modalità seriale se trova `0x07738135` a **0x20007FFC**
  (eredità SAMD a 32 KB di RAM). Il demo lo scrive al touch a 1200 baud
  (callback `cdc_acm_dte_rate_callback_set`), quindi `zephyr-app/flash.sh`
  funziona senza toccare la scheda; dal death loop serve il doppio reset.
- Il bootloader azzera la RAM bassa (`.noinit` di Zephyr): il record vive a
  0x2003F000 (le sonde a 0x20020000, 0x2003F000 e 0x2003FE00 sopravvivono).
- Timer: il driver `nrfx_timer` non generava interrupt nella configurazione
  provata; i registri di TIMER1 sono più semplici e funzionano.
