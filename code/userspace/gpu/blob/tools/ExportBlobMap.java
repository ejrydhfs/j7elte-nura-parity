// Ghidra headless post-script: inventory export for libGLES_mali.so (r7p0).
// Writes TSV files to the directory given as script arg 0.
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Data;
import ghidra.program.model.listing.DataIterator;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionIterator;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.InstructionIterator;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.ReferenceIterator;
import ghidra.program.model.symbol.Symbol;
import ghidra.program.model.symbol.SymbolIterator;
import ghidra.program.model.symbol.SymbolTable;
import java.io.File;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

public class ExportBlobMap extends GhidraScript {

    String[] IMPORT_NAMES = {
        "ioctl", "open", "openat", "close", "mmap", "mmap64", "munmap", "poll", "read", "write",
        "ion_open", "ion_alloc_fd", "ion_close", "hw_get_module", "__android_log_print",
        "__android_log_write", "atrace_begin_body", "atrace_setup", "get_process_name",
        "dlopen", "dlsym", "dlclose", "fork", "execv", "execve", "prctl", "getpid",
        "_ZN7android6RegionC1Ev", "_ZN7android6RegionD1Ev", "_ZN7android6Region5clearEv",
        "_ZN7android6Region3setEii", "_ZN7android6Region12subtractSelfERKNS_4RectE",
        "_ZN7android6Region6orSelfERKNS_4RectE", "_ZNK7android6Region8getArrayEPj"
    };

    String[] STR_KEYS = {
        "/dev/mali", "not of a compatible version", "DDK was built", "mali_base_kbase.c",
        "gralloc", "hwcomposer", "binder", "GLSL", "glsl", "compiler", "Midgard",
        "kbase", "shader", "mali_egl", "EGL_ANDROID", "eglSwapBuffers", "native_window",
        "ANativeWindow", "ion_", "job", "atom", "surface", "batch", "command"
    };

    PrintWriter funcs, thunks, ext, strs, imms, groups;

    static String addr(long a) {
        return String.format("0x%08x", a);
    }

    @Override
    public void run() throws Exception {
        File outDir = new File(getScriptArgs()[0]);
        outDir.mkdirs();
        funcs = new PrintWriter(new File(outDir, "functions.tsv"));
        thunks = new PrintWriter(new File(outDir, "thunks.tsv"));
        ext = new PrintWriter(new File(outDir, "external_xrefs.tsv"));
        strs = new PrintWriter(new File(outDir, "string_xrefs.tsv"));
        imms = new PrintWriter(new File(outDir, "imm_scan.tsv"));
        groups = new PrintWriter(new File(outDir, "entry_groups.tsv"));

        funcs.println("address\tname\tsize\tis_thunk\tis_external");
        thunks.println("address\tname\tthunked_symbol");
        ext.println("import\timport_addr\tcall_site\tcaller_func\tcaller_addr");
        strs.println("string_addr\tstring\tsite\tfunc\tfunc_addr");
        imms.println("insn_addr\tvalue\tfunc\tfunc_addr\tinsn");
        groups.println("entry\tentry_addr\tsize\tcallee\tcallee_addr");

        // 1. functions
        Set<Long> seen = new HashSet<>();
        FunctionIterator fit = currentProgram.getFunctionManager().getFunctions(true);
        while (fit.hasNext() && !monitor.isCancelled()) {
            Function f = fit.next();
            String name = f.getName();
            funcs.println(addr(f.getEntryPoint().getOffset()) + "\t" + name + "\t" + f.getBody().getNumAddresses()
                    + "\t" + f.isThunk() + "\t" + f.isExternal());
            if (f.isThunk()) {
                Function tf = f.getThunkedFunction(true);
                thunks.println(addr(f.getEntryPoint().getOffset()) + "\t" + name + "\t"
                        + (tf == null ? "" : tf.getName()));
            }
            seen.add(f.getEntryPoint().getOffset());
        }
        funcs.flush();

        // 2. external / imported symbol references
        SymbolTable st = currentProgram.getSymbolTable();
        Set<String> want = new HashSet<>(Arrays.asList(IMPORT_NAMES));
        for (Symbol s : iterable(st.getAllSymbols(true))) {
            if (!want.contains(s.getName())) continue;
            Address a = s.getAddress();
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(a);
            while (refs.hasNext()) {
                Reference r = refs.next();
                Address from = r.getFromAddress();
                Function cf = getFunctionContaining(from);
                if (cf != null && cf.getEntryPoint().equals(a)) continue; // the thunk itself
                ext.println(s.getName() + "\t" + addr(a.getOffset()) + "\t" + addr(from.getOffset()) + "\t"
                        + (cf == null ? "?" : cf.getName()) + "\t"
                        + (cf == null ? "?" : addr(cf.getEntryPoint().getOffset())));
            }
        }
        ext.flush();

        // 3. string references
        List<String> keys = Arrays.asList(STR_KEYS);
        DataIterator dit = currentProgram.getListing().getDefinedData(true);
        while (dit.hasNext() && !monitor.isCancelled()) {
            Data d = dit.next();
            if (d == null || !d.hasStringValue()) continue;
            Object v = d.getValue();
            if (!(v instanceof String)) continue;
            String sv = (String) v;
            if (sv.length() > 160) continue;
            String low = sv.toLowerCase(Locale.ROOT);
            boolean hit = false;
            for (String k : keys) {
                if (low.contains(k.toLowerCase(Locale.ROOT))) { hit = true; break; }
            }
            if (!hit) continue;
            ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(d.getAddress());
            while (refs.hasNext()) {
                Reference r = refs.next();
                Address from = r.getFromAddress();
                Function cf = getFunctionContaining(from);
                strs.println(addr(d.getAddress().getOffset()) + "\t"
                        + sv.replace("\t", "\\t").replace("\n", "\\n") + "\t" + addr(from.getOffset())
                        + "\t" + (cf == null ? "?" : cf.getName()) + "\t"
                        + (cf == null ? "?" : addr(cf.getEntryPoint().getOffset())));
            }
        }
        strs.flush();

        // 4. kbase UAPI id immediates (512..569) and 0x200..0x23f
        InstructionIterator iit = currentProgram.getListing().getInstructions(true);
        while (iit.hasNext() && !monitor.isCancelled()) {
            Instruction ins = iit.next();
            for (int op = 0; op < ins.getNumOperands(); op++) {
                Scalar sc = ins.getScalar(op);
                if (sc == null) continue;
                long val = sc.getUnsignedValue();
                if ((val >= 512 && val <= 569) || (val >= 0x200 && val <= 0x240)) {
                    Function cf = getFunctionContaining(ins.getAddress());
                    imms.println(addr(ins.getAddress().getOffset()) + "\t" + val + "\t"
                            + (cf == null ? "?" : cf.getName()) + "\t"
                            + (cf == null ? "?" : addr(cf.getEntryPoint().getOffset())) + "\t"
                            + ins.toString().replace("\t", " "));
                }
            }
        }
        imms.flush();

        // 5. exported GL/EGL/CL entry points and direct callees
        fit = currentProgram.getFunctionManager().getFunctions(true);
        while (fit.hasNext() && !monitor.isCancelled()) {
            Function f = fit.next();
            String n = f.getName();
            if (!(n.matches("^(egl|gl|cl)[A-Z].*") || n.startsWith("mali_egl"))) continue;
            Set<Function> callees = f.getCalledFunctions(monitor);
            int i = 0;
            for (Function c : callees) {
                if (i++ >= 30) break;
                groups.println(n + "\t" + addr(f.getEntryPoint().getOffset()) + "\t"
                        + f.getBody().getNumAddresses() + "\t" + c.getName() + "\t"
                        + addr(c.getEntryPoint().getOffset()));
            }
            if (callees.isEmpty()) {
                groups.println(n + "\t" + addr(f.getEntryPoint().getOffset()) + "\t"
                        + f.getBody().getNumAddresses() + "\t-\t-");
            }
        }
        groups.flush();

        funcs.close(); thunks.close(); ext.close(); strs.close(); imms.close(); groups.close();
        println("EXPORT DONE functions=" + seen.size());
    }

    private static <T> Iterable<T> iterable(java.util.Iterator<T> it) {
        return () -> it;
    }
}
