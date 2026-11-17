#!/usr/bin/env bash
set -euo pipefail
mkdir -p evidence
git rev-parse HEAD > evidence/commit.txt
gcc -std=c11 -O2 -Wall -Wextra -Werror sperf/sperf.c -o sperf/sperf
gcc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer ci/unit.c -o evidence/unit
gcc -std=c11 -O2 -Wall -Wextra -Werror ci/workload.c -o evidence/workload
./evidence/unit > evidence/unit.output
python3 - <<'PY'
from pathlib import Path
import subprocess,os,re,time
p=Path('evidence')
s=(p/'unit.output').read_bytes();assert s==b'read (66%)\nwrite (33%)\n'+b'\0'*80,s
bin=str(Path('sperf/sperf').resolve())
def frames(out):
 assert out.endswith(b'\0'*80)
 blocks=out.split(b'\0'*80)[:-1]
 for block in blocks:
  lines=block.decode().splitlines();assert len(lines)<=5
  names=[]
  for line in lines:
   m=re.fullmatch(r'([a-z0-9_]+) \((\d+)%\)',line);assert m,line
   names.append(m[1]);assert 0<=int(m[2])<=100
  assert len(set(names))==len(names)
 return blocks
for label,command in [('absolute',['/bin/true']),('path',['true']),('empty-path',['/bin/true']),('arguments',[str((p/'workload').resolve()),'x']),('relative',['./evidence/workload','x'])]:
 env=os.environ.copy()
 if label=='empty-path':env['PATH']=''
 start=time.monotonic();q=subprocess.run([bin,*command],capture_output=True,timeout=15,env=env)
 assert q.returncode==0,(label,q.stderr,q.returncode)
 blocks=frames(q.stdout);assert blocks
 if label in ['arguments','relative']:
  assert 8<=len(blocks)<=16,(label,len(blocks))
  assert b'clock_nanosleep' in q.stdout and b'HOSTILE' not in q.stdout
 (p/(label+'.output')).write_bytes(q.stdout)
for label,command in [('missing',['does-not-exist-123']),('exit7',['/bin/sh','-c','exit 7']),('usage',[])]:
 q=subprocess.run([bin,*command],capture_output=True,timeout=10)
 assert q.returncode==({'missing':127,'exit7':7,'usage':1}[label]),(label,q.returncode)
(p/'checks.result').write_text('PASS: parser/aggregation sanitizer unit; 5 real strace runs; 3 exit-status cases; periodic top5 framing and hostile output isolation\n')
PY
