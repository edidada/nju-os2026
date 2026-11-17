#!/usr/bin/env bash
set -euo pipefail
mkdir -p evidence
git rev-parse HEAD > evidence/commit.txt
gcc --version > evidence/compiler.txt
gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror -fPIC -Ilibkvdb -c libkvdb/kvdb.c -pthread -o evidence/kvdb.o
gcc -shared evidence/kvdb.o -pthread -o evidence/libkvdb.so
gcc -std=gnu2x -O2 -g -Ilibkvdb -Itestkit libkvdb/tests/test_main.c testkit/testkit.c evidence/kvdb.o -pthread -o evidence/public-tests
TK_RUN=1 TK_VERBOSE=1 evidence/public-tests 2>&1 | tee evidence/public-tests.txt
gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror -Ilibkvdb ci/stress.c evidence/kvdb.o -pthread -o evidence/stress
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -Ilibkvdb -fsanitize=address,undefined -fno-omit-frame-pointer ci/stress.c libkvdb/kvdb.c -pthread -o evidence/stress-sanitized
work=$(mktemp -d)
trap 'rm -rf -- "$work"' EXIT
timeout 180 evidence/stress "$work/normal.db" > evidence/stress.txt
timeout 180 evidence/stress-sanitized "$work/sanitized.db" > evidence/stress-sanitized.txt
python3 - <<'PY'
from pathlib import Path
import ctypes as C,threading,time,tempfile,json,re,functools,concurrent.futures
scores=re.findall(r'(\d+)/(\d+) test cases passed',Path('evidence/public-tests.txt').read_text())
assert scores and all(a==b=='2' for a,b in scores),scores
class DB(C.Structure):_fields_=[('path',C.c_void_p),('fd',C.c_int),('cookie',C.c_uint64)]
lib=C.CDLL(str(Path('evidence/libkvdb.so').resolve()))
lib.kvdb_open.argtypes=[C.POINTER(DB),C.c_char_p]
lib.kvdb_close.argtypes=[C.POINTER(DB)]
lib.kvdb_put.argtypes=[C.POINTER(DB),C.c_char_p,C.c_char_p]
lib.kvdb_get.argtypes=[C.POINTER(DB),C.c_char_p,C.c_void_p,C.c_size_t]
all_history=[]
with tempfile.TemporaryDirectory(prefix='kvdb-history-') as d:
 db=DB();assert lib.kvdb_open(C.byref(db),(d+'/history.db').encode())==0
 for round in range(20):
  assert lib.kvdb_put(C.byref(db),b'hot',b'initial')==0
  barrier=threading.Barrier(3);history=[];lock=threading.Lock()
  def run(thread):
   barrier.wait()
   for i in range(4):
    value=f'{round}-{thread}-{i}'.encode();start=time.monotonic_ns()
    if i%2==0:
     assert lib.kvdb_put(C.byref(db),b'hot',value)==0;kind='put';result=value.decode()
    else:
     buf=C.create_string_buffer(128);n=lib.kvdb_get(C.byref(db),b'hot',buf,len(buf));assert n==len(buf.value);kind='get';result=buf.value.decode()
    end=time.monotonic_ns()
    with lock:history.append({'thread':thread,'index':i,'kind':kind,'value':result,'start':start,'end':end})
  with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:list(pool.map(run,range(3)))
  pred=[sum(1<<j for j,p in enumerate(history) if p['end']<op['start']) for op in history]
  @functools.lru_cache(None)
  def legal(mask,state):
   if mask==(1<<len(history))-1:return True
   for i,op in enumerate(history):
    if mask>>i&1 or pred[i]&~mask:continue
    if op['kind']=='get' and op['value']!=state:continue
    if legal(mask|1<<i,op['value'] if op['kind']=='put' else state):return True
   return False
  assert legal(0,'initial'),history
  all_history.append({'round':round,'linearizable':True,'operations':history})
 assert lib.kvdb_close(C.byref(db))==0
Path('evidence/histories.json').write_text(json.dumps(all_history,indent=2)+'\n')
Path('evidence/checks.result').write_text('PASS: original 2/2; normal/ASan/UBSan shared/separate/inherited-handle concurrency and reopen; 20 histories of 12 operations checked against real-time-constrained serial KV semantics. Crash-model validation remains separate.\n')
PY
