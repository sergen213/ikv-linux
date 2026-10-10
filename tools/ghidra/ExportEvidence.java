// Static evidence only. Run after normal Ghidra auto-analysis; output is local, ignored.
// @category IKV
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.util.*;
import java.util.regex.Pattern;

public class ExportEvidence extends GhidraScript {
    private static final Pattern API = Pattern.compile("(?i).*(Ordinal_[0-9]+|WSA|socket|connect|send|recv|select|hostby|addrinfo|inet_|Internet|Http|Crypt|SSL|Direct3D|D3DX|DirectDraw|DirectSound|DirectInput|OpenGL|CreateFile|ReadFile|fopen|fread|LoadLibrary|GetProcAddress).*" );
    private static final Pattern TEXT = Pattern.compile("(?i).*(SKYF|ADF|lightmap|objects|zone|audio|https?://|ftp://|[a-z0-9-]+\\.(com|net|org|com\\.tr)|server|host|port|socket|connect|packet|login|auth|patch|update|direct3d|d3dx|shader|texture|mesh|\\.(pak|zip|dat|raw|dds|tga|png|ogg|wav|ini|cfg|xml)).*");
    private String cell(Object v) { return String.valueOf(v).replace("\t", " ").replace("\r", "\\r").replace("\n", "\\n"); }
    private void row(PrintWriter out, Object... values) { for (int i=0; i<values.length; i++) { if(i>0) out.print('\t'); out.print(cell(values[i])); } out.println(); }
    private String offset(Address a) { try { return Long.toHexString(currentProgram.getMemory().getAddressSourceInfo(a).getFileOffset()); } catch(Exception e) { return "unknown"; } }
    public void run() throws Exception {
        String[] args = getScriptArgs();
        if(args.length < 1) throw new IllegalArgumentException("Pass output directory, optionally followed by function addresses");
        Path root = Paths.get(args[0]); Files.createDirectories(root.resolve("decompile"));
        LinkedHashSet<Function> seeds = new LinkedHashSet<>();
        for(int i=1; i<args.length; i++) {
            Function f=currentProgram.getFunctionManager().getFunctionAt(toAddr(args[i]));
            if(f==null) { disassemble(toAddr(args[i])); f=createFunction(toAddr(args[i]),null); }
            if(f==null) throw new IllegalArgumentException("No function at "+args[i]);
            seeds.add(f);
        }
        try(PrintWriter out = new PrintWriter(Files.newBufferedWriter(root.resolve("imports.tsv"), StandardCharsets.UTF_8))) {
            row(out,"library","name","address","xref","caller","signature");
            SymbolIterator syms = currentProgram.getSymbolTable().getExternalSymbols();
            while(syms.hasNext() && !monitor.isCancelled()) {
                Symbol sym = syms.next(); Address a = sym.getAddress(); boolean relevant = API.matcher(sym.getName()).matches();
                ReferenceIterator refs = currentProgram.getReferenceManager().getReferencesTo(a); boolean any=false;
                while(refs.hasNext()) { Reference ref=refs.next(); Function f=currentProgram.getFunctionManager().getFunctionContaining(ref.getFromAddress());
                    row(out,sym.getParentNamespace().getName(true),sym.getName(),a,ref.getFromAddress(),f==null?"":f.getEntryPoint(),f==null?"":f.getSignature()); any=true;
                    if(relevant && f!=null) seeds.add(f);
                }
                if(!any) row(out,sym.getParentNamespace().getName(true),sym.getName(),a,"","","");
            }
            // PE import references often target IAT labels rather than external symbols.
            SymbolIterator all = currentProgram.getSymbolTable().getAllSymbols(true);
            while(all.hasNext()) { Symbol sym=all.next(); if(!API.matcher(sym.getName()).matches() || sym.isExternal()) continue;
                ReferenceIterator refs=currentProgram.getReferenceManager().getReferencesTo(sym.getAddress());
                while(refs.hasNext()) { Reference ref=refs.next(); Function f=currentProgram.getFunctionManager().getFunctionContaining(ref.getFromAddress());
                    row(out,sym.getParentNamespace().getName(true),sym.getName(),sym.getAddress(),ref.getFromAddress(),f==null?"":f.getEntryPoint(),f==null?"":f.getSignature());
                    if(f!=null) seeds.add(f);
                }
            }
        }
        try(PrintWriter out=new PrintWriter(Files.newBufferedWriter(root.resolve("strings.tsv"),StandardCharsets.UTF_8))) {
            row(out,"address","file_offset_hex","value","xref","caller");
            DataIterator it=currentProgram.getListing().getDefinedData(true);
            while(it.hasNext()) { Data d=it.next(); if(!d.hasStringValue()) continue; String value=String.valueOf(d.getValue()); if(!TEXT.matcher(value).matches()) continue;
                ReferenceIterator refs=currentProgram.getReferenceManager().getReferencesTo(d.getAddress()); boolean any=false;
                while(refs.hasNext()) { Reference ref=refs.next(); Function f=currentProgram.getFunctionManager().getFunctionContaining(ref.getFromAddress());
                    row(out,d.getAddress(),offset(d.getAddress()),value,ref.getFromAddress(),f==null?"":f.getEntryPoint()); any=true;
                    if(f!=null) seeds.add(f);
                }
                if(!any) row(out,d.getAddress(),offset(d.getAddress()),value,"","");
            }
        }
        // One caller layer; bounded to avoid wholesale decompilation of proprietary code.
        LinkedHashSet<Function> selected=new LinkedHashSet<>(seeds);
        for(Function f:seeds) { ReferenceIterator refs=currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
            while(refs.hasNext()) { Function caller=currentProgram.getFunctionManager().getFunctionContaining(refs.next().getFromAddress()); if(caller!=null) selected.add(caller); }
        }
        DecompInterface decompiler=new DecompInterface(); decompiler.openProgram(currentProgram);
        try(PrintWriter out=new PrintWriter(Files.newBufferedWriter(root.resolve("functions.tsv"),StandardCharsets.UTF_8))) {
            row(out,"address","name","signature","size","seed","callers","callees","decompile_status"); int n=0;
            for(Function f:selected) { if(monitor.isCancelled() || n++>=300) break;
                List<String> callers=new ArrayList<>(); ReferenceIterator refs=currentProgram.getReferenceManager().getReferencesTo(f.getEntryPoint());
                while(refs.hasNext()) { Reference ref=refs.next(); Function c=currentProgram.getFunctionManager().getFunctionContaining(ref.getFromAddress()); if(c!=null) callers.add(c.getEntryPoint()+"@"+ref.getFromAddress()); }
                List<String> callees=new ArrayList<>(); for(Function c:f.getCalledFunctions(monitor)) callees.add(c.getName()+"@"+c.getEntryPoint());
                DecompileResults result=decompiler.decompileFunction(f,30,monitor);
                row(out,f.getEntryPoint(),f.getName(),f.getSignature(),f.getBody().getNumAddresses(),seeds.contains(f),String.join(",",callers),String.join(",",callees),result.decompileCompleted()?"ok":result.getErrorMessage());
                if(result.decompileCompleted()) Files.writeString(root.resolve("decompile").resolve(f.getEntryPoint()+".c"),result.getDecompiledFunction().getC(),StandardCharsets.UTF_8);
            }
        } finally { decompiler.dispose(); }
        Files.writeString(root.resolve("program.txt"),"name="+currentProgram.getName()+"\nformat="+currentProgram.getExecutableFormat()+"\nlanguage="+currentProgram.getLanguageID()+"\ncompiler="+currentProgram.getCompilerSpec().getCompilerSpecID()+"\nimage_base="+currentProgram.getImageBase()+"\nsha256="+currentProgram.getExecutableSHA256()+"\nselected="+selected.size()+"\nexport_limit=300\n",StandardCharsets.UTF_8);
    }
}
