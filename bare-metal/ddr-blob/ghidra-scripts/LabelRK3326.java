// Labels the RK3326 hardware the DDR blob talks to, so Ghidra shows
// "DDR_PHY + 0x10" instead of a bare 0xff2a0010. Addresses come from
// mainline U-Boot (drivers/ram/rockchip/sdram_px30.c and px30.dtsi).
//@category RK3326
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.MemoryBlock;
import ghidra.program.model.symbol.SourceType;

public class LabelRK3326 extends GhidraScript {
	private void block(String name, long base, long size) throws Exception {
		Address a = toAddr(base);
		MemoryBlock b = currentProgram.getMemory().createUninitializedBlock(name, a, size, false);
		b.setVolatile(true);
		b.setWrite(true);
		createLabel(a, name, true, SourceType.USER_DEFINED);
	}

	@Override
	public void run() throws Exception {
		block("PMUGRF", 0xff010000L, 0x1000);
		block("GRF", 0xff140000L, 0x10000);
		block("UART2_DEBUG", 0xff160000L, 0x100);
		block("DDR_PHY", 0xff2a0000L, 0x1000);
		block("CRU", 0xff2b0000L, 0x1000);
		block("MSCH0", 0xff530000L, 0x1000);
		block("DDRC_PCTL", 0xff600000L, 0x10000);
		block("DDR_GRF", 0xff630000L, 0x1000);
		createLabel(toAddr(0xff0e1000L), "rk33_header", true, SourceType.USER_DEFINED);
		createLabel(toAddr(0xff0e1004L), "entry", true, SourceType.USER_DEFINED);
		disassemble(toAddr(0xff0e1004L));
		createFunction(toAddr(0xff0e1004L), "entry");
		createLabel(toAddr(0xff0e3694L), "magic_deadbeaf", true, SourceType.USER_DEFINED);
	}
}
