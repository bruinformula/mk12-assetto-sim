# MK12 SIL VCU build & test targets.
#   make test      build native bfr_vcu and run C reference parity + LuaJIT SIL tests
#   make windows   build/bfr_vcu.exe (needs mingw-w64: brew install mingw-w64)
#   make kit       build/bfr_vcu_kit/, ready to hand off to Windows AC + CSP

MINGW   ?= x86_64-w64-mingw32-gcc
VCU_DIR ?= ../vendor/mk12-vcu

CFLAGS     = -O2 -Wall -Wextra -std=c11 -DTC_ENABLED=1
VCU_INCS   = -I$(VCU_DIR)/Core/Inc -I$(VCU_DIR)/Core/Src -I$(VCU_DIR)/Core/Src/traction_model
VCU_TC_SRC = $(VCU_DIR)/Core/Src/traction_control.c \
             $(VCU_DIR)/Core/Src/traction_model/Block_Diagram_1.c \
             $(VCU_DIR)/Core/Src/traction_model/Block_Diagram_1_data.c \
             $(VCU_DIR)/Core/Src/traction_model/rt_nonfinite.c
COMMON_SRC = bfr_vcu.c profile_loader.c $(VCU_TC_SRC)
COMMON_HDR = bridge_layout.h profile_loader.h \
             $(VCU_DIR)/Core/Inc/torque_map.h \
             $(VCU_DIR)/Core/Inc/power_limit.h \
             $(VCU_DIR)/Core/Inc/traction_control.h \
             $(VCU_DIR)/Core/Src/torque_map.c \
             $(VCU_DIR)/Core/Src/power_limit.c

# Suppress harmless unused-parameter warnings inside Embedded Coder generated files only.
EC_CFLAGS = -O2 -std=c11

all: test

build:
	mkdir -p build

# Native host build (macOS / Linux)
build/profile_loader.o: profile_loader.c profile_loader.h | build
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -c -o $@ profile_loader.c

build/bfr_vcu.o: bfr_vcu.c $(COMMON_HDR) | build
	$(CC) $(CFLAGS) -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE $(VCU_INCS) -c -o $@ bfr_vcu.c

build/traction_control.o: $(VCU_DIR)/Core/Src/traction_control.c $(VCU_DIR)/Core/Inc/traction_control.h | build
	$(CC) $(CFLAGS) $(VCU_INCS) -c -o $@ $(VCU_DIR)/Core/Src/traction_control.c

TC_MODEL_HDR = $(VCU_DIR)/Core/Src/traction_model/Block_Diagram_1.h \
               $(VCU_DIR)/Core/Src/traction_model/Block_Diagram_1_types.h \
               $(VCU_DIR)/Core/Src/traction_model/rt_nonfinite.h

build/Block_Diagram_1.o: $(VCU_DIR)/Core/Src/traction_model/Block_Diagram_1.c $(TC_MODEL_HDR) | build
	$(CC) $(EC_CFLAGS) $(VCU_INCS) -c -o $@ $<

build/Block_Diagram_1_data.o: $(VCU_DIR)/Core/Src/traction_model/Block_Diagram_1_data.c $(TC_MODEL_HDR) | build
	$(CC) $(EC_CFLAGS) $(VCU_INCS) -c -o $@ $<

build/rt_nonfinite.o: $(VCU_DIR)/Core/Src/traction_model/rt_nonfinite.c $(TC_MODEL_HDR) | build
	$(CC) $(EC_CFLAGS) $(VCU_INCS) -c -o $@ $<

build/bfr_vcu: build/bfr_vcu.o build/profile_loader.o build/traction_control.o \
               build/Block_Diagram_1.o build/Block_Diagram_1_data.o build/rt_nonfinite.o
	$(CC) -o $@ $^ -lm

# Windows x86_64 static cross-compile
build/win_profile_loader.o: profile_loader.c profile_loader.h | build
	$(MINGW) $(CFLAGS) -c -o $@ profile_loader.c

build/win_bfr_vcu.o: bfr_vcu.c $(COMMON_HDR) | build
	$(MINGW) $(CFLAGS) $(VCU_INCS) -c -o $@ bfr_vcu.c

build/win_traction_control.o: $(VCU_DIR)/Core/Src/traction_control.c $(VCU_DIR)/Core/Inc/traction_control.h | build
	$(MINGW) $(CFLAGS) $(VCU_INCS) -c -o $@ $(VCU_DIR)/Core/Src/traction_control.c

build/win_Block_Diagram_1.o: $(VCU_DIR)/Core/Src/traction_model/Block_Diagram_1.c $(TC_MODEL_HDR) | build
	$(MINGW) $(EC_CFLAGS) $(VCU_INCS) -c -o $@ $<

build/win_Block_Diagram_1_data.o: $(VCU_DIR)/Core/Src/traction_model/Block_Diagram_1_data.c $(TC_MODEL_HDR) | build
	$(MINGW) $(EC_CFLAGS) $(VCU_INCS) -c -o $@ $<

build/win_rt_nonfinite.o: $(VCU_DIR)/Core/Src/traction_model/rt_nonfinite.c $(TC_MODEL_HDR) | build
	$(MINGW) $(EC_CFLAGS) $(VCU_INCS) -c -o $@ $<

build/bfr_vcu.exe: build/win_bfr_vcu.o build/win_profile_loader.o build/win_traction_control.o \
                   build/win_Block_Diagram_1.o build/win_Block_Diagram_1_data.o build/win_rt_nonfinite.o
	$(MINGW) -static -o $@ $^ -lm -lwinmm

test: build/bfr_vcu
	./build/bfr_vcu --profile $(VCU_DIR)/profiles/base.yaml \
	                --verify-reference $(VCU_DIR)/tests/reference/outputs/base.csv
	./build/bfr_vcu --profile $(VCU_DIR)/tests/reference/fixtures/test_spread.yaml \
	                --verify-reference $(VCU_DIR)/tests/reference/outputs/test_spread.csv
	luajit tests/test_sil.lua build/bfr_vcu $(VCU_DIR)

windows: build/bfr_vcu.exe

kit: build/bfr_vcu.exe
	rm -rf build/bfr_vcu_kit
	mkdir -p build/bfr_vcu_kit/data
	cp ../bfr_mk12/data/brakes.lua ../bfr_mk12/data/drivetrain.ini build/bfr_vcu_kit/data/
	cp car/script.lua car/vcu_bridge.lua build/bfr_vcu_kit/data/
	cp build/bfr_vcu.exe README.txt build/bfr_vcu_kit/
	cp $(VCU_DIR)/profiles/base.yaml build/bfr_vcu_kit/
	@echo "kit: sil/build/bfr_vcu_kit/"

pages:
	python3 build_pages.py build/pages

clean:
	rm -rf build

.PHONY: all test windows kit pages clean
