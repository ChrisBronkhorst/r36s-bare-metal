# Build kernel.img for the R36S (RK3326, 64-bit ARM).
#   make          build kernel.img
#   make dump     show the disassembly
#   make clean

CC = clang --target=aarch64-none-elf
LD = ld.lld
# -ffreestanding: no C library or OS underneath us
# -mgeneral-regs-only: no floating point/SIMD registers (not set up yet)
# -mstrict-align: no unaligned memory accesses (they fault on device memory,
#   which is all memory before the MMU is on, and the hardware registers after)
CFLAGS = -O2 -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
	-mgeneral-regs-only -mstrict-align -fno-stack-protector \
	-fno-asynchronous-unwind-tables

OBJS = start.o vectors.o main.o screen.o console.o font.o gpio.o buttons.o exception.o mmu.o speed.o gic.o timer.o backlight.o i2c.o rk817.o dump.o scan.o display.o board.o

kernel.img: kernel.elf
	$(LD) -T linker.ld --oformat binary -o $@ $(OBJS)
	@echo "kernel.img: $$(wc -c < $@ | tr -d ' ') bytes"

# Our kernel in Rockchip's loader format, to start without U-Boot
# (./card-loader.sh puts it on the card)
uboot.img: kernel.img pack-loader.py
	python3 -I pack-loader.py kernel.img $@

kernel.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -o $@ $(OBJS)

$(OBJS): screen.h console.h gpio.h buttons.h exception.h sysreg.h mmu.h speed.h gic.h timer.h backlight.h i2c.h rk817.h dump.h scan.h display.h display_tables.h board.h

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) -c $< -o $@

dump: kernel.elf
	objdump -d kernel.elf

clean:
	rm -f *.o kernel.elf kernel.img uboot.img

.PHONY: dump clean
