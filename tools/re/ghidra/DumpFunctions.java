/*
 * Ghidra headless post-script: describe and decompile the function containing
 * each RVA passed as a script argument (hex, no 0x prefix).
 *
 * Used to follow the anchors produced by tools/re/analyze_pe.py. Output is
 * analysis material for a human, not something to commit: it contains
 * decompiled game code.
 *
 *   /opt/ghidra/support/analyzeHeadless <proj-dir> <proj> \
 *       -import <hyperdemon.exe> -overwrite \
 *       -scriptPath tools/re/ghidra \
 *       -postScript DumpFunctions.java 21257e 213ce6
 */
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

public class DumpFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] targets = getScriptArgs();
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        for (String target : targets) {
            long rva = Long.parseLong(target, 16);
            Address address = currentProgram.getImageBase().add(rva);
            println("");
            println("=== TARGET rva=0x" + Long.toHexString(rva) + " va=" + address + " ===");

            Function function = getFunctionContaining(address);
            if (function == null) {
                println("no function contains this address");
                continue;
            }
            println("function : " + function.getName() + " @ " + function.getEntryPoint()
                    + " (" + function.getBody().getNumAddresses() + " bytes)");

            for (Function callee : function.getCalledFunctions(monitor)) {
                println("  callee : " + callee.getName() + " @ " + callee.getEntryPoint());
            }
            for (Reference reference : getReferencesTo(address)) {
                println("  ref-to : " + reference);
            }

            DecompileResults results = decompiler.decompileFunction(function, 180, monitor);
            if (results != null && results.decompileCompleted()) {
                println("--- decompiled ---");
                println(results.getDecompiledFunction().getC());
                println("--- end ---");
            } else {
                println("decompilation failed");
            }
        }
        decompiler.dispose();
    }
}
