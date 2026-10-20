// Cleanup for Win16 NE modules (MSC 16-bit):
//  1. FAR PASCAL signatures of the imported system APIs (win16_api.txt), with custom storage
//     to keep the real order of the pascal parameters;
//  2. calling convention and stack purge of the internal functions, deduced from RET/RETF [n];
//  3. manual names/signatures from names_<MODULE>.txt (format: address name [C signature]);
//  4. propagation of the parameters from the decompiler to the database (two passes).
// Usage (headless): -postScript ApplyWin16.java [names_folder]
// @category UPSY

import java.io.File;
import java.nio.file.Files;
import java.util.*;

import ghidra.app.decompiler.*;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.data.*;
import ghidra.program.model.lang.Register;
import ghidra.program.model.listing.*;
import ghidra.program.model.listing.Function.FunctionUpdateType;
import ghidra.program.model.pcode.HighFunction;
import ghidra.program.model.pcode.HighFunctionDBUtil;
import ghidra.program.model.pcode.HighFunctionDBUtil.ReturnCommitOption;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.*;

public class ApplyWin16 extends GhidraScript {

	private DataTypeManager dtm;
	private final CategoryPath CAT = new CategoryPath("/win16");
	private final Map<String, DataType> types = new HashMap<>();

	private static final Set<String> DWORD_TYPES = new HashSet<>(Arrays.asList(
		"DWORD", "LONG", "COLORREF", "LRESULT", "LPARAM"));
	private static final Set<String> FAR_CODE_TYPES = new HashSet<>(Arrays.asList(
		"FARPROC", "WNDPROC", "ABORTPROC", "WNDENUMPROC", "TIMECALLBACK"));

	@Override
	public void run() throws Exception {
		dtm = currentProgram.getDataTypeManager();
		File dir = getSourceFile().getParentFile().getFile(false);
		String module = currentProgram.getName().replaceAll("\\.[^.]*$", "");

		defineStructs();
		int api = applyApiSignatures(new File(dir, "win16_api.txt"));
		int conv = applyConventions();
		// manual names of the game: folder passed as argument (default: the script's folder)
		String[] args = getScriptArgs();
		File namesDir = args.length > 0 ? new File(args[0]) : dir;
		int names = applyNames(new File(namesDir, "names_" + module + ".txt"));
		int committed = commitParams() + commitParams();
		println(String.format("UPSY ApplyWin16 %s: %d API, %d conventions, %d manual names, %d signatures propagated",
			module, api, conv, names, committed));
	}

	// ---------------------------------------------------------------- types

	private void defineStructs() {
		DataType i16 = ShortDataType.dataType;
		DataType u16 = WordDataType.dataType;
		DataType u32 = DWordDataType.dataType;
		struct("POINT", new Object[][] { { i16, "x" }, { i16, "y" } });
		struct("RECT", new Object[][] { { i16, "left" }, { i16, "top" }, { i16, "right" }, { i16, "bottom" } });
		struct("PALETTEENTRY", new Object[][] { { ByteDataType.dataType, "peRed" },
			{ ByteDataType.dataType, "peGreen" }, { ByteDataType.dataType, "peBlue" },
			{ ByteDataType.dataType, "peFlags" } });
		struct("MSG", new Object[][] { { u16, "hwnd" }, { u16, "message" }, { u16, "wParam" },
			{ u32, "lParam" }, { u32, "time" }, { types.get("POINT"), "pt" } });
		struct("WNDCLASS", new Object[][] { { u16, "style" }, { u32, "lpfnWndProc" },
			{ i16, "cbClsExtra" }, { i16, "cbWndExtra" }, { u16, "hInstance" }, { u16, "hIcon" },
			{ u16, "hCursor" }, { u16, "hbrBackground" }, { u32, "lpszMenuName" },
			{ u32, "lpszClassName" } });
	}

	private void struct(String name, Object[][] fields) {
		StructureDataType s = new StructureDataType(CAT, name, 0, dtm);
		for (Object[] f : fields) {
			s.add((DataType) f[0], (String) f[1], null);
		}
		s.setPackingEnabled(false);
		types.put(name, dtm.addDataType(s, DataTypeConflictHandler.REPLACE_HANDLER));
	}

	private DataType farPtr(DataType base) {
		return dtm.getPointer(base, 4);
	}

	private DataType typedef(String name, DataType base) {
		return types.computeIfAbsent(name, n -> dtm.addDataType(
			new TypedefDataType(CAT, n, base, dtm), DataTypeConflictHandler.REPLACE_HANDLER));
	}

	private DataType resolveType(String t) {
		t = t.replace("const ", "").trim();
		if (t.endsWith("FAR*") || t.endsWith("*")) {
			String base = t.replaceAll("\\s*(FAR)?\\*$", "").trim();
			return farPtr(resolveType(base));
		}
		switch (t) {
			case "void": return VoidDataType.dataType;
			case "int": return ShortDataType.dataType;
			case "char": return CharDataType.dataType;
			case "LPSTR": case "LPCSTR": case "HPSTR": return typedef(t, farPtr(CharDataType.dataType));
			case "LPVOID": return typedef(t, farPtr(VoidDataType.dataType));
			case "NPVOID": return typedef(t, dtm.getPointer(VoidDataType.dataType, 2));
		}
		if (FAR_CODE_TYPES.contains(t)) {
			return typedef(t, farPtr(VoidDataType.dataType));
		}
		if (DWORD_TYPES.contains(t)) {
			return typedef(t, t.equals("LONG") || t.equals("LRESULT") || t.equals("LPARAM")
				? LongDataType.dataType : DWordDataType.dataType);
		}
		if (types.containsKey(t) && types.get(t) instanceof Structure) {
			return types.get(t);
		}
		if (t.matches("[A-Z]+[A-Z0-9]*") && (t.startsWith("H") || t.equals("UINT") ||
			t.equals("WORD") || t.equals("BOOL") || t.equals("ATOM") || t.equals("WPARAM"))) {
			return typedef(t, t.equals("BOOL") ? ShortDataType.dataType : WordDataType.dataType);
		}
		// undefined structure: opaque placeholder
		return types.computeIfAbsent(t, n -> dtm.addDataType(new StructureDataType(CAT, n, 0, dtm),
			DataTypeConflictHandler.KEEP_HANDLER));
	}

	// ---------------------------------------------------------------- API

	private int applyApiSignatures(File f) throws Exception {
		Map<String, String[]> protos = new HashMap<>();
		for (String line : Files.readAllLines(f.toPath())) {
			if (line.isBlank() || line.startsWith("#")) {
				continue;
			}
			String[] p = line.split("\\|", -1);
			protos.put(p[0] + "::" + p[1].split("=")[0], p);
		}

		int n = 0;
		ExternalManager em = currentProgram.getExternalManager();
		for (String lib : em.getExternalLibraryNames()) {
			ExternalLocationIterator it = em.getExternalLocations(lib);
			while (it.hasNext()) {
				ExternalLocation loc = it.next();
				String[] p = protos.get(lib + "::" + loc.getLabel());
				if (p == null) {
					continue;
				}
				Function fn = loc.getFunction();
				if (fn == null) {
					fn = loc.createFunction();
				}
				if (p[1].contains("=")) {
					fn.setName(p[1].split("=")[1], SourceType.USER_DEFINED);
				}
				applySignature(fn, p[2], p[3], p.length > 4 && p[4].trim().equals("cdecl"));
				n++;
			}
		}
		return n;
	}

	// FAR signature with custom storage: pascal (callee pop, reverse order on the stack) or cdecl.
	private void applySignature(Function fn, String ret, String paramList, boolean cdecl)
			throws Exception {
		Register ax = currentProgram.getRegister("AX");
		Register dx = currentProgram.getRegister("DX");

		List<String[]> params = new ArrayList<>();
		for (String a : paramList.split(",")) {
			a = a.trim();
			if (a.isEmpty()) {
				continue;
			}
			int sp = a.lastIndexOf(' ');
			params.add(new String[] { a.substring(0, sp).trim(), a.substring(sp + 1) });
		}

		List<ParameterImpl> vars = new ArrayList<>();
		int[] sizes = new int[params.size()];
		DataType[] dts = new DataType[params.size()];
		int total = 0;
		for (int i = 0; i < params.size(); i++) {
			dts[i] = resolveType(params.get(i)[0]);
			sizes[i] = Math.max(2, dts[i].getLength());
			total += sizes[i];
		}
		// pascal: first parameter at the highest offset; cdecl: at the lowest
		int off = 4;
		int[] offs = new int[params.size()];
		if (cdecl) {
			for (int i = 0; i < params.size(); i++) { offs[i] = off; off += sizes[i]; }
		}
		else {
			for (int i = params.size() - 1; i >= 0; i--) { offs[i] = off; off += sizes[i]; }
		}
		for (int i = 0; i < params.size(); i++) {
			vars.add(new ParameterImpl(params.get(i)[1], dts[i],
				new VariableStorage(currentProgram, offs[i], sizes[i]), currentProgram));
		}

		DataType rt = resolveType(ret);
		VariableStorage rs = rt.getLength() <= 0 ? VariableStorage.VOID_STORAGE
			: rt.getLength() <= 2 ? new VariableStorage(currentProgram, ax)
			: new VariableStorage(currentProgram, dx, ax);
		fn.updateFunction(cdecl ? "__cdecl16far" : "__stdcall16far",
			new ReturnParameterImpl(rt, rs, currentProgram), vars,
			FunctionUpdateType.CUSTOM_STORAGE, true, SourceType.USER_DEFINED);
		fn.setVarArgs(cdecl && fn.isExternal());
		fn.setStackPurgeSize(cdecl ? 0 : total);
	}

	// ---------------------------------------------------------------- conventions

	private int applyConventions() throws Exception {
		int n = 0;
		for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
			if (f.isThunk() || f.hasCustomVariableStorage()) {
				continue;
			}
			boolean far = false, near = false;
			int purge = -1;
			boolean mixed = false;
			for (Instruction ins : currentProgram.getListing().getInstructions(f.getBody(), true)) {
				String m = ins.getMnemonicString();
				if (!m.equals("RETF") && !m.equals("RET")) {
					continue;
				}
				far |= m.equals("RETF");
				near |= m.equals("RET");
				Scalar s = ins.getNumOperands() > 0 ? ins.getScalar(0) : null;
				int v = s == null ? 0 : (int) s.getUnsignedValue();
				if (purge >= 0 && purge != v) {
					mixed = true;
				}
				purge = v;
			}
			if (purge < 0 || mixed || (far && near)) {
				continue;
			}
			String cc = (purge > 0 ? "__stdcall16" : "__cdecl16") + (far ? "far" : "near");
			f.setCallingConvention(cc);
			f.setStackPurgeSize(purge);
			n++;
		}
		return n;
	}

	// ---------------------------------------------------------------- manual names

	private int applyNames(File f) throws Exception {
		if (!f.exists()) {
			return 0;
		}
		int n = 0;
		for (String line : Files.readAllLines(f.toPath())) {
			line = line.trim();
			if (line.isEmpty() || line.startsWith("#")) {
				continue;
			}
			// address|name|return|parameters|pascal/cdecl|noreturn  (fields from the 3rd on are optional)
			String[] p = line.split("\\|", -1);
			Address a = currentProgram.getAddressFactory().getAddress(p[0].trim());
			Function fn = a == null ? null : getFunctionAt(a);
			if (fn == null) {
				printerr("names: no function at " + p[0]);
				continue;
			}
			fn.setName(p[1].trim(), SourceType.USER_DEFINED);
			if (p.length > 3 && !p[2].isBlank()) {
				applySignature(fn, p[2].trim(), p[3], p.length > 4 && p[4].trim().equals("cdecl"));
			}
			if (p.length > 5 && p[5].trim().equals("noreturn")) {
				fn.setNoReturn(true);
			}
			n++;
		}
		return n;
	}

	// ---------------------------------------------------------------- decompiler -> DB

	private int commitParams() throws Exception {
		DecompInterface ifc = new DecompInterface();
		ifc.setOptions(new DecompileOptions());
		ifc.openProgram(currentProgram);
		int n = 0;
		try {
			for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
				if (monitor.isCancelled()) {
					break;
				}
				if (f.isThunk() || f.getSignatureSource() == SourceType.USER_DEFINED) {
					continue;
				}
				DecompileResults r = ifc.decompileFunction(f, 60, monitor);
				HighFunction hf = r == null ? null : r.getHighFunction();
				if (hf == null) {
					continue;
				}
				try {
					HighFunctionDBUtil.commitParamsToDatabase(hf, true, ReturnCommitOption.COMMIT,
						SourceType.ANALYSIS);
					n++;
				}
				catch (Exception e) {
					// e.g. parameters in "unique" storage that cannot be represented in the DB
					printerr("commit skipped " + f.getEntryPoint() + " " + f.getName() + ": " +
						e.getMessage());
				}
			}
		}
		finally {
			ifc.dispose();
		}
		return n;
	}
}
