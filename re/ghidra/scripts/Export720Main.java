// Ghidra headless script: build an analysed program of the 720 Degrees main CPU code
// (DEC T-11, using the PDP11 processor module from re/ghidra/PDP11) and export decompiled C.
// Usage (postScript): Export720Main.java <input dir> <output dir>
//   input dir: main_fixed.bin (loaded at 0x8000 by the importer), bank_<seg>.bin, seeds.txt
//@category 720Degrees
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.program.model.address.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class Export720Main extends GhidraScript {
	private final Map<String, AddressSpace> spaces = new HashMap<>();

	private Address addr(String seg, long a) {
		if (seg.equals("F") || seg.equals("RAM")) return toAddr(a);
		return spaces.get(seg).getAddress(a);
	}

	private void label(long a, String name) throws Exception {
		currentProgram.getSymbolTable().createLabel(toAddr(a), name, SourceType.USER_DEFINED);
	}

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		File in = new File(args[0]);
		String out = args[1];
		Memory mem = currentProgram.getMemory();
		int tx = currentProgram.startTransaction("720 setup");
		try {
			mem.createUninitializedBlock("RAM", toAddr(0x0000), 0x1000, false);
			MemoryBlock io = mem.createUninitializedBlock("IO", toAddr(0x1000), 0x1000, false);
			io.setVolatile(true);
			mem.createUninitializedBlock("VRAM", toAddr(0x2000), 0x2000, false);
			for (File f : in.listFiles()) {
				String n = f.getName();
				if (!n.startsWith("bank_")) continue;
				String seg = n.substring(5, n.length() - 4);
				long base = seg.startsWith("A") ? 0x4000 : 0x6000;
				try (InputStream is = new FileInputStream(f)) {
					MemoryBlock b = mem.createInitializedBlock(seg, toAddr(base), is, f.length(), monitor, true);
					spaces.put(seg, b.getStart().getAddressSpace());
				}
			}
			String[][] hw = {
				{"1000", "PALETTE_MO"}, {"1080", "PALETTE_ALPHA"}, {"1100", "PALETTE_PF"},
				{"1400", "BANK_A"}, {"1402", "BANK_B"}, {"1580", "ACK_SNDCMD_IRQ"}, {"15A0", "SOUND_RESET"},
				{"15C0", "ACK_32V_IRQ"}, {"15E0", "ACK_VBLANK_IRQ"}, {"1600", "IRQ_ENABLE"}, {"1680", "SOUND_CMD"},
				{"1700", "XSCROLL"}, {"1780", "YSCROLL"}, {"1800", "IN0"}, {"1C00", "SOUND_RESPONSE"},
				{"2000", "VRAM_WINDOW"}, {"3800", "MO_RAM"},
				{"01C4", "frame_counter"}, {"0191", "irq32v_count"}, {"0192", "vblank_count"},
				{"01D0", "slapstic_bank_shadow"}, {"01D2", "bank_a_shadow"}, {"01D4", "bank_b_shadow"},
				{"0F59", "level_id"}, {"0FED", "irq32v_busy"},
			};
			for (String[] h : hw) label(Long.parseLong(h[0], 16), h[1]);

			// disassemble every known instruction, create functions at entries
			BufferedReader br = new BufferedReader(new FileReader(new File(in, "seeds.txt")));
			List<String[]> entries = new ArrayList<>();
			Map<String, AddressSet> sets = new HashMap<>();
			String line;
			while ((line = br.readLine()) != null) {
				String[] p = line.trim().split("\\s+");
				if (p.length < 3) continue;
				if (p[0].equals("E")) entries.add(p);
				else sets.computeIfAbsent(p[1], k -> new AddressSet()).add(addr(p[1], Long.parseLong(p[2], 16)));
			}
			br.close();
			for (Map.Entry<String, AddressSet> e : sets.entrySet()) {
				DisassembleCommand cmd = new DisassembleCommand(e.getValue(), null, false);
				cmd.applyTo(currentProgram, monitor);
			}
			for (String[] e : entries) {
				Address a = addr(e[1], Long.parseLong(e[2], 16));
				String name = String.format("fn_%s_%s", e[1], e[2]);
				if (e[3].startsWith("irq") || e[3].equals("reset")) name = e[3] + "_" + e[2];
				Function f = getFunctionAt(a);
				if (f == null) f = createFunction(a, name);
				else f.setName(name, SourceType.USER_DEFINED);
			}
		} finally {
			currentProgram.endTransaction(tx, true);
		}

		DecompInterface ifc = new DecompInterface();
		ifc.openProgram(currentProgram);
		Map<String, PrintWriter> files = new TreeMap<>();
		int n = 0, failed = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			String space = f.getEntryPoint().getAddressSpace().getName();
			String key = space.equals("ram") ? "F" : space;
			PrintWriter pw = files.get(key);
			if (pw == null) {
				pw = new PrintWriter(new FileWriter(out + "/main_" + key + "_decompiled.c"));
				pw.println("/* 720 Degrees main program, segment " + key + " - Ghidra decompiler output (PDP11/T11 module).");
				pw.println("   Pseudo-C for reverse engineering reference; the playable port uses the verified translation in port/src/game. */");
				files.put(key, pw);
			}
			DecompileResults res = ifc.decompileFunction(f, 120, monitor);
			if (res != null && res.decompileCompleted()) {
				pw.println("// " + f.getEntryPoint());
				pw.println(res.getDecompiledFunction().getC());
				n++;
			} else {
				pw.println("// " + f.getEntryPoint() + " " + f.getName() + ": decompilation failed: " + (res == null ? "" : res.getErrorMessage()));
				failed++;
			}
		}
		for (PrintWriter pw : files.values()) pw.close();
		println("decompiled " + n + " functions, failed " + failed);
	}
}
