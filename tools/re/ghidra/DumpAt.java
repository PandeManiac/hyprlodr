// Decompile the functions containing the given addresses.
//
// Used to inspect the code sites that reference the title/lobby asset names found
// by tools/re/analyze_pe.py. Output is decompiled game code: it is printed to the
// console for local analysis and must not be committed.
//
// Usage:
//   analyzeHeadless <project> <name> -import hyperdemon.exe \
//     -scriptPath tools/re/ghidra -postScript DumpAt.java 0x1401208a2 ...
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

public class DumpAt extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.toggleCCode(true);
        decompiler.toggleSyntaxTree(true);
        decompiler.openProgram(currentProgram);

        String[] args = getScriptArgs();
        for (String raw : args) {
            Address address = toAddr(Long.parseLong(raw.replace("0x", ""), 16));
            Function function = getFunctionContaining(address);
            if (function == null) {
                println("=== NO FUNCTION at " + raw);
                continue;
            }
            println("=== FUNCTION " + function.getName() + " entry=" + function.getEntryPoint()
                    + " (asked " + raw + ")");
            DecompileResults results = decompiler.decompileFunction(function, 90, monitor);
            if (results != null && results.decompileCompleted()) {
                println(results.getDecompiledFunction().getC());
            } else {
                println("=== DECOMPILE FAILED at " + raw);
            }
        }
        decompiler.dispose();
    }
}
