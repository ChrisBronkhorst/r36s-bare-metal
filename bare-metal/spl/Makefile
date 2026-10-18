# Build our SPL, in place of Rockchip's loader.
#   make          spl.bin, and idbloader.img for the card (sector 64)
#   make dump     show the disassembly

CC = clang --target=aarch64-none-elf
LD = ld.lld
CFLAGS = -O2 -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
	-mgeneral-regs-only -mstrict-align -fno-stack-protector \
	-fno-asynchronous-unwind-tables -I../kernel

# The kernel's board set-up, for flashing the backlight when we fail
KERNEL = board.o backlight.o i2c.o rk817.o
OBJS = start.o main.o sd.o $(addprefix k-,$(KERNEL))

# Rockchip's RAM setup, stage 1 (still theirs), from the RAM blob work
DDR = ../ddr-blob/rk3326-ddr-v1.10.bin

idbloader.img: spl.bin $(DDR) pack-idb.py
	python3 -I pack-idb.py $(DDR) spl.bin $@

# The same with our own RAM setup (bare-metal/ddr) as stage 1
idbloader-ours.img: spl.bin ../ddr/ddr.bin pack-idb.py
	python3 -I pack-idb.py ../ddr/ddr.bin spl.bin $@

../ddr/ddr.bin: FORCE
	$(MAKE) -C ../ddr ddr.bin

FORCE:

spl.bin: spl.elf
	$(LD) -T linker.ld --oformat binary -o $@ $(OBJS)
	@echo "spl.bin: $$(wc -c < $@ | tr -d ' ') bytes"

spl.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -o $@ $(OBJS)

$(OBJS): sd.h report.h

k-%.o: ../kernel/%.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) -c $< -o $@

dump: spl.elf
	objdump -d spl.elf

clean:
	rm -f *.o spl.elf spl.bin idbloader.img idbloader-ours.img

.PHONY: dump clean FORCE
