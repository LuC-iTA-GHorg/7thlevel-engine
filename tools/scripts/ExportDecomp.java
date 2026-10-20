// Exports the decompiled C of every function of the current program.
// Usage (headless): -postScript ExportDecomp.java <output_folder>
// Produces <out>\<MODULE>\<MODULE>.c, functions.csv, imports.txt, strings.txt
// @category UPSY

import java.io.File;
import java.io.PrintWriter;

import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileOptions;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;

public class ExportDecomp extends GhidraScript {

	@Override
	public void run() throws Exception {
		String[] args = getScriptArgs();
		String module = currentProgram.getName().replaceAll("\\.[^.]*$", "");
		File outDir = new File(args.length > 0 ? args[0] : ".", module);
		outDir.mkdirs();

		DecompInterface ifc = new DecompInterface();
		ifc.setOptions(new DecompileOptions());
		ifc.toggleCCode(true);
		ifc.toggleSyntaxTree(true);
		ifc.setSimplificationStyle("decompile");
		if (!ifc.openProgram(currentProgram)) {
			printerr("Decompiler not started: " + ifc.getLastMessage());
			return;
		}

		int ok = 0, fail = 0;
		try (PrintWriter c = new PrintWriter(new File(outDir, module + ".c"), "UTF-8");
				PrintWriter csv = new PrintWriter(new File(outDir, "functions.csv"), "UTF-8")) {
			c.println("// " + currentProgram.getName() + " - decompiled with Ghidra " +
				ghidra.framework.Application.getApplicationVersion());
			c.println("// Language: " + currentProgram.getLanguageID());
			c.println();
			csv.println("address,name,size,calling_convention,status");

			FunctionIterator it = currentProgram.getFunctionManager().getFunctions(true);
			while (it.hasNext() && !monitor.isCancelled()) {
				Function f = it.next();
				if (f.isExternal() || f.isThunk()) {
					continue;
				}
				DecompileResults r = ifc.decompileFunction(f, 120, monitor);
				String status;
				c.println("// ---- " + f.getEntryPoint() + " " + f.getName() + " ----");
				if (r != null && r.decompileCompleted() && r.getDecompiledFunction() != null) {
					c.println(r.getDecompiledFunction().getC());
					status = "ok";
					ok++;
				}
				else {
					c.println("/* DECOMPILATION FAILED: " +
						(r == null ? "?" : r.getErrorMessage()) + " */\n");
					status = "fail";
					fail++;
				}
				csv.printf("%s,%s,%d,%s,%s%n", f.getEntryPoint(), f.getName(),
					f.getBody().getNumAddresses(), f.getCallingConventionName(), status);
			}
		}
		finally {
			ifc.dispose();
		}

		try (PrintWriter imp = new PrintWriter(new File(outDir, "imports.txt"), "UTF-8")) {
			SymbolIterator si = currentProgram.getSymbolTable().getExternalSymbols();
			while (si.hasNext()) {
				Symbol s = si.next();
				imp.println(s.getParentNamespace().getName() + "::" + s.getName());
			}
		}

		try (PrintWriter str = new PrintWriter(new File(outDir, "strings.txt"), "UTF-8")) {
			DataIterator di = currentProgram.getListing().getDefinedData(true);
			while (di.hasNext()) {
				Data d = di.next();
				if (d.hasStringValue()) {
					str.println(d.getAddress() + "\t" + d.getDefaultValueRepresentation());
				}
			}
		}

		println("UPSY export " + module + ": " + ok + " functions ok, " + fail + " failed -> " +
			outDir.getAbsolutePath());
	}
}
