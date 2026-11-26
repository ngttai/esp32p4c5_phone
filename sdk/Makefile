# Build an ELF app of the phone without an ESP-IDF project, and pack it into a .bpk.
#
#   make APP=examples/hello          -> build/hello.elf and out/hello.bpk (copy the .bpk to /packages on the card)
#
# An app folder holds: one or more .c files, manifest.json (runtime.entry = <name>.elf), icon.png (optional).
# Needs the RISC-V toolchain of ESP-IDF (riscv32-esp-elf-gcc); TC= points to another one.

APP   ?= examples/hello
NAME  := $(notdir $(APP))
SDK   := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
TC    ?= $(lastword $(sort $(wildcard $(HOME)/.espressif/tools/riscv32-esp-elf/esp-*/riscv32-esp-elf)))
CC    := $(TC)/bin/riscv32-esp-elf-gcc
STRIP := $(TC)/bin/riscv32-esp-elf-strip
API   ?= $(SDK)../components/phone_elf_apps/include

SRCS  := $(wildcard $(APP)/*.c)

# the same flags as the loader's project_elf(): position independent, no startup files, entry app_main (main is renamed)
CFLAGS  := -march=rv32imafc_zicsr_zifencei -mabi=ilp32f -Os -Wall -Wextra -fPIC -shared -nostartfiles -nostdlib \
           -fdata-sections -ffunction-sections -fvisibility=hidden -Dmain=app_main \
           -isystem $(TC)/picolibc/include -I$(SDK) -I$(API)
LDFLAGS := -e app_main -Wl,--gc-sections -Wl,--strip-all -Wl,--strip-discarded

all: out/$(NAME).bpk

build/$(NAME).elf: $(SRCS) $(SDK)phone_elf_sdk.h $(API)/phone_elf_api.h
	@mkdir -p build
	$(CC) $(CFLAGS) $(SRCS) $(LDFLAGS) -o $@
	$(STRIP) --strip-unneeded --remove-section=.comment --remove-section=.dynamic --remove-section=.riscv.attributes $@
	@ls -l $@

out/$(NAME).bpk: build/$(NAME).elf $(APP)/manifest.json
	@rm -rf build/pkg-$(NAME) && mkdir -p build/pkg-$(NAME) out
	cp build/$(NAME).elf build/pkg-$(NAME)/$(NAME).elf
	cp $(APP)/manifest.json build/pkg-$(NAME)/
	@if [ -f $(APP)/icon.png ]; then cp $(APP)/icon.png build/pkg-$(NAME)/; fi
	python3 $(SDK)../tools/make_bpk.py build/pkg-$(NAME) $@

clean:
	rm -rf build out

.PHONY: all clean
