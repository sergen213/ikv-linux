#!/usr/bin/env python3
"""Read-only SKYF/ADF structural corpus evidence; never extracts proprietary bytes."""
import argparse, collections, hashlib, json, pathlib, struct, zlib
MAX_DECODED = 512 * 1024 * 1024
from asset_skeleton import arithmetic_decode

def archive(path):
    with pathlib.Path(path).open('rb') as f:
        if f.read(6) != b'SKYF\x01\0': raise ValueError('SKYF v1 required')
        def directory(prefix='',depth=0):
            if depth>64: raise ValueError('directory depth')
            b=f.read(68)
            if len(b)!=68: raise ValueError('truncated directory')
            name=b[:64].split(b'\0')[0].decode('ascii'); prefix=prefix+'/'+name if prefix else name
            nd,nf=struct.unpack_from('<HH',b,64); entries=[]
            for _ in range(nf):
                b=f.read(136)
                if len(b)!=136: raise ValueError('truncated entry')
                entries.append((prefix+'/'+b[:128].split(b'\0')[0].decode('ascii'), *struct.unpack_from('<II',b,128)))
            for _ in range(nd): entries.extend(directory(prefix,depth+1))
            return entries
        entries=directory(); end=f.tell(); size=pathlib.Path(path).stat().st_size
        for n,o,s in entries:
            if o!=end or o+s>size: raise ValueError('member coverage')
            end=o+s
        if end!=size: raise ValueError('trailing payload')
        return entries

def chunks(data):
    if len(data)<24 or data[:4]!=b'root': raise ValueError('ADF root')
    count,stride,codec=struct.unpack_from('<III',data,4)
    if stride!=32 or count>(len(data)-24)//32: raise ValueError('ADF table')
    total=0; ranges=[]
    for j in range(count):
        tag,ident,stored,decoded,flag,offset,_,_=struct.unpack_from('<4s7I',data,24+j*32)
        if offset<24+count*32 or offset+stored>len(data): raise ValueError('chunk range')
        total+=decoded
        if total>MAX_DECODED or flag not in (0,1): raise ValueError('chunk allocation/flag')
        ranges.append((offset,offset+stored))
        src=data[offset:offset+stored]
        if flag:
            if codec==255:
                dec=zlib.decompressobj(); out=dec.decompress(src,decoded+1)
                if not dec.eof or dec.unused_data or dec.unconsumed_tail: raise ValueError('zlib stream')
            else:
                out=arithmetic_decode(src,decoded)
        else: out=src
        if len(out)!=decoded: raise ValueError('decoded size')
        yield tag.decode('ascii'),ident,out
    ordered=sorted(ranges)
    if any(a[1]>b[0] for a,b in zip(ordered,ordered[1:])): raise ValueError('chunk overlap')

def inspect(path, member):
    entries=archive(path); entry=next((e for e in entries if e[0].casefold()==member.casefold()),None)
    if not entry: raise ValueError('member not found')
    with pathlib.Path(path).open('rb') as f:
        f.seek(entry[1]); data=f.read(entry[2])
    records=list(chunks(data)); counts=collections.Counter(t for t,i,b in records)
    vertices=triangles=keys=0; bad_indices=0; materials=set(); geometry={}
    for tag,ident,b in records:
        stride={'mate':468,'item':256,'bone':256,'vert':48,'mver':16,'tris':160,'qkey':64,'wmap':32,'ptri':116}.get(tag)
        if stride and len(b)%stride: raise ValueError('record stride '+tag)
        if tag in ('vert','mver'):
            n=len(b)//stride; geometry[ident]=n; vertices+=n
        if tag=='mate':
            materials.update(struct.unpack_from('<I',b,j)[0] for j in range(0,len(b),468))
        if tag=='qkey':keys+=len(b)//64
    for tag,ident,b in records:
        if tag in ('tris','ptri'):
            stride=160 if tag=='tris' else 116; fmt='<3I' if tag=='tris' else '<3H'
            triangles+=len(b)//stride
            if ident in geometry:
                for j in range(0,len(b),stride):bad_indices+=sum(v>=geometry[ident] for v in struct.unpack_from(fmt,b,j))
    return {'member':entry[0],'size':entry[2],'sha256':hashlib.sha256(data).hexdigest(),'chunks':dict(counts),'decoded_bytes':sum(len(b) for t,i,b in records),'vertices':vertices,'triangles':triangles,'transform_keys':keys,'material_ids':len(materials),'out_of_range_triangle_indices':bad_indices}

def main():
    p=argparse.ArgumentParser();p.add_argument('archive');p.add_argument('members',nargs='+');p.add_argument('--output');a=p.parse_args()
    report={'format':'SKYF v1 / ADF root stride32','resources':[inspect(a.archive,m) for m in a.members]}
    text=json.dumps(report,ensure_ascii=False,indent=2)+'\n'
    if a.output:
        out=pathlib.Path(a.output).resolve();source=pathlib.Path(a.archive).resolve()
        if out==source or source.parent==out.parent:raise ValueError('output must be outside original asset directory')
        out.parent.mkdir(parents=True,exist_ok=True);out.write_text(text,encoding='utf8')
    else:print(text,end='')
if __name__=='__main__':main()
