// Read-only static Direct3D call-site and global producer evidence; no PE execution.
// @category IKV
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.scalar.Scalar;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
import java.io.*;
import java.util.*;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class GraphicsEvidence extends GhidraScript {
    public void run() throws Exception {
        String[] args=getScriptArgs();
        if(args.length<1)throw new IllegalArgumentException("output dir [global or function addresses...]");
        Path root=Paths.get(args[0]);Files.createDirectories(root.resolve("decompile"));
        LinkedHashSet<Function> selected=new LinkedHashSet<>();
        if(args.length>1) {
            try(PrintWriter values=new PrintWriter(Files.newBufferedWriter(root.resolve("values.tsv"),StandardCharsets.UTF_8));
                PrintWriter strings=new PrintWriter(Files.newBufferedWriter(root.resolve("strings.tsv"),StandardCharsets.UTF_8))) {
                values.println("address\tbits_hex\tfloat32");
                strings.println("address\tutf16");
                for(int n=1;n<args.length;n++) {
                    if(currentProgram.getMemory().getBlock(toAddr(args[n])).isExecute())continue;
                    try {
                        byte[] bytes=new byte[64];currentProgram.getMemory().getBytes(toAddr(args[n]),bytes);
                        ByteBuffer buffer=ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN);
                        for(int k=0;k<16;k++){int bits=buffer.getInt();values.println(toAddr(args[n]).add(4*k)+"\t"+Integer.toHexString(bits)+"\t"+Float.intBitsToFloat(bits));}
                        StringBuilder text=new StringBuilder();
                        for(int k=0;k<256;k++) {
                            char c=(char)currentProgram.getMemory().getShort(toAddr(args[n]).add(2*k));
                            if(c==0)break;
                            text.append(c);
                        }
                        strings.println(args[n]+"\t"+text.toString().replace("\t","\\t").replace("\r","\\r").replace("\n","\\n"));
                    }catch(Exception e){values.println(args[n]+"\tuninitialized\t"+e.getMessage());}
                }
            }
        }
        try(PrintWriter out=new PrintWriter(Files.newBufferedWriter(root.resolve("calls.tsv"),StandardCharsets.UTF_8));
            PrintWriter assembly=new PrintWriter(Files.newBufferedWriter(root.resolve("instructions.tsv"),StandardCharsets.UTF_8))) {
            out.println("function\taddress\tinstruction\tcontext");
            if(args.length==1) {
                InstructionIterator instructions=currentProgram.getListing().getInstructions(true);
                while(instructions.hasNext()&&!monitor.isCancelled()) {
                    Instruction ins=instructions.next();if(!ins.getMnemonicString().equalsIgnoreCase("CALL"))continue;
                    boolean match=false;
                    for(int n=0;n<ins.getNumOperands();n++)for(Object operand:ins.getOpObjects(n))if(operand instanceof Scalar) {
                        long value=((Scalar)operand).getUnsignedValue();
                        if(value==0xe4||value==0x114||value==0x148||value==0x178||value==0x1b4)match=true;
                    }
                    if(!match)continue;
                    Function f=currentProgram.getFunctionManager().getFunctionContaining(ins.getAddress());
                    if(f!=null)selected.add(f);
                    Instruction cursor=ins;StringBuilder context=new StringBuilder();
                    for(int n=0;n<8&&cursor!=null;n++,cursor=cursor.getPrevious())context.insert(0,cursor.getAddress()+":"+cursor+" | ");
                    out.println((f==null?"":f.getEntryPoint())+"\t"+ins.getAddress()+"\t"+ins+"\t"+context);
                }
            } else {
                for(int n=1;n<args.length;n++) {
                    Function direct=currentProgram.getFunctionManager().getFunctionAt(toAddr(args[n]));
                    if(direct==null&&currentProgram.getMemory().getBlock(toAddr(args[n])).isExecute()) {
                        disassemble(toAddr(args[n]));direct=createFunction(toAddr(args[n]),null);
                    }
                    if(direct!=null) {
                        selected.add(direct);
                        assembly.println("FUNCTION "+args[n]);
                        InstructionIterator body=currentProgram.getListing().getInstructions(direct.getBody(),true);
                        while(body.hasNext()) {
                            Instruction ins=body.next();assembly.println(ins.getAddress()+"\t"+ins);
                        }
                    }
                    ReferenceIterator refs=currentProgram.getReferenceManager().getReferencesTo(toAddr(args[n]));
                    while(refs.hasNext()) {
                        Reference ref=refs.next();Function f=currentProgram.getFunctionManager().getFunctionContaining(ref.getFromAddress());
                        if(f!=null)selected.add(f);
                        out.println((f==null?"":f.getEntryPoint())+"\t"+ref.getFromAddress()+"\t"+ref.getReferenceType()+"\t"+args[n]);
                        Instruction center=currentProgram.getListing().getInstructionAt(ref.getFromAddress());
                        if(center!=null) {
                            assembly.println("TARGET "+args[n]+" XREF "+ref.getFromAddress());
                            Instruction first=center;
                            for(int k=0;k<(f==null?48:8)&&first.getPrevious()!=null;k++)first=first.getPrevious();
                            Instruction cursor=first;
                            for(int k=0;k<(f==null?72:20)&&cursor!=null;k++,cursor=cursor.getNext())
                                assembly.println(cursor.getAddress()+"\t"+cursor);
                        }
                    }
                }
            }
        }
        DecompInterface decompiler=new DecompInterface();decompiler.openProgram(currentProgram);
        try {
            for(Function f:selected) {
                if(monitor.isCancelled())break;
                DecompileResults result=decompiler.decompileFunction(f,30,monitor);
                if(result.decompileCompleted())Files.writeString(root.resolve("decompile").resolve(f.getEntryPoint()+".c"),result.getDecompiledFunction().getC(),StandardCharsets.UTF_8);
            }
        } finally {decompiler.dispose();}
        println("Exported "+selected.size()+" bounded graphics functions; no native image execution");
    }
}
