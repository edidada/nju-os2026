#!/usr/bin/env bash
set -euo pipefail
mkdir -p evidence
git rev-parse HEAD > evidence/commit.txt
gcc -std=gnu2x -O2 -g -Wall -Wextra -Werror -c crepl/crepl.c -o evidence/crepl.o
gcc -std=gnu2x -O2 -g -Itestkit evidence/crepl.o crepl/tests.c testkit/testkit.c -ldl -o crepl/crepl
(cd crepl; TK_RUN=1 TK_VERBOSE=1 ./crepl </dev/null) 2>&1 | tee evidence/public-tests.txt
gcc -std=c11 -g -O1 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer crepl/crepl.c -ldl -o crepl/crepl-sanitized
python3 - <<'PY'
from pathlib import Path
import re,subprocess,os,select,tempfile
p=Path('evidence');s=(p/'public-tests.txt').read_text()
scores=re.findall(r'(\d+)/(\d+) test cases passed',s);assert scores and all(a==b and int(a)==9 for a,b in scores),scores
lines=['int gcd(int a,int b) { return b ? gcd(b,a%b) : a; }','gcd(256,144)*gcd(56,84)','int twice(int a) { return 2*gcd(a,24); }','twice(18)','int broken() { return ; bad syntax','unknown_function()','21 +','gcd(12,8)','int fib(int n) { return n<2 ? n : fib(n-1)+fib(n-2); }','fib(12)','-17 / 3','(3 << 4) | 2']
expected=['OK.','= 448.','OK.','= 12.','ERROR.','ERROR.','ERROR.','= 4.','OK.','= 144.','= -5.','= 50.']
# /proc is not a writable working directory; all compilation must use /tmp.
for binary in ['crepl/crepl','crepl/crepl-sanitized']:
 q=subprocess.run([str(Path(binary).resolve())],input='\n'.join(lines)+'\n',text=True,capture_output=True,cwd='/proc',timeout=40)
 assert q.returncode==0,(binary,q.stderr)
 assert q.stdout.splitlines()==expected,(binary,q.stdout,q.stderr)
 assert 'AddressSanitizer' not in q.stderr and 'runtime error:' not in q.stderr
 (p/(Path(binary).name+'.output')).write_text(q.stdout)
 (p/(Path(binary).name+'.stderr')).write_text(q.stderr)
# Keep stdin open: every input must produce an immediate flushed response.
q=subprocess.Popen([str(Path('crepl/crepl').resolve())],stdin=subprocess.PIPE,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
for line,expected in [('int foo(int n) { return n+1; }','OK.'),('foo(41)','= 42.'),('1 +','ERROR.'),('foo(0)','= 1.')]:
 q.stdin.write(line+'\n');q.stdin.flush()
 assert select.select([q.stdout],[],[],10)[0],'response was not flushed'
 assert q.stdout.readline().strip()==expected
q.stdin.close();assert q.wait(timeout=10)==0
# More than 100 definitions checks dynamic handle ownership and cleanup.
source='\n'.join('int f%d(void) { return %d; }'%(i,i) for i in range(100))+'\nf0()+f99()\n'
q=subprocess.run([str(Path('crepl/crepl').resolve())],input=source,text=True,capture_output=True,timeout=60)
assert q.returncode==0 and q.stdout.splitlines()==['OK.']*100+['= 99.']
(p/'checks.result').write_text('PASS: 9 original tests; 24 normal/sanitized line results; 4 interactive flush/recovery results; 100 definitions and final expression\n')
PY
