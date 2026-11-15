# Spike kit.
#   make test      build vcu_dummy for this machine and run the car script against it
#   make windows   build/vcu_dummy.exe (needs mingw-w64: brew install mingw-w64)
#   make kit       build/bfr_vcu_spike/, the folder to hand over (Windows exe included)

MINGW ?= x86_64-w64-mingw32-gcc
CFLAGS = -O2 -Wall -Wextra -std=c11

all: test

build:
	mkdir -p build

build/vcu_dummy: vcu_dummy.c bridge_layout.h | build
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -o $@ vcu_dummy.c

build/vcu_dummy.exe: vcu_dummy.c bridge_layout.h | build
	$(MINGW) $(CFLAGS) -static -o $@ vcu_dummy.c -lwinmm

test: build/vcu_dummy
	luajit tests/test_spike.lua build/vcu_dummy

windows: build/vcu_dummy.exe

kit: build/vcu_dummy.exe
	rm -rf build/bfr_vcu_spike
	mkdir -p build/bfr_vcu_spike/data
	cp ../bfr_mk12/data/brakes.lua build/bfr_vcu_spike/data/
	cp car/script.lua car/spike.lua build/bfr_vcu_spike/data/
	cp build/vcu_dummy.exe CHECKLIST.md build/bfr_vcu_spike/
	@echo "kit: spike/build/bfr_vcu_spike/"

clean:
	rm -rf build

.PHONY: all test windows kit clean
