#!/usr/bin/env bash
set -euo pipefail
mkdir -p evidence
git rev-parse HEAD > evidence/commit.txt
git rev-parse origin/M6 > evidence/starter-commit.txt
lscpu > evidence/cpu.txt
gcc --version > evidence/compiler.txt
python3 - <<'PY'
from pathlib import Path
import urllib.request, re, json, struct, hashlib
p=Path('gpt/gpt2_124M.bin')
if not p.exists():
    page=urllib.request.urlopen('https://box.nju.edu.cn/f/ca18466ad6054f2d85c3/',timeout=60).read().decode()
    raw=re.search(r"rawPath: '([^']+)'",page).group(1)
    url=json.loads('"'+raw+'"')
    with urllib.request.urlopen(url,timeout=180) as response,p.open('wb') as out:
        while chunk:=response.read(1024*1024): out.write(chunk)
assert p.stat().st_size==497760256,p.stat().st_size
assert hashlib.file_digest(p.open('rb'),'sha256').hexdigest()=='6146cdd8493c97e4ce74b3b81d52af9bbf6eddda59879599db38b2c932cd4b43'
with p.open('rb') as f: header=struct.unpack('<256i',f.read(1024))
assert header[:7]==(20240326,1,1024,50257,12,12,768),header[:7]
Path('evidence/model.json').write_text(json.dumps({'source':'https://box.nju.edu.cn/f/ca18466ad6054f2d85c3/','size':p.stat().st_size,'sha256':hashlib.file_digest(p.open('rb'),'sha256').hexdigest(),'header':header[:7]},indent=2)+'\n')
PY
git show origin/M6:gpt/gpt.c > evidence/serial.c
gcc -std=gnu11 -O2 -g -pg -Igpt evidence/serial.c -pthread -lm -o gpt/profile
(cd gpt; timeout 180 ./profile 31373 612 > ../evidence/profile-tokens.txt; gprof ./profile gmon.out > ../evidence/profile.txt)
gcc -std=gnu11 -O2 -g -Igpt evidence/serial.c -pthread -lm -o gpt/serial
python3 - <<'PY'
from pathlib import Path
import subprocess,json,time
cases=[[31373,612],[50256],[31373,612,338,635,281,4998,3715,351,2506]]
results=[]
for i,tokens in enumerate(cases):
    start=time.monotonic()
    q=subprocess.run(['./serial',*map(str,tokens)],cwd='gpt',capture_output=True,text=True,timeout=180,check=True)
    out=list(map(int,q.stdout.split()))
    assert len(out)==10-len(tokens) and all(0<=v<50257 for v in out),out
    if i==2: assert out==[852],out
    Path(f'evidence/serial-{i}.txt').write_text(q.stdout)
    results.append({'input':tokens,'generated':out,'wall_seconds_including_model_load':time.monotonic()-start})
assert Path('evidence/profile-tokens.txt').read_text()==Path('evidence/serial-0.txt').read_text()
Path('evidence/serial-results.json').write_text(json.dumps(results,indent=2)+'\n')
Path('evidence/checks.result').write_text('PASS: official model header/size; original serial inference for three prompts; published expected token 852; profiled and normal output identical\n')
PY
for workers in 1 2 4; do
  gcc -std=gnu11 -O2 -g -DGPT_WORKERS="$workers" gpt/gpt.c -pthread -lm -o "gpt/parallel-$workers"
  gcc -std=gnu11 -O2 -g -DGPT_WORKERS="$workers" ci/matmul-check.c -pthread -lm -o "gpt/matmul-$workers"
  timeout 60 "gpt/matmul-$workers" > "evidence/matmul-$workers.txt"
  gcc -std=gnu11 -O2 -g -DGPT_WORKERS="$workers" ci/benchmark.c -pthread -lm -o "gpt/benchmark-$workers"
done
gcc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer ci/matmul-check.c -pthread -lm -o gpt/matmul-sanitized
timeout 60 gpt/matmul-sanitized > evidence/matmul-sanitized.txt
gcc -std=gnu11 -O2 -g -Igpt -DGPT_SOURCE='"../evidence/serial.c"' ci/benchmark.c -pthread -lm -o gpt/benchmark-serial
gcc -std=gnu2x -O2 -g -Itestkit gpt/gpt.c gpt/tests.c testkit/testkit.c -pthread -lm -o gpt/gpt
(cd gpt; TK_RUN=1 TK_VERBOSE=1 ./gpt </dev/null) 2>&1 | tee evidence/public-tests.txt
python3 - <<'PY'
from pathlib import Path
import subprocess,json,re
root=Path('evidence')
original=json.loads((root/'serial-results.json').read_text())
for workers in [1,2,4]:
    for i,case in enumerate(original):
        q=subprocess.run([f'./parallel-{workers}',*map(str,case['input'])],cwd='gpt',capture_output=True,text=True,timeout=180,check=True)
        assert q.stdout==(root/f'serial-{i}.txt').read_text(),(workers,i,q.stdout)
        assert not q.stderr,q.stderr
        (root/f'parallel-{workers}-{i}.txt').write_text(q.stdout)
scores=re.findall(r'(\d+)/(\d+) test cases passed',(root/'public-tests.txt').read_text())
assert scores and all(a==b=='1' for a,b in scores),scores
for name in ['serial','1','2','4']:
    q=subprocess.run([f'./benchmark-{name}'],cwd='gpt',capture_output=True,text=True,timeout=180,check=True)
    (root/f'benchmark-{name}.csv').write_text(q.stdout)
    (root/f'benchmark-{name}.stderr').write_text(q.stderr)
(root/'parallel-checks.result').write_text('PASS: original 1/1; three prompts at 1/2/4 workers exactly match original; 196 scalar cases per build plus ASan/UBSan; three timed eight-token inference rounds per configuration\n')
PY
