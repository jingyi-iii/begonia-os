# Multiboot v1 kernel: GRUB (or qemu -kernel) enters _start in 32-bit
# protected mode, so there is no 16-bit boot sector and no 512-byte image.

TARGET  ?= i686-elf

AS      := $(TARGET)-as
CC      := $(TARGET)-gcc
LD      := $(TARGET)-ld

QEMU          ?= qemu-system-i386
GRUB_MKRESCUE ?= grub-mkrescue

# boot.S is 32-bit assembly, protmode.c is freestanding 32-bit C: keep -m32
# even when TARGET=x86_64-elf, because the image is still 32-bit here.
# -fno-pic keeps addressing absolute, so `lgdt` and the far jump stay simple.
ASFLAGS := --32
CFLAGS  := -m32 -O2 -ffreestanding -fno-pic -fno-stack-protector -fno-builtin \
           -fno-asynchronous-unwind-tables -Wall -Wextra

BUILD  := build
OBJ    := $(BUILD)/boot.o $(BUILD)/protmode.o $(BUILD)/interrupt.o \
          $(BUILD)/interrupt_asm.o
ELF    := $(BUILD)/boot.elf
ISO    := $(BUILD)/boot.iso
ISODIR := $(BUILD)/iso

.PHONY: all run iso run-iso clean

all: $(ELF)

$(BUILD):
	mkdir -p $(BUILD)

$(BUILD)/boot.o: boot.S | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

# protmode.c: protected-mode entry (GDT + CR0.PE).  See the header comment.
$(BUILD)/protmode.o: protmode.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# interrupt.c is the IDT, interrupt.S is the ISR entry points.
# The assembly object cannot be interrupt.o: interrupt.c owns that
# name, so it is built as interrupt_asm.o from the same basename.
$(BUILD)/interrupt.o: interrupt.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/interrupt_asm.o: interrupt.S | $(BUILD)
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
