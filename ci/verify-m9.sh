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
gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror -Ilibkvdb ci/crash.c evidence/kvdb.o -pthread -Wl,--wrap=pwrite,--wrap=fdatasync,--wrap=ftruncate -o evidence/crash
python3 - <<'PY'
from pathlib import Path
import subprocess,tempfile,shutil,json,hashlib,random,time,signal
root=Path('evidence').resolve();binary=root/'crash';rng=random.Random(6192026)
def run(*args):
    q=subprocess.run([str(binary),*map(str,args)],capture_output=True,text=True,timeout=30)
    assert q.returncode==0,(args,q.returncode,q.stdout,q.stderr)
    return q.stdout.strip()
summary={'model_cuts':[],'actual_sigkill':[],'io_failures':[]}
with tempfile.TemporaryDirectory(prefix='kvdb-crash-') as temporary:
    work=Path(temporary);seed=work/'seed.db';run('seed',seed)
    control=work/'control';control.mkdir();db=control/'db';shutil.copyfile(seed,db)
    run('write',db,control,0)
    events=(control/'events').read_text().splitlines()
    assert events[-1].endswith(' A') and len(events)>=10,events
    (root/'crash-events.txt').write_text('\n'.join(events)+'\n')
    saved=root/'crash-snapshots';saved.mkdir()
    for cut in range(1,len(events)+1):
        directory=work/f'cut-{cut}';directory.mkdir();db=directory/'db';shutil.copyfile(seed,db)
        q=subprocess.run([str(binary),'write',str(db),str(directory),str(cut)],capture_output=True,text=True,timeout=30)
        assert q.returncode==-signal.SIGKILL,(cut,q.returncode,q.stderr)
        stable=(directory/'stable').read_bytes();current=(directory/'current').read_bytes()
        (saved/f'{cut}-stable.db').write_bytes(stable);(saved/f'{cut}-current.db').write_bytes(current)
        check=directory/'stable-check.db';check.write_bytes(stable)
        must_new=run('check',check,0)=='new'
        length=len(current);base=stable+bytes(length-len(stable))
        assert length>=len(stable)
        changed=[i for i,(a,b) in enumerate(zip(base,current)) if a!=b]
        candidates=[('stable',stable),('current',current)]
        for n in sorted(set([0,1,2,3,7,8,len(changed)//2,len(changed)])):
            image=bytearray(base)
            for i in changed[:n]:image[i]=current[i]
            candidates.append((f'changed-prefix-{n}',bytes(image)))
        for pattern in range(6):
            image=bytearray(base)
            for i in changed:
                selected=(i//512)%2==pattern%2 if pattern<2 else ((i%3)==pattern-2 if pattern<5 else bool(rng.getrandbits(1)))
                if selected:image[i]=current[i]
            candidates.append((f'sector-byte-mask-{pattern}',bytes(image)))
        if length>len(stable):candidates.append(('partial-length',current[:len(stable)+(length-len(stable))//2]))
        observed=[];seen=set()
        for name,image in candidates:
            digest=hashlib.sha256(image).hexdigest()
            if digest in seen:continue
            seen.add(digest)
            path=directory/'restart.db';path.write_bytes(image)
            outcome=run('check',path,int(must_new))
            assert outcome in ['old','new'],outcome
            observed.append({'pattern':name,'sha256':digest,'bytes':len(image),'value':outcome})
        summary['model_cuts'].append({'event':events[cut-1],'sigkill':True,'stable_requires_new':must_new,'stable_sha256':hashlib.sha256(stable).hexdigest(),'current_sha256':hashlib.sha256(current).hexdigest(),'variants':observed})
    # A real running writer, no event cut or persistence substitution: after
    # kill, all observed ACKs must survive and at most one put is in flight.
    for cycle in range(12):
        db=work/f'live-{cycle}.db';shutil.copyfile(seed,db)
        q=subprocess.Popen([str(binary),'loop',str(db)],stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        ack=[]
        for _ in range(5):
            line=q.stdout.readline();assert line,line;ack.append(int(line))
        time.sleep(rng.uniform(0,.015))
        assert q.poll() is None,'writer unexpectedly finished before kill'
        q.kill();rest,err=q.communicate(timeout=10)
        assert q.returncode==-signal.SIGKILL and not err,(q.returncode,err)
        ack.extend(int(line) for line in rest.splitlines() if line.isdecimal())
        observed=int(run('counter',db));last=max(ack)
        assert last<=observed<=last+1,(cycle,last,observed)
        summary['actual_sigkill'].append({'cycle':cycle,'last_ack':last,'recovered_counter':observed})
    db=work/'short.db';shutil.copyfile(seed,db)
    (root/'short-io.txt').write_text(run('short',db)+'\n')
    for kind,points in [('W',range(1,6)),('D',range(1,5)),('T',[1])]:
        for point in points:
            db=work/f'error-{kind}-{point}.db';shutil.copyfile(seed,db)
            result=run('error',db,kind,point)
            summary['io_failures'].append({'syscall':kind,'index':point,'result':result})
(root/'crash-results.json').write_text(json.dumps(summary,indent=2)+'\n')
(root/'crash-checks.result').write_text(f'PASS: {len(summary["model_cuts"])} syscall/ACK cuts with real SIGKILL and {sum(len(c["variants"]) for c in summary["model_cuts"])} unique persisted-byte/length restart images; 12 live SIGKILL ACK-preservation cycles; EINTR/short writes; 10 injected I/O errors and subsequent recovery. This is a persistence model, not physical power-loss testing.\n')
PY
