# allsparse.py SRC: every resolved near surface (not only foliage) traces GI+reflection on one 2x2 block of four per frame.
import sys
src=sys.argv[1]; p=src+'/lighting.glsl'; s=open(p).read()
old="} else if(frame.shadowParams.z>.5&&frame.params.z>.5&&foliage) {"
assert old in s
s=s.replace(old,"} else if(frame.shadowParams.z>.5&&frame.params.z>.5&&!(draw.extra.w>.75&&draw.flags.w<=0)) {",1)
open(p,'w').write(s)
