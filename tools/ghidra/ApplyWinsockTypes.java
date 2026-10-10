// Canonical WS2_32 names and x86 stdcall parameter footprints; scalar signedness,
// WORD returns and pointed-to structures are intentionally not fully modeled.
// These analysis annotations do not infer any game message layout.
// Ordinal source: https://raw.githubusercontent.com/wine-mirror/wine/master/dlls/ws2_32/ws2_32.spec
// @category IKV
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.*;
import java.util.*;

public class ApplyWinsockTypes extends GhidraScript {
    public void run() throws Exception {
        Map<String,String[]> types=new HashMap<>();
        types.put("Ordinal_3",new String[]{"closesocket","i"});
        types.put("Ordinal_4",new String[]{"connect","ipi"});
        types.put("Ordinal_7",new String[]{"getsockopt","iiipp"});
        types.put("Ordinal_9",new String[]{"htons","i"});
        types.put("Ordinal_16",new String[]{"recv","ipii"});
        types.put("Ordinal_19",new String[]{"send","ipii"});
        types.put("Ordinal_21",new String[]{"setsockopt","iiipi"});
        types.put("Ordinal_23",new String[]{"socket","iii"});
        types.put("Ordinal_52",new String[]{"gethostbyname","p"});
        types.put("Ordinal_111",new String[]{"WSAGetLastError",""});
        types.put("Ordinal_115",new String[]{"WSAStartup","ip"});
        types.put("Ordinal_116",new String[]{"WSACleanup",""});
        SymbolIterator symbols=currentProgram.getSymbolTable().getExternalSymbols();
        while(symbols.hasNext()) {
            Symbol symbol=symbols.next();
            if(!symbol.getParentNamespace().getName(true).toUpperCase().contains("WS2_32")) continue;
            String[] type=types.get(symbol.getName()); if(type==null) continue;
            Function f=currentProgram.getFunctionManager().getFunctionAt(symbol.getAddress());
            if(f==null) continue;
            DataType ret=type[0].equals("gethostbyname")?PointerDataType.dataType:IntegerDataType.dataType;
            List<Parameter> params=new ArrayList<>();
            for(int i=0;i<type[1].length();i++) params.add(new ParameterImpl("arg"+i,type[1].charAt(i)=='p'?PointerDataType.dataType:IntegerDataType.dataType,currentProgram));
            f.setName(type[0],SourceType.USER_DEFINED);
            f.setCallingConvention("__stdcall");
            f.setReturnType(ret,SourceType.USER_DEFINED);
            f.replaceParameters(params,Function.FunctionUpdateType.DYNAMIC_STORAGE_ALL_PARAMS,true,SourceType.USER_DEFINED);
            println(symbol.getAddress()+" "+type[0]+" "+f.getSignature());
        }
    }
}
