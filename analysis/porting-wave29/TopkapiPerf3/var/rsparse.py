# rsparse.py SRC SHIFT: TIMING ONLY: opaque surfaces trace glossy reflection rays on one 2^SHIFT tile of each 2x2 per frame
# (untraced: reflection 0, no history reuse yet).
import sys
src,shift=sys.argv[1],int(sys.argv[2])
p=src+'/rt_lighting.glsl'; s=open(p).read()
s=s.replace("struct RtLight {","bool rtReflect = true;\nstruct RtLight {",1)
s=s.replace("for (int sampleIndex = 0; indirect && sampleIndex < samples; ++sampleIndex) {","for (int sampleIndex = 0; indirect && rtReflect && sampleIndex < samples; ++sampleIndex) {",1)
open(p,'w').write(s)
p=src+'/lighting.glsl'; s=open(p).read()
old="RtLight light=rtLighting("
assert old in s
s=s.replace(old,f"if(draw.extra.w<.75){{uvec2 q=(uvec2(gl_FragCoord.xy)>>{shift}u)&1u;rtReflect=uint[4](0u,2u,3u,1u)[q.y*2u+q.x]==(uint(frame.viewport.z)&3u);}}\n"+old,1)
open(p,'w').write(s)
