# sunmip.py SRC LOD [SECLOD]: foliage (blended alpha-tested) Sun rays alpha-test at mip LOD; secondary rays at SECLOD (3).
import sys, subprocess, os
src,lod=sys.argv[1],sys.argv[2]; sec=sys.argv[3] if len(sys.argv)>3 else '3'
subprocess.run([sys.executable, os.path.join(os.path.dirname(__file__),'alpha.py'), src, 'mip', sec], check=True)
p=src+'/rt_lighting.glsl'; s=open(p).read()
s=s.replace("struct RtLight {","float rtSunLod = 3.0;\nstruct RtLight {",1)
old="        visibility = rtSunVisibility(position, facet, sunDir, 0u);"
assert old in s
s=s.replace(old,"        { rayQueryEXT q; rtTraceA(q, position, facet, sunDir, gl_RayFlagsTerminateOnFirstHitEXT, 0xffu, rtSunLod, 79.0 / 255.0);\n          visibility = rayQueryGetIntersectionTypeEXT(q, true) == gl_RayQueryCommittedIntersectionNoneEXT ? 1.0 : 0.0; }",1)
open(p,'w').write(s)
p=src+'/lighting.glsl'; s=open(p).read()
old="RtLight light=rtLighting("
s=s.replace(old,f"if(draw.extra.w>.75&&draw.extra.w<2.)rtSunLod={lod}.;\n"+old,1)
open(p,'w').write(s)
