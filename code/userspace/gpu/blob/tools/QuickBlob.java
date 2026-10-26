// Lightweight Ghidra headless export (import with -noanalysis):
// creates/disassembles exported DDK functions < 0x520000 and reports string xrefs.
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolType;
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.Iterator;
import java.util.List;

public class QuickBlob extends GhidraScript {
    @Override
    public void run() throws Exception {
        File out = new File(getScriptArgs()[0]);
        out.mkdirs();
        PrintWriter fw = new PrintWriter(new File(out, "quick_functions.tsv"));
        PrintWriter sw = new PrintWriter(new File(out, "quick_strings.tsv"));
        fw.println("addr\tname\tsize\tcreated");

        List<Symbol> funcs = new ArrayList<>();
        for (Symbol s : iterable(currentProgram.getSymbolTable().getAllSymbols(true))) {
            if (s.getSymbolType() != SymbolType.FUNCTION) continue;
            long o = s.getAddress().getOffset();
            if (o < 0x235000L || o >= 0x520000L) continue;
            funcs.add(s);
        }
        funcs.sort(Comparator.comparingLong(s -> s.getAddress().getOffset()));

        int created = 0;
        for (Symbol s : funcs) {
            Address a = s.getAddress();
            if (getFunctionAt(a) != null) continue;
            new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
            new CreateFunctionCmd(a).applyTo(currentProgram, monitor);
            if (getFunctionAt(a) != null) created++;
        }

        AddressSet base = new AddressSet(toAddr(0x264c00L), toAddr(0x266c00L));
        new DisassembleCommand(base, null, true).applyTo(currentProgram, monitor);

        for (Function f : iterable(currentProgram.getFunctionManager().getFunctions(true))) {
            long o = f.getEntryPoint().getOffset();
            if (o < 0x235000L || o >= 0x520000L) continue;
            fw.println(String.format("0x%08x\t%s\t%d\t", o, f.getName(), f.getBody().getNumAddresses()));
        }
        fw.close();

        long[] targets = {0x12d8900L, 0x12d89b4L, 0x12d8934L, 0x12da47cL, 0x132f288L,
                          0x132f328L, 0x132f2e8L, 0x1420328L, 0x142fdc0L, 0x142fe08L,
                          0x12dda38L, 0x12f4704L, 0x12f2e00L, 0x12e43fcL};
        String[] names = {"dev_mali", "mali_t720", "not_compatible", "cmar_assert",
                          "gles_src_internal", "gles_compile_shader", "gles_shader_binary",
                          "gles_shaders_get", "compiler_frontend_fail", "compiler_middle_fail",
                          "support_mali", "shader_binary_check", "glsl310", "langver"};
        sw.println("string\taddr\tfrom\tfunction");
        for (int i = 0; i < targets.length; i++) {
            Address ta = toAddr(targets[i]);
            ReferenceIterator ri = currentProgram.getReferenceManager().getReferencesTo(ta);
            while (ri.hasNext()) {
                Reference r = ri.next();
                Address from = r.getFromAddress();
                Function f = getFunctionContaining(from);
                sw.println(names[i] + "\t" + ta + "\t" + from + "\t"
                        + (f == null ? "?" : f.getName() + "@" + f.getEntryPoint()));
            }
        }
        sw.close();
        println("QUICK DONE exported=" + funcs.size() + " created=" + created);
    }

    private static <T> Iterable<T> iterable(Iterator<T> it) {
        return () -> it;
    }
}
