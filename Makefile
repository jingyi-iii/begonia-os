# Multiboot v1 kernel: GRUB (or qemu -kernel) enters _start in 32-bit
# protected mode, so there is no 16-bit boot sector and no 512-byte image.

TARGET  ?= i686-elf

AS      := $(TARGET)-as
CC      := $(TARGET)-gcc
LD      := $(TARGET)-ld

QEMU          ?= qemu-system-i386
GRUB_MKRESCUE ?= grub-mkrescue

# Paths are relative to the top of the tree.
ARCH_DIR := arch
INC_DIR  := include

# boot.S is 32-bit assembly, the C files are freestanding and 32-bit too: keep
# -m32 even when TARGET=x86_64-elf, because the image is still 32-bit here.
# -fno-pic keeps addressing absolute, so `lgdt` and the far jump stay simple.
ASFLAGS := --32
CFLAGS  := -m32 -O2 -ffreestanding -fno-pic -fno-stack-protector -fno-builtin \
           -I$(INC_DIR) -fno-asynchronous-unwind-tables -Wall -Wextra

BUILD   := build
OBJ     := $(BUILD)/boot.o $(BUILD)/gdt.o $(BUILD)/com.o $(BUILD)/pic.o \
           $(BUILD)/interrupt.o $(BUILD)/interrupt_asm.o
ELF     := $(BUILD)/boot.elf
ISO     := $(BUILD)/boot.iso
ISODIR  := $(BUILD)/iso

.PHONY: all run iso run-iso clean

all: $(ELF)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: startup/boot.S | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

# gdt.c: the flat GDT and the switch into protected mode.
$(BUILD)/gdt.o: $(ARCH_DIR)/i686/gdt.c $(INC_DIR)/boot_common.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# com.c: the COM1 console; the 16550 and port I/O are PC hardware.
$(BUILD)/com.o: $(ARCH_DIR)/i686/com.c $(ARCH_DIR)/i686/com.h \
                $(ARCH_DIR)/i686/io.h $(INC_DIR)/boot_common.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# pic.c: the 8259 pair and the PIT.
$(BUILD)/pic.o: $(ARCH_DIR)/i686/pic.c $(ARCH_DIR)/i686/pic.h \
                $(ARCH_DIR)/i686/io.h $(INC_DIR)/boot_common.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# interrupt.c is the IDT, interrupt.S the ISR entry points.  The assembly
# object cannot be interrupt.o: interrupt.c owns that name.
$(BUILD)/interrupt.o: $(ARCH_DIR)/i686/interrupt.c $(ARCH_DIR)/i686/com.h \
                      $(ARCH_DIR)/i686/pic.h $(INC_DIR)/boot_common.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/interrupt_asm.o: $(ARCH_DIR)/i686/interrupt.S | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

$(ELF): $(OBJ) linker.ld
	$(LD) -m elf_i386 -T linker.ld -o $@ $(OBJ)

# Fast path: qemu's built-in Multiboot loader.
run: $(ELF)
	@echo "Booting in qemu (exits automatically after the message)..."
	$(QEMU) -kernel $(ELF) -nographic -device isa-debug-exit || true

# GRUB path: bootable ISO built with grub-mkrescue.
$(ISO): $(ELF) | $(BUILD)
	rm -rf $(ISODIR)
	mkdir -p $(ISODIR)/boot/grub
	cp $(ELF) $(ISODIR)/boot/boot.elf
	printf 'set timeout=0\nset default=0\n\nmenuentry "BegoniaOS" {\n    multiboot /boot/boot.elf\n    boot\n}\n' > $(ISODIR)/boot/grub/grub.cfg
	$(GRUB_MKRESCUE) -o $@ $(ISODIR)

iso: $(ISO)

run-iso: $(ISO)
	@echo "Booting the GRUB ISO in qemu (exits automatically after the message)..."
	$(QEMU) -cdrom $(ISO) -boot d -nographic -device isa-debug-exit || true

clean:
	rm -rf $(BUILD)
