# Build our EL3 monitor, the part that replaces ARM Trusted Firmware.
#   make          monitor.bin, and trust.img for the card
#   make dump     show the disassembly

CC = clang --target=aarch64-none-elf
LD = ld.lld
CFLAGS = -O2 -Wall -Wextra -ffreestanding -fno-builtin -nostdlib \
	-mgeneral-regs-only -mstrict-align -fno-stack-protector \
	-fno-asynchronous-unwind-tables

OBJS = start.o main.o

trust.img: monitor.bin pack-trust.py
	python3 -I pack-trust.py monitor.bin $@

monitor.bin: monitor.elf
	$(LD) -T linker.ld --oformat binary -o $@ $(OBJS)
	@echo "monitor.bin: $$(wc -c < $@ | tr -d ' ') bytes"

monitor.elf: $(OBJS) linker.ld
	$(LD) -T linker.ld -o $@ $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) -c $< -o $@

dump: monitor.elf
	objdump -d monitor.elf

clean:
	rm -f *.o monitor.elf monitor.bin trust.img

.PHONY: dump clean
