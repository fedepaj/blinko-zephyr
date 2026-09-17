BOARD ?= arduino_nano_33_ble
.PHONY: build flash
build:
	west build -b $(BOARD) -d build samples/blinko_demo
flash: build
	samples/blinko_demo/flash.sh
