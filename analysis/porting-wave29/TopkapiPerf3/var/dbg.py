# dbg.py SRC: DEBUG: paint pixels magenta where the sparse pattern skipped but no history forced a trace (foliage: magenta,
# glossy: cyan).
import sys
src=sys.argv[1]; p=src+'/lighting.glsl'; s=open(p).read()
old="if(!indirect)indirect=!(sparseHistory=rtSparseHistory(sparseDiffuse,sparseReflection));"
assert old in s
s=s.replace(old,"bool dbgFallback=false,dbgGlossy=false;\nif(!indirect){indirect=!(sparseHistory=rtSparseHistory(sparseDiffuse,sparseReflection));dbgFallback=indirect;}",1)
old="glossy=!rtSparseHistory(unused,glossyHistory);"
s=s.replace(old,"{glossy=!rtSparseHistory(unused,glossyHistory);dbgGlossy=glossy;}",1)
old="outputColor=vec4(result,original.a);"
assert old in s
s=s.replace(old,old+"\n#ifdef IKV_RT_GUIDED\nif(dbgFallback)outputColor=vec4(8,0,8,original.a);if(dbgGlossy)outputColor=vec4(0,8,8,original.a);\n#endif",1)
open(p,'w').write(s)
