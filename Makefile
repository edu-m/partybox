# partybox -- build with clang + lld-link against the edk2 MdePkg headers only.

EDK2     ?= edk2
CC       := clang
LD       := lld-link
PYTHON   ?= python3
BUILD    := build
VERSION  := $(shell tr -d '[:space:]' < VERSION)

OVMF_CODE ?= /usr/share/edk2/x64/OVMF_CODE.4m.fd
OVMF_VARS ?= /usr/share/edk2/x64/OVMF_VARS.4m.fd
LIMINE    ?= /usr/share/limine/BOOTX64.EFI
QEMU_MEM  ?= 1024

CFLAGS := --target=x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
          -fno-stack-protector -mno-stack-arg-probe -std=c11 -O2 -Wall -Wextra \
          -Wno-unused-parameter -I$(EDK2)/MdePkg/Include -I$(EDK2)/MdePkg/Include/X64 -Isrc \
          -DPARTYBOX_VERSION='L"$(VERSION)"'
LDFLAGS := -subsystem:efi_application -entry:efi_main -nodefaultlib -machine:x64

SRCS := src/main.c src/gfx.c src/scan.c src/lib.c
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)
TESTS := hello exitdata modeswitch lowhog

.PHONY: all tests esp run run-sb clean font version doom check fuzz

all: $(BUILD)/partybox.efi $(BUILD)/blackjack.efi

$(BUILD)/partybox.efi: $(OBJS)
	$(LD) $(LDFLAGS) -out:$@ $^

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# The version string is baked into main.o.
$(BUILD)/main.o: VERSION

$(BUILD):
	mkdir -p $(BUILD)/test $(BUILD)/apps

version:
	@echo $(VERSION)

# Regenerate the embedded font (needs Pillow and Noto Sans / DejaVu Sans installed).
font:
	$(PYTHON) tools/genfont.py ui src/font_data.h
	$(PYTHON) tools/genfont.py cards apps/blackjack/font_cards.h

# ---- showcase apps (share partybox's gfx + lib) -------------------------------
$(BUILD)/apps/blackjack.o: apps/blackjack/blackjack.c apps/blackjack/font_cards.h $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -Iapps/blackjack -c $< -o $@

$(BUILD)/blackjack.efi: $(BUILD)/apps/blackjack.o $(BUILD)/gfx.o $(BUILD)/lib.o
	$(LD) $(LDFLAGS) -out:$@ $^

# ---- test payloads ----------------------------------------------------------
tests: $(TESTS:%=$(BUILD)/test/%.efi)

$(BUILD)/test/%.o: test/testapp.c src/lib.h | $(BUILD)
	$(CC) $(CFLAGS) -DMODE_$(shell echo $* | tr a-z A-Z) -c $< -o $@

$(BUILD)/test/%.efi: $(BUILD)/test/%.o $(BUILD)/lib.o
	$(LD) $(LDFLAGS) -out:$@ $^

# ---- host-side tests ------------------------------------------------------------
# The real efi_main() runs on the host under ASan/UBSan against a fake firmware
# (test/host/fakefw.c): leaks, memory errors, spec violations, out-of-memory paths.
# `make fuzz` runs libFuzzer on the PE, config and device-path parsers.
HOST    := $(BUILD)/host
HFLAGS  := -g -O1 -fshort-wchar -I$(EDK2)/MdePkg/Include -I$(EDK2)/MdePkg/Include/X64 -Isrc \
           -Wno-macro-redefined -fsanitize=address,undefined -fno-sanitize=function \
           -fno-sanitize-recover=undefined -fno-omit-frame-pointer
HREN    := -Dmemset=pb_memset -Dmemcpy=pb_memcpy -Dmemmove=pb_memmove -Dmemcmp=pb_memcmp
FUZZ_SECS ?= 60

$(HOST)/fakefw: test/host/fakefw.c $(SRCS) $(wildcard src/*.h) VERSION
	mkdir -p $(HOST)
	for f in $(SRCS); do $(CC) $(HFLAGS) $(HREN) -DPARTYBOX_VERSION='L"host"' -c $$f \
		-o $(HOST)/$$(basename $$f .c).o || exit 1; done
	$(CC) $(HFLAGS) -c test/host/fakefw.c -o $(HOST)/fakefw.o
	$(CC) $(HFLAGS) $(HOST)/main.o $(HOST)/scan.o $(HOST)/gfx.o $(HOST)/lib.o $(HOST)/fakefw.o -o $@

check: all tests $(HOST)/fakefw
	test/host/run.sh $(HOST)

fuzz: all tests
	EDK2=$(EDK2) test/host/fuzz.sh $(FUZZ_SECS)

# ---- DOOM: third-party apps/uefidoom submodule, built with edk2 + edk2-libc ---
doom:
	apps/uefidoom-build/build.sh

# ---- QEMU playground: Limine -> partybox -> test apps --------------------------
ESP := $(BUILD)/esp.img
ESP_MB ?= 128
# Extra folders copied as-is into PARTYBOX\<name>\ when present (git-ignored,
# e.g. doom/ with doom.efi and its WADs). Override: make esp ESP_EXTRA="a b".
ESP_EXTRA ?= $(wildcard doom)

esp: $(ESP)

$(ESP): $(BUILD)/partybox.efi $(BUILD)/blackjack.efi tests examples/limine.conf $(shell find $(ESP_EXTRA) -type f 2>/dev/null) $(wildcard $(BUILD)/doom.efi $(BUILD)/AudioDxe.efi)
	rm -f $@
	mformat -C -F -i $@ -T $$(( $(ESP_MB) * 2048 )) -h 64 -s 32 ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/EFI/partybox ::/EFI/partybox/PARTYBOX ::/EFI/partybox/PARTYBOX/tools
	mcopy -i $@ $(LIMINE) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ examples/limine.conf ::/EFI/BOOT/limine.conf
	mcopy -i $@ $(BUILD)/partybox.efi ::/EFI/partybox/partybox.efi
	mcopy -i $@ $(BUILD)/test/hello.efi $(BUILD)/test/exitdata.efi $(BUILD)/test/modeswitch.efi $(BUILD)/test/lowhog.efi \
		::/EFI/partybox/PARTYBOX/
	mcopy -i $@ $(BUILD)/test/hello.efi ::/EFI/partybox/PARTYBOX/tools/nested-hello.efi
	mcopy -i $@ $(BUILD)/blackjack.efi ::/EFI/partybox/PARTYBOX/
	$(foreach d,$(ESP_EXTRA),mcopy -s -i $@ $(d) ::/EFI/partybox/PARTYBOX/ &&) true
	# A source-built doom.efi (make doom) replaces whatever doom/ shipped with.
	if [ -f $(BUILD)/doom.efi ] && [ -d doom ]; then \
		mcopy -o -i $@ $(BUILD)/doom.efi ::/EFI/partybox/PARTYBOX/doom/doom.efi; fi
	# The HD Audio driver goes in a subfolder (not scanned by partybox) for
	# startup.nsh to `load` before the game; see examples/doom-startup.nsh.
	if [ -f $(BUILD)/AudioDxe.efi ] && [ -d doom ]; then \
		mdir -i $@ ::/EFI/partybox/PARTYBOX/doom/drivers >/dev/null 2>&1 || \
			mmd -i $@ ::/EFI/partybox/PARTYBOX/doom/drivers; \
		mcopy -o -i $@ $(BUILD)/AudioDxe.efi ::/EFI/partybox/PARTYBOX/doom/drivers/AudioDxe.efi; fi

$(BUILD)/OVMF_VARS.fd: | $(BUILD)
	cp $(OVMF_VARS) $@

run: $(ESP) $(BUILD)/OVMF_VARS.fd
	qemu-system-x86_64 -machine q35 -m $(QEMU_MEM) -net none \
		-drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
		-drive if=pflash,format=raw,file=$(BUILD)/OVMF_VARS.fd \
		-drive format=raw,file=$(ESP) $(QEMUFLAGS)

# ---- Secure Boot playground ----------------------------------------------------
# Throwaway test keys enrolled as PK/KEK/db with SB enforced. Limine isn't signed,
# so partybox boots directly as BOOTX64.EFI here. PARTYBOX holds one signed and
# two unsigned apps.
SB           := $(BUILD)/sb
SB_GUID      := 3a1f7c52-6b0d-4c8e-9e21-5d7a0f4b8c19
OVMF_CODE_SB ?= /usr/share/edk2/x64/OVMF_CODE.secboot.4m.fd

$(SB)/db.pem:
	mkdir -p $(SB)
	$(PYTHON) tools/sbtest.py keygen $(SB)

$(SB)/OVMF_VARS.fd: $(SB)/db.pem
	virt-fw-vars --input $(OVMF_VARS) --output $@ --no-microsoft --sb \
		--set-pk $(SB_GUID) $< --add-kek $(SB_GUID) $< --add-db $(SB_GUID) $<

$(SB)/esp.img: $(BUILD)/partybox.efi tests $(SB)/db.pem
	rm -f $@ $(SB)/*.signed.efi
	$(PYTHON) tools/sbtest.py sign $(SB)/db.key $(SB)/db.pem $(BUILD)/partybox.efi $(SB)/partybox.signed.efi
	$(PYTHON) tools/sbtest.py sign $(SB)/db.key $(SB)/db.pem $(BUILD)/test/hello.efi $(SB)/hello-signed.signed.efi
	mformat -C -F -i $@ -T 131072 -h 64 -s 32 ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/EFI/BOOT/PARTYBOX
	mcopy -i $@ $(SB)/partybox.signed.efi ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ $(SB)/hello-signed.signed.efi ::/EFI/BOOT/PARTYBOX/hello-signed.efi
	mcopy -i $@ $(BUILD)/test/hello.efi $(BUILD)/test/exitdata.efi ::/EFI/BOOT/PARTYBOX/

run-sb: $(SB)/esp.img $(SB)/OVMF_VARS.fd
	cp $(SB)/OVMF_VARS.fd $(SB)/OVMF_VARS.run.fd
	qemu-system-x86_64 -machine q35,smm=on -global driver=cfi.pflash01,property=secure,value=on \
		-m 512 -net none \
		-drive if=pflash,format=raw,unit=0,readonly=on,file=$(OVMF_CODE_SB) \
		-drive if=pflash,format=raw,unit=1,file=$(SB)/OVMF_VARS.run.fd \
		-drive format=raw,file=$< $(QEMUFLAGS)

clean:
	rm -rf $(BUILD)
