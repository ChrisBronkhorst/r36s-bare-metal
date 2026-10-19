# Build kernel.img for the R36S (RK3326, 64-bit ARM).
#   make          build kernel.img
#   make push     send it to the running console over USB, save it to the
#                 card, and start it
#   make try      the same, but RAM only: the card keeps its kernel
#   make reboot   restart the console (it starts from the card)
#   make monitor  write our EL3 monitor (../monitor) to the card; it runs
#                 from the next boot
#   make shell    type commands to the running console (./shell.py CMD: one)
#   make dump     show the disassembly
#   make clean

CC = clang --target=aarch64-none-elf
LD = ld.lld
# -ffreestanding: no C library or OS underneath us
# -mgeneral-regs-only: no floating point/SIMD registers (not set up yet)
# -mstrict-align: no unaligned memory accesses (they fault on device memory,
#   which is all memory before the MMU is on, and the hardware registers after)
# make FLASHES=1: the backlight flashes once per display start-up step
# make SPEEDTEST=1: also time the start-up work before the MMU is on
CFLAGS = $(if $(FLASHES),-DFLASHES) $(if $(SPEEDTEST),-DSPEEDTEST) -O2 -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
	-mgeneral-regs-only -mstrict-align -fno-stack-protector \
	-fno-asynchronous-unwind-tables

OBJS = start.o vectors.o main.o screen.o console.o font.o gpio.o buttons.o exception.o mmu.o speed.o gic.o timer.o backlight.o i2c.o rk817.o dump.o scan.o display.o board.o monitor.o ram.o rom.o usb.o net.o log.o loader.o chain.o shell.o cpufreq.o thermal.o matmul.o cores.o gpu.o render.o joystick.o sd.o

kernel.img: kernel.elf
	$(LD) -T linker.ld --oformat binary -o $@ $(OBJS)
	@echo "kernel.img: $$(wc -c < $@ | tr -d ' ') bytes"

# Our kernel in Rockchip's loader format, to start without U-Boot
# (./card-loader.sh puts it on the card)
uboot.img: kernel.img pack-loader.py
	python3 -I pack-loader.py kernel.img $@

kernel.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -o $@ $(OBJS)

$(OBJS): screen.h console.h gpio.h buttons.h exception.h sysreg.h mmu.h speed.h gic.h timer.h backlight.h i2c.h rk817.h dump.h scan.h display.h display_tables.h board.h monitor.h ram.h rom.h usb.h net.h log.h loader.h shell.h cpufreq.h thermal.h matmul.h cores.h gpu.h shaders.h render.h joystick.h ../spl/sd.h ../ddr/report.h ../spl/report.h

sd.o: ../spl/sd.c ../spl/sd.h
	$(CC) $(CFLAGS) -c $< -o $@

# The one file allowed to use the floating point / SIMD registers
matmul.o: matmul.c
	$(CC) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -c $< -o $@

render.o: render.c
	$(CC) $(filter-out -mgeneral-regs-only,$(CFLAGS)) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) -c $< -o $@

push: kernel.img
	python3 -I push.py kernel.img

try: kernel.img
	python3 -I push.py --try kernel.img

reboot:
	python3 -I push.py --reboot

monitor:
	$(MAKE) -C ../monitor trust.img
	python3 -I push.py --monitor ../monitor/trust.img

shell:
	python3 -I shell.py

dump: kernel.elf
	objdump -d kernel.elf

clean:
	rm -f *.o kernel.elf kernel.img uboot.img

.PHONY: push try reboot shell monitor dump clean
