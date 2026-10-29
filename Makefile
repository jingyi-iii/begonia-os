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
KERNEL_DIR := kernel
LIB_DIR    := lib

# boot.S is 32-bit assembly, the C files are freestanding and 32-bit too: keep
# -m32 even when TARGET=x86_64-elf, because the image is still 32-bit here.
# -fno-pic keeps addressing absolute, so `lgdt` and the far jump stay simple.
# The kernel calls into the arch drivers (com_print and friends), so their
# headers are on the path by bare name, next to the shared ones.
ASFLAGS := --32
# Declared-but-unused parameters are the norm while the process and thread
# entry points are stubs; drop this once their bodies land.
CFLAGS  := -m32 -O2 -ffreestanding -fno-pic -fno-stack-protector -fno-builtin \
           -I$(INC_DIR) -I$(ARCH_DIR)/i686 -fno-asynchronous-unwind-tables \
           -Wno-unused-parameter \
           -Wall -Wextra
BUILD   := build
OBJ     := $(BUILD)/boot.o $(BUILD)/cpu.o $(BUILD)/gdt.o $(BUILD)/com.o $(BUILD)/pic.o \
           $(BUILD)/interrupt.o $(BUILD)/interrupt_asm.o \
           $(BUILD)/task.o $(BUILD)/task_asm.o \
           $(BUILD)/string.o $(BUILD)/spinlock.o $(BUILD)/heap.o \
           $(BUILD)/process.o $(BUILD)/irq.o \
           $(BUILD)/kerner.o
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
$(BUILD)/gdt.o: $(ARCH_DIR)/i686/gdt.c $(INC_DIR)/typedefs.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# com.c: the COM1 console; the 16550 and port I/O are PC hardware.
$(BUILD)/com.o: $(ARCH_DIR)/i686/com.c $(ARCH_DIR)/i686/com.h \
                $(ARCH_DIR)/i686/io.h $(INC_DIR)/typedefs.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# pic.c: the 8259 pair and the PIT.
$(BUILD)/pic.o: $(ARCH_DIR)/i686/pic.c $(ARCH_DIR)/i686/pic.h \
                $(ARCH_DIR)/i686/io.h $(INC_DIR)/typedefs.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# interrupt.c is the IDT, interrupt.S the ISR entry points.  The assembly
# object cannot be interrupt.o: interrupt.c owns that name.
$(BUILD)/interrupt.o: $(ARCH_DIR)/i686/interrupt.c $(ARCH_DIR)/i686/com.h \
                      $(ARCH_DIR)/i686/pic.h $(INC_DIR)/typedefs.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BUILD)/interrupt_asm.o: $(ARCH_DIR)/i686/interrupt.S | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

# cpu.c: the x86 instructions, so that lib/ stays portable C.
$(BUILD)/cpu.o: $(ARCH_DIR)/i686/cpu.c $(ARCH_DIR)/i686/cpu.h \
                 $(INC_DIR)/typedefs.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# task.c: TSS, LDT and the task context.
$(BUILD)/task.o: $(ARCH_DIR)/i686/task.c $(ARCH_DIR)/i686/gdt.h \
                  $(INC_DIR)/task.h $(INC_DIR)/typedefs.h \
                  $(INC_DIR)/string.h $(INC_DIR)/errno.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# string.c: memset and memcpy; there is no libc.
$(BUILD)/string.o: $(LIB_DIR)/string.c $(INC_DIR)/string.h $(INC_DIR)/typedefs.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# spinlock.c: the locks behind include/spinlock.h; taking one disables interrupts.
$(BUILD)/spinlock.o: $(LIB_DIR)/spinlock.c $(INC_DIR)/spinlock.h \
                     $(INC_DIR)/typedefs.h $(ARCH_DIR)/i686/cpu.h \
                     $(INC_DIR)/errno.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# heap.c: the 16 MiB static pool behind malloc()/free().
$(BUILD)/heap.o: $(LIB_DIR)/heap.c $(INC_DIR)/heap.h $(INC_DIR)/list.h \
                 $(INC_DIR)/spinlock.h $(INC_DIR)/typedefs.h \
                 $(INC_DIR)/string.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# arch/i686/task.S: the register and stack switch itself; it cannot be inlined
# because it changes ESP, which GCC assumes constant inside a function.
$(BUILD)/task_asm.o: $(ARCH_DIR)/i686/task.S | $(BUILD)
	$(AS) $(ASFLAGS) -o $@ $<

# process.c: threads and processes on top of task.c.
$(BUILD)/process.o: $(KERNEL_DIR)/process.c $(INC_DIR)/process.h \
                    $(INC_DIR)/heap.h $(INC_DIR)/list.h $(INC_DIR)/log.h \
                    $(INC_DIR)/spinlock.h $(INC_DIR)/string.h \
                    $(INC_DIR)/task.h $(INC_DIR)/typedefs.h \
                    $(INC_DIR)/errno.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# irq.c: the irqline/irq registry on top of interrupt_mask()/unmask().
$(BUILD)/irq.o: $(KERNEL_DIR)/irq.c $(INC_DIR)/irq.h $(INC_DIR)/heap.h \
                $(INC_DIR)/log.h $(INC_DIR)/spinlock.h $(INC_DIR)/list.h \
                $(INC_DIR)/errno.h $(INC_DIR)/typedefs.h \
                $(ARCH_DIR)/i686/interrupt.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

# kerner.c: the kernel itself (kmain).
$(BUILD)/kerner.o: $(KERNEL_DIR)/kerner.c $(INC_DIR)/boot.h \
                    $(ARCH_DIR)/i686/com.h $(INC_DIR)/task.h | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

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
