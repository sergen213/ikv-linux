# alpha.py SRC MODE ARG: secondary (GI/reflection) rays of opaque surfaces: MODE mip -> alpha test at mip ARG;
# MODE stoch -> stochastic coverage: confirm when mip-ARG alpha exceeds a per-ray hash (expected occlusion = coverage).
import sys
src,mode,arg=sys.argv[1],sys.argv[2],sys.argv[3]
p=src+'/rt_lighting.glsl'; s=open(p).read()
s=s.replace("bool rtAlphaOpaque(uint index, uint slot, vec2 barycentrics) {","bool rtAlphaOpaque(uint index, uint slot, vec2 barycentrics, float lod, float cut) {")
s=s.replace("return textureLod(rtAlphaTextures[nonuniformEXT(slot - 1u)], uv, 3.0).a > 79.0 / 255.0;","return textureLod(rtAlphaTextures[nonuniformEXT(slot - 1u)], uv, lod).a > cut;")
old="""#define rtTrace(query, position, facet, direction, flags, mask) {"""
s=s.replace(old,"""#define rtTrace(query, position, facet, direction, flags, mask) rtTraceA(query, position, facet, direction, flags, mask, 3.0, 79.0 / 255.0)
#define rtTraceA(query, position, facet, direction, flags, mask, lod, cut) {""")
s=s.replace("""                          rayQueryGetIntersectionBarycentricsEXT(query, false))) \\""","""                          rayQueryGetIntersectionBarycentricsEXT(query, false), lod, cut)) \\""")
old="""    rtTrace(query, position, facet, direction, flags, 0xffu);
    if (rayQueryGetIntersectionTypeEXT(query, true) == gl_RayQueryCommittedIntersectionNoneEXT)
        return roofed"""
assert old in s
if mode=='mip': cut="79.0 / 255.0"
else: cut="float(rtHash(floatBitsToUint(position.x) ^ rtHash(floatBitsToUint(direction.y) ^ floatBitsToUint(position.z))) >> 8u) * (1.0 / 16777216.0)"
s=s.replace(old,old.replace("rtTrace(query, position, facet, direction, flags, 0xffu);",f"rtTraceA(query, position, facet, direction, flags, 0xffu, {arg}.0, {cut});"))
open(p,'w').write(s)
