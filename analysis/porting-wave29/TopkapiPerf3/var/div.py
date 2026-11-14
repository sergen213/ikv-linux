# div.py SRC: untraced blended alpha-tested fragments divide the reused (coverage-scaled) history by their own coverage.
import sys
src=sys.argv[1]; p=src+'/lighting.glsl'; s=open(p).read()
old="if(sparseHistory){light.diffuse=sparseDiffuse;light.reflection=sparseReflection;}"
assert old in s
s=s.replace(old,"if(sparseHistory){float hc=draw.extra.w>.75?max(original.a,draw.flags.w):1.;light.diffuse=sparseDiffuse/hc;light.reflection=sparseReflection/hc;}",1)
old="if(sparseHistory&&!fogged)diffuse=sparseDiffuse;"
assert old in s
s=s.replace(old,"if(sparseHistory&&!fogged)diffuse=sparseDiffuse/(draw.extra.w>.75?max(original.a,draw.flags.w):1.);",1)
open(p,'w').write(s)
