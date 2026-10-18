# Build our stage 1, the RAM setup, in place of Rockchip's DDR blob.
#   make          ddr.bin (the SPL's Makefile packs it with the SPL)
#   make dump     show the disassembly

CC = clang --target=aarch64-none-elf
LD = ld.lld
CFLAGS = -Os -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
	-mgeneral-regs-only -mstrict-align -fno-stack-protector \
	-fno-asynchronous-unwind-tables -I../kernel

# The kernel's board set-up, for flashing the backlight when we fail
KERNEL = board.o backlight.o i2c.o rk817.o
OBJS = start.o main.o dram.o $(addprefix k-,$(KERNEL))

# Padded to whole 2 KB blocks, like Rockchip's blob
ddr.bin: ddr.elf
	$(LD) -T linker.ld --oformat binary -o $@ $(OBJS)
	@python3 -I -c "import sys; b=open('$@','rb').read(); open('$@','wb').write(b + bytes(-len(b) % 2048))"
	@echo "ddr.bin: $$(wc -c < $@ | tr -d ' ') bytes"

ddr.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -o $@ $(OBJS)

$(OBJS): dram.h report.h

k-%.o: ../kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) -c $< -o $@

dump: ddr.elf
	objdump -d ddr.elf

clean:
	rm -f *.o ddr.elf ddr.bin

.PHONY: dump clean
