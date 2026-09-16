BOARD ?= arduino_nano_33_ble
.PHONY: build flash
build:
	west build -b $(BOARD) -d build samples/rslog_demo
flash: build
	samples/rslog_demo/flash.sh
