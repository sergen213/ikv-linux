// Static analysis only; operate on an isolated copy of the saved project.
// @category IKV
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.io.*;
import java.util.*;

public class AudioEvidence extends GhidraScript {
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Path root = Paths.get(args[0]); Files.createDirectories(root);
        DecompInterface dc = new DecompInterface(); dc.openProgram(currentProgram);
        for (String address : new String[]{"0049fbd0","0056e033","0056dc5d"}) {
            Function f=currentProgram.getFunctionManager().getFunctionAt(toAddr(address));
            if(f!=null) f.setNoReturn(false);
        }
        LinkedHashSet<Function> selected = new LinkedHashSet<>();
        if (args.length == 1) {
            FunctionIterator fs = currentProgram.getFunctionManager().getFunctions(toAddr("00453800"), true);
            while (fs.hasNext()) { Function f=fs.next(); if(f.getEntryPoint().compareTo(toAddr("00459b80"))>=0) break; selected.add(f); }
        } else for(int i=1;i<args.length;i++) {
            // "@ADDR" selects the function containing a referencing instruction.
            boolean containing=args[i].startsWith("@"); String a=containing?args[i].substring(1):args[i];
            Function f=containing?currentProgram.getFunctionManager().getFunctionContaining(toAddr(a))
                                 :currentProgram.getFunctionManager().getFunctionAt(toAddr(a));
            if(f==null&&!containing) { disassemble(toAddr(a)); f=createFunction(toAddr(a),null); }
            if(f==null) throw new IllegalArgumentException("Missing function " + args[i]); selected.add(f);
        }
        try(PrintWriter out=new PrintWriter(Files.newBufferedWriter(root.resolve("trace.tsv"),StandardCharsets.UTF_8))) {
            for(Function f:selected) {
                out.println("FUNCTION\t"+f.getEntryPoint()+"\t"+f.getSignature());
                ReferenceIterator refs=currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
                while(refs.hasNext()) out.println("XREF\t"+f.getEntryPoint()+"\t"+refs.next().getFromAddress());
                InstructionIterator ins=currentProgram.getListing().getInstructions(f.getBody(),true);
                while(ins.hasNext()) { Instruction in=ins.next(); out.println(in.getAddress()+"\t"+in); }
                DecompileResults result=dc.decompileFunction(f,45,monitor);
                if(result.decompileCompleted()) Files.writeString(root.resolve(f.getEntryPoint()+".c"),result.getDecompiledFunction().getC(),StandardCharsets.UTF_8);
                else out.println("ERROR\t"+result.getErrorMessage());
            }
        } finally { dc.dispose(); }
        Files.writeString(root.resolve("program.txt"),"sha256="+currentProgram.getExecutableSHA256()+"\nimage_base="+currentProgram.getImageBase()+"\nselected="+selected.size()+"\n",StandardCharsets.UTF_8);
    }
}
