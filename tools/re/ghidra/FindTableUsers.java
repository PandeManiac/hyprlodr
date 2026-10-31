/*
 * Ghidra headless post-script: show who reads and writes a given address, and
 * decompile the containing functions.
 *
 * Used to find the routine that builds the cipher tables the asset loader
 * uses at run time. Arguments are virtual addresses in hex (no 0x prefix):
 *
 *   analyzeHeadless <proj-dir> <proj> -process hyperdemon.exe -noanalysis \
 *       -scriptPath tools/re/ghidra \
 *       -postScript FindTableUsers.java 140361710 140361810
 */
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;
import ghidra.program.model.symbol.Reference;

import java.util.LinkedHashSet;

public class FindTableUsers extends GhidraScript {
    @Override
    public void run() throws Exception {
        DecompInterface decompiler = new DecompInterface();
        decompiler.openProgram(currentProgram);

        for (String target : getScriptArgs()) {
            Address address = toAddr(Long.parseLong(target, 16));
            println("");
            println("=== ADDRESS " + address + " ===");

            Reference[] references = getReferencesTo(address);
            println("references: " + references.length);

            LinkedHashSet<Function> functions = new LinkedHashSet<>();
            for (Reference reference : references) {
                println("  " + reference.getReferenceType() + " from "
                        + reference.getFromAddress());
                Function function = getFunctionContaining(reference.getFromAddress());
                if (function != null) {
                    functions.add(function);
                }
            }

            for (Function function : functions) {
                println("function : " + function.getName() + " @ "
                        + function.getEntryPoint() + " ("
                        + function.getBody().getNumAddresses() + " bytes)");
                DecompileResults results =
                        decompiler.decompileFunction(function, 180, monitor);
                if (results != null && results.decompileCompleted()) {
                    println("--- decompiled ---");
                    println(results.getDecompiledFunction().getC());
                    println("--- end ---");
                }
            }
        }
        decompiler.dispose();
    }
}
