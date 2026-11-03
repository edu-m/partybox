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

CFLAGS := --target=x86_64-unknown-windows -ffreestanding -fshort-wchar -mno-red-zone \
          -fno-stack-protector -mno-stack-arg-probe -std=c11 -O2 -Wall -Wextra \
          -Wno-unused-parameter -I$(EDK2)/MdePkg/Include -I$(EDK2)/MdePkg/Include/X64 -Isrc \
          -DPARTYBOX_VERSION='L"$(VERSION)"'
LDFLAGS := -subsystem:efi_application -entry:efi_main -nodefaultlib -machine:x64

SRCS := src/main.c src/gfx.c src/scan.c src/lib.c
OBJS := $(SRCS:src/%.c=$(BUILD)/%.o)
TESTS := hello exitdata modeswitch

.PHONY: all tests esp run run-sb clean font version

all: $(BUILD)/partybox.efi

$(BUILD)/partybox.efi: $(OBJS)
	$(LD) $(LDFLAGS) -out:$@ $^

$(BUILD)/%.o: src/%.c $(wildcard src/*.h) | $(BUILD)
	$(CC) $(CFLAGS) -c $< -o $@

# The version string is baked into main.o.
$(BUILD)/main.o: VERSION

$(BUILD):
	mkdir -p $(BUILD)/test

version:
	@echo $(VERSION)

# Regenerate the embedded font (needs Pillow and Noto Sans / DejaVu Sans installed).
font:
	$(PYTHON) tools/genfont.py src/font_data.h

# ---- test payloads ----------------------------------------------------------
tests: $(TESTS:%=$(BUILD)/test/%.efi)

$(BUILD)/test/%.o: test/testapp.c src/lib.h | $(BUILD)
	$(CC) $(CFLAGS) -DMODE_$(shell echo $* | tr a-z A-Z) -c $< -o $@

$(BUILD)/test/%.efi: $(BUILD)/test/%.o $(BUILD)/lib.o
	$(LD) $(LDFLAGS) -out:$@ $^

# ---- QEMU playground: Limine -> partybox -> test apps --------------------------
ESP := $(BUILD)/esp.img

esp: $(ESP)

$(ESP): $(BUILD)/partybox.efi tests examples/limine.conf
	rm -f $@
	mformat -C -F -i $@ -T 131072 -h 64 -s 32 ::
	mmd -i $@ ::/EFI ::/EFI/BOOT ::/EFI/partybox ::/EFI/partybox/PARTYBOX ::/EFI/partybox/PARTYBOX/tools
	mcopy -i $@ $(LIMINE) ::/EFI/BOOT/BOOTX64.EFI
	mcopy -i $@ examples/limine.conf ::/EFI/BOOT/limine.conf
	mcopy -i $@ $(BUILD)/partybox.efi ::/EFI/partybox/partybox.efi
	mcopy -i $@ $(BUILD)/test/hello.efi $(BUILD)/test/exitdata.efi $(BUILD)/test/modeswitch.efi \
		::/EFI/partybox/PARTYBOX/
	mcopy -i $@ $(BUILD)/test/hello.efi ::/EFI/partybox/PARTYBOX/tools/nested-hello.efi

$(BUILD)/OVMF_VARS.fd: | $(BUILD)
	cp $(OVMF_VARS) $@

run: $(ESP) $(BUILD)/OVMF_VARS.fd
	qemu-system-x86_64 -machine q35 -m 512 -net none \
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
