#!/usr/bin/env bash
set -euo pipefail
mkdir -p evidence .cache
git rev-parse HEAD > evidence/commit.txt
gcc --version > evidence/compiler.txt
mkfs.fat --help > evidence/mkfs-version.txt 2>&1 || true
gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror -DFSRECOV_TRACE fsrecov/fsrecov.c -o evidence/fsrecov
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -DFSRECOV_TRACE fsrecov/fsrecov.c -o evidence/fsrecov-sanitized
/usr/bin/python3 - <<'PY'
from pathlib import Path
import subprocess,os,json,re,hashlib,urllib.request,shutil,time,tempfile,struct
from PIL import Image
root=Path('evidence').resolve();cache=Path('.cache/fsrecov.img')
os.environ['MTOOLS_SKIP_CHECK']='1'
if not cache.exists():
    page=urllib.request.urlopen('https://box.nju.edu.cn/f/66a41ec46a334e8d8203/',timeout=60).read().decode()
    raw=re.search(r"rawPath: '([^']+)'",page).group(1)
    url=json.loads('"'+raw+'"')
    with urllib.request.urlopen(url,timeout=120) as response,cache.open('wb') as out:
        while chunk:=response.read(1024*1024):out.write(chunk)
assert cache.stat().st_size==64*1024*1024,cache.stat().st_size
assert hashlib.file_digest(cache.open('rb'),'sha256').hexdigest()=='4ff3dc690a55ee964ed31a693902b59b8d2da129779ea16704038a8dd05cb7bd'
(root/'official-source.json').write_text(json.dumps({'url':'https://box.nju.edu.cn/f/66a41ec46a334e8d8203/','size':cache.stat().st_size,'sha256':hashlib.file_digest(cache.open('rb'),'sha256').hexdigest()},indent=2)+'\n')

def command(args):
    q=subprocess.run(list(map(str,args)),capture_output=True,text=True,check=True,timeout=60)
    return q.stdout+q.stderr
def extract(image,directory):
    directory.mkdir()
    command(['mcopy','-i',image,'::/DCIM/*',directory])
    return {p.name:hashlib.sha1(p.read_bytes()).hexdigest() for p in directory.iterdir() if p.suffix.lower()=='.bmp'}
def evaluate(binary,image,expected,label):
    before=hashlib.file_digest(image.open('rb'),'sha256').hexdigest()
    start=time.monotonic()
    q=subprocess.run([str(root/binary),str(image)],capture_output=True,text=True,timeout=60 if binary.endswith("sanitized") else 10)
    seconds=time.monotonic()-start
    (root/f'{label}-{binary}.stdout').write_text(q.stdout)
    (root/f'{label}-{binary}.stderr').write_text(q.stderr)
    assert q.returncode==0,(label,binary,q.returncode,q.stderr)
    assert 'AddressSanitizer' not in q.stderr and 'runtime error:' not in q.stderr,q.stderr
    assert all(re.fullmatch(r'[0-9a-f]{40}  [A-Za-z0-9_-]+\.[bB][mM][pP]',line) for line in q.stdout.splitlines()),q.stdout
    pairs=[line.split() for line in q.stdout.splitlines()]
    names={name for digest,name in pairs if name in expected}
    exact={name for digest,name in pairs if expected.get(name)==digest}
    assert before==hashlib.file_digest(image.open('rb'),'sha256').hexdigest(),'modified source image'
    result={'label':label,'binary':binary,'seconds':seconds,'expected':len(expected),'name_count':len(names),'exact_count':len(exact),'name_ratio':len(names)/len(expected),'exact_ratio':len(exact)/len(expected),'missing_names':sorted(set(expected)-names),'incorrect_or_missing_content':sorted(set(expected)-exact),'source_unchanged':True}
    (root/f'{label}-{binary}.json').write_text(json.dumps(result,indent=2)+'\n')
    return result

results=[]
with tempfile.TemporaryDirectory(prefix='m8-') as temporary:
    work=Path(temporary)
    expected=extract(cache,work/'official-files')
    assert len(expected)>10,len(expected)
    (root/'official-expected.json').write_text(json.dumps(expected,indent=2)+'\n')
    (root/'official-directory.txt').write_text(command(['mdir','-i',cache,'::/DCIM']))
    (root/'official-cluster-chains.txt').write_text(command(['mshowfat','-i',cache,'::/DCIM/*']))
    formatted=work/'formatted.img';shutil.copyfile(cache,formatted)
    (root/'official-format.txt').write_text(command(['mkfs.fat','-F','32','-S','512','-s','8',formatted]))
    for binary in ['fsrecov','fsrecov-sanitized']:
        results.append(evaluate(binary,cache,expected,'official-intact'))
        results.append(evaluate(binary,formatted,expected,'official-formatted'))

    for case,(sector,spc) in enumerate([(512,1),(512,8),(1024,1)]):
        image=work/f'fixture-{case}.img'
        with image.open('wb') as f:f.truncate(64*1024*1024)
        command(['mkfs.fat','-F','32','-S',sector,'-s',spc,image])
        command(['mmd','-i',image,'::/DCIM'])
        expected={}
        names=['a.bmp','UPPER.BMP','lower.bmp','AbCdEfGhI.bmp','Long_File-name_123456789.bmp','ThirteenX.bmp']
        for i,name in enumerate(names):
            p=work/name
            im=Image.new('RGB',(41+i*2,43+i*3));im.putdata([((x*3+y)%256,(y*7+x)%256,(x+y*5)%256) for y in range(im.height) for x in range(im.width)])
            im.save(p)
            expected[name]=hashlib.sha1(p.read_bytes()).hexdigest()
            command(['mcopy','-i',image,p,'::/DCIM/'+name])
        command(['mkfs.fat','-F','32','-S',sector,'-s',spc,image])
        for binary in ['fsrecov','fsrecov-sanitized']:
            r=evaluate(binary,image,expected,f'fixture-{case}')
            results.append(r)
            assert r['exact_count']==len(expected),r
    # Truncation, invalid geometry, and random junk must not dereference
    # unvalidated BPB/cluster pointers, including under sanitizers.
    bad_cases=[b'',bytes(511),bytes(512),b'BM'+os.urandom(2046)]
    boot=bytearray(cache.read_bytes()[:512])
    for offset,data in [(11,b'\x00\x00'),(13,b'\x03'),(36,b'\xff'*4),(32,b'\xff'*4)]:
        malformed=bytearray(boot);malformed[offset:offset+len(data)]=data;bad_cases.append(malformed)
    for i,data in enumerate(bad_cases):
        bad=work/f'bad-{i}.img';bad.write_bytes(data)
        for binary in ['fsrecov','fsrecov-sanitized']:
            q=subprocess.run([str(root/binary),str(bad)],capture_output=True,text=True,timeout=5)
            assert q.returncode==1 and not q.stdout,(i,binary,q.returncode,q.stderr)
            assert 'AddressSanitizer' not in q.stderr and 'runtime error:' not in q.stderr,q.stderr
    (root/'malformed.result').write_text(f'PASS: {len(bad_cases)} malformed images per build\n')
    (root/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    for r in results:
        if r['label']=='official-formatted':
            assert r['name_ratio']>.75 and r['exact_ratio']>.50,r
(root/'checks.result').write_text('PASS: official formatted image >75% names and >50% exact image hashes; three generated FAT32 geometries fully recovered in normal/ASan/UBSan; source unchanged; malformed images rejected\n')
PY
