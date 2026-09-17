# blinko-zephyr

Zephyr module `blinko` (`CONFIG_BLINKO=y`): optical logging over the board LEDs
and the "red LED of death" fatal-error path, on portable Zephyr APIs
(`counter`, `gpio`, `hwinfo`, `flash_map`). Shared core in the git submodule
`core/`. `samples/blinko_demo` is a demo with a USB shell; `west.yml` makes
this repo a west manifest repository (see `docs/ZEPHYR.md`).

```sh
git submodule update --init
# from the workspace top dir (parent of this repo):
west init -l zephyr-module && west update --narrow -o=--depth=1
cd zephyr-module && make build && make flash
```
