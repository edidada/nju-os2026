#!/usr/bin/env bash
set -euo pipefail
mkdir -p evidence
git rev-parse HEAD > evidence/commit.txt
gcc -std=gnu2x -O2 -g -Wall -Wextra -Werror -c pstree/pstree.c -o evidence/pstree.o
# Keep upstream TestKit unchanged; its own warnings are not task-source errors.
gcc -std=gnu2x -O2 -g -Wall -Itestkit evidence/pstree.o pstree/tests.c testkit/testkit.c -o pstree/pstree
(cd pstree; TK_RUN=1 TK_VERBOSE=1 ./pstree -V) 2>&1 | tee evidence/public-tests.txt
python3 - <<'PY'
from pathlib import Path
import re
s=Path('evidence/public-tests.txt').read_text()
scores=re.findall(r'(\d+)/(\d+) test cases passed',s)
assert scores and all(a==b and int(a)>0 for a,b in scores),scores
PY
gcc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer pstree/pstree.c -o pstree/pstree-sanitized
python3 - <<'PY'
import subprocess,ctypes,os,signal,json,re
from pathlib import Path
helper=r'''import os,ctypes,json,signal
lib=ctypes.CDLL(None)
def name(s):
 assert lib.prctl(15,s.encode(),0,0,0)==0
name('m2-root')
children=[]
for label in ['z) child','a child','middle']:
 r,w=os.pipe()
 pid=os.fork()
 if pid==0:
  os.close(r);name(label)
  grand=os.fork()
  if grand==0:
   os.close(w);name('leaf) space');signal.pause();os._exit(0)
  os.write(w,str(grand).encode());os.close(w);signal.pause();os._exit(0)
 os.close(w);grand=int(os.read(r,100));os.close(r)
 children.append([pid,grand,label])
print(json.dumps([os.getpid(),children]),flush=True)
signal.pause()
'''
p=subprocess.Popen(['python3','-c',helper],stdout=subprocess.PIPE,text=True,start_new_session=True)
try:
 root,children=json.loads(p.stdout.readline())
 for binary in ['pstree/pstree','pstree/pstree-sanitized']:
  for flags in [['-p','-n'],['--show-pids','--numeric-sort'],['-p'],[]]:
   out=subprocess.check_output([binary,*flags],text=True,timeout=15)
   tag=Path(binary).name+'-'+('-'.join(flags) or 'default')
   Path('evidence',tag+'.output').write_text(out)
   if '-p' in flags or '--show-pids' in flags:
    records={};ordered=[]
    for line in out.splitlines():
     m=re.fullmatch(r'( *)(.*)\((\d+)\)',line)
     assert m,line
     pid=int(m[3]);assert pid not in records
     records[pid]=(len(m[1])//2,m[2]);ordered.append(pid)
    assert root in records
    for child,grand,label in children:
     assert records[child]==(records[root][0]+1,label),(child,records.get(child))
     assert records[grand]==(records[child][0]+1,'leaf) space'),(grand,records.get(grand))
     assert ordered.index(root)<ordered.index(child)<ordered.index(grand)
    actual=[pid for pid in ordered if pid in [c[0] for c in children]]
    expected=sorted(children,key=(lambda c:c[0]) if '-n' in flags or '--numeric-sort' in flags else lambda c:c[2])
    assert actual==[c[0] for c in expected],(actual,expected)
   else:
    assert 'm2-root' in out and 'leaf) space' in out
  for flag in ['-V','--version']:
   assert subprocess.check_output([binary,flag],text=True).startswith('pstree ')
  for flag in ['--invalid-option','-np','operand']:
   q=subprocess.run([binary,flag],capture_output=True,text=True)
   assert q.returncode!=0 and 'Usage' in q.stderr
 Path('evidence/controlled.result').write_text('PASS: two builds, 8 tree snapshots, 10 option checks; three child/grandchild branches and names containing parentheses\n')
finally:
 os.killpg(p.pid,signal.SIGKILL);p.wait()
PY
