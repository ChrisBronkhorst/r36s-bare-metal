// The GIC has two parts, both memory-mapped:
//  - the distributor (GICD), shared: which interrupts are enabled, their
//    priorities, and which cores they go to
//  - the CPU interface (GICC), one per core (each core sees its own at the
//    same address): the priority mask, and acknowledge / end-of-interrupt
//
// ARM Trusted Firmware set the GIC up before us: secure interrupts are in
// "group 0" and go to it (as FIQs); everything else is "group 1", which is all
// that we, in the non-secure world, can see or change.

#include "gic.h"

#define GICD 0xff131000UL
#define GICC 0xff132000UL

#define GICD_CTLR       0x000
#define GICD_ISENABLER  0x100   // write 1 to enable, 32 interrupts per register
#define GICD_ISPENDR    0x200   // read: which are pending
#define GICD_IPRIORITYR 0x400   // one byte per interrupt
#define GICD_ITARGETSR  0x800   // one byte per interrupt: bit n = core n

#define GICC_CTLR 0x00
#define GICC_PMR  0x04   // priority mask: only more urgent than this get through
#define GICC_IAR  0x0c   // acknowledge: read to get the interrupt's number
#define GICC_EOIR 0x10   // end of interrupt: write the number back

static inline uint32_t read32(uintptr_t addr) { return *(volatile uint32_t *)addr; }
static inline void write32(uintptr_t addr, uint32_t v) { *(volatile uint32_t *)addr = v; }

void gic_init(void)
{
	write32(GICD + GICD_CTLR, 1);    // distributor: pass group 1 interrupts on
	write32(GICC + GICC_PMR, 0xff);  // let every priority through
	write32(GICC + GICC_CTLR, 1);    // CPU interface: signal group 1 as IRQ
}

// Set one interrupt's byte in a byte-per-interrupt register
static void set_byte(uintptr_t reg, uint32_t id, uint8_t value)
{
	uintptr_t addr = reg + (id & ~3U);
	uint32_t shift = (id & 3) * 8;
	write32(addr, (read32(addr) & ~(0xffU << shift)) | (uint32_t)value << shift);
}

void gic_enable(uint32_t id, uint8_t priority)
{
	set_byte(GICD + GICD_IPRIORITYR, id, priority);
	// A shared interrupt only goes to the cores in its target byte, which
	// starts out empty. (Private ones always go to their own core.)
	if (id >= 32)
		set_byte(GICD + GICD_ITARGETSR, id, 1 << 0);   // core 0
	write32(GICD + GICD_ISENABLER + id / 32 * 4, 1U << (id % 32));
}

uint32_t gic_ack(void)
{
	return read32(GICC + GICC_IAR) & 0x3ff;
}

void gic_done(uint32_t id)
{
	write32(GICC + GICC_EOIR, id);
}

int gic_pending(uint32_t id)
{
	return read32(GICD + GICD_ISPENDR + id / 32 * 4) >> (id % 32) & 1;
}
