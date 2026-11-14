# fsparse.py SRC SHIFT [STRIDE=2]: blended alpha-tested draws (foliage, grass) trace GI+reflection on one pixel tile of each
# STRIDExSTRIDE group per frame (tile = 2^SHIFT pixels), reusing the resolved history elsewhere (as sparse far indirect).
import sys
src,shift=sys.argv[1],int(sys.argv[2]); n=int(sys.argv[3]) if len(sys.argv)>3 else 2
p=src+'/lighting.glsl'; s=open(p).read()
old="""    indirect=uint[16](0u,8u,2u,10u,12u,4u,14u,6u,3u,11u,1u,9u,15u,7u,13u,5u)[q.y*4u+q.x]==(uint(frame.viewport.z)&15u);
}"""
assert old in s
pat={2:"uint[4](0u,2u,3u,1u)[q.y*2u+q.x]==(uint(frame.viewport.z)&3u)",4:"uint[16](0u,8u,2u,10u,12u,4u,14u,6u,3u,11u,1u,9u,15u,7u,13u,5u)[q.y*4u+q.x]==(uint(frame.viewport.z)&15u)"}[n]
new=old+f"""
else if(frame.shadowParams.y>0&&frame.params.z>.5&&draw.extra.w>.75&&draw.extra.w<2.&&draw.flags.w>0) {{
    uvec2 q=(uvec2(gl_FragCoord.xy)>>{shift}u)&{n-1}u;
    indirect={pat};
}}"""
s=s.replace(old,new); open(p,'w').write(s)
