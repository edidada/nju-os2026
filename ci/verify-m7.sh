#!/usr/bin/env bash
set -euo pipefail
mkdir -p evidence
git rev-parse HEAD > evidence/commit.txt
gcc --version > evidence/compiler.txt
gcc -std=gnu11 -O2 -g -Wall -Wextra -Werror httpd/httpd.c -pthread -o evidence/httpd
gcc -std=gnu11 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer httpd/httpd.c -pthread -o evidence/httpd-sanitized
python3 - <<'PY'
from pathlib import Path
import os,socket,subprocess,tempfile,shutil,time,threading,re,json,concurrent.futures,select
evidence=Path('evidence').resolve()
assert not re.search(r'\b(?:SystemTest|UnitTest)\s*\(',Path('httpd/tests.c').read_text()),'revisit official coverage'

def wait_until(predicate, timeout=8):
    end=time.monotonic()+timeout
    while time.monotonic()<end:
        if predicate(): return
        time.sleep(.01)
    raise AssertionError('condition did not become true')

def exchange(port,path=None,raw=None,chunks=None):
    s=socket.create_connection(('127.0.0.1',port),timeout=10)
    s.settimeout(15)
    if raw is None: raw=f'GET {path} HTTP/1.1\r\nHost: localhost\r\n\r\n'.encode()
    for chunk in chunks or [raw]:
        s.sendall(chunk)
        if chunks: time.sleep(.01)
    data=bytearray()
    while True:
        try: b=s.recv(65536)
        except ConnectionResetError:
            if data: break
            raise
        if not b: break
        data.extend(b)
    s.close()
    data=bytes(data)
    separator=b'\r\n\r\n' if b'\r\n\r\n' in data else b'\n\n'
    head,body=data.split(separator,1)
    status=int(head.splitlines()[0].split()[1])
    return status,body,data

all_results=[]
for binary in ['httpd','httpd-sanitized']:
 with tempfile.TemporaryDirectory(prefix='m7-') as directory:
    root=Path(directory); cgi=root/'cgi-bin';cgi.mkdir()
    shutil.copy2('httpd/cgi-bin/echo',cgi/'echo')
    def script(name,body,executable=True):
        p=cgi/name;p.write_text('#!/bin/sh\n'+body+'\n');p.chmod(0o755 if executable else 0o644)
    script('env', '''printf 'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n%s|%s' "$REQUEST_METHOD" "$QUERY_STRING"''')
    script('forbidden', "printf 'HTTP/1.1 403 Forbidden\\r\\nContent-Length: 2\\r\\n\\r\\nno'")
    script('standard', "printf 'Status: 418 Teapot\\r\\nContent-Type: text/plain\\r\\n\\r\\ntea'")
    script('default', "printf 'Content-Type: text/plain\\n\\ndefault'")
    script('fail', "printf 'HTTP/1.1 200 OK\\r\\n\\r\\nLEAK'; exit 7")
    script('signal', 'kill -TERM $$')
    script('empty', 'exit 0')
    script('garbage', "printf 'not an HTTP response'")
    script('nonexec', 'exit 0',False)
    (cgi/'bad-interpreter').write_text('#!/nonexistent-m7-interpreter\n');(cgi/'bad-interpreter').chmod(0o755)
    script('blob', "printf 'HTTP/1.1 200 OK\\r\\nContent-Length: 2097152\\r\\n\\r\\n'; dd if=/dev/zero bs=65536 count=32 2>/dev/null")
    script('odd name', "printf 'HTTP/1.1 200 OK\\r\\n\\r\\nescaped'")
    (cgi/'nested').mkdir();script('nested/ok', "printf 'HTTP/1.1 200 OK\\r\\n\\r\\nnested'")
    outside=root/'outside';outside.write_text('#!/bin/sh\nprintf SECRET\n');outside.chmod(0o755)
    (cgi/'link').symlink_to(outside)
    (cgi/'dirlink').symlink_to(root,target_is_directory=True)
    os.mkfifo(cgi/'fifo')
    gate='''touch "started-$QUERY_STRING"
while [ ! -f "release-$QUERY_STRING" ]; do sleep .01; done
printf 'HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n\r\n%s' "$QUERY_STRING"'''
    for i in range(5): script(f'gate{i}',gate)
    probe=socket.socket();probe.bind(('127.0.0.1',0));port=probe.getsockname()[1];probe.close()
    lines=[];logs=[]
    env=dict(os.environ,REQUEST_METHOD='POISON',QUERY_STRING='POISON')
    stderr=(evidence/f'{binary}.stderr').open('w')
    server=subprocess.Popen([str(evidence/binary),str(port)],cwd=root,stdout=subprocess.PIPE,stderr=stderr,text=True,env=env)
    def read_logs():
        for line in server.stdout:
            lines.append(line)
            m=re.fullmatch(r'\[[^\]]+\] \[([^\]]+)\] \[([^\]]+)\] \[(\d+)\]\n',line)
            if m: logs.append((m[1],m[2],int(m[3])))
    reader=threading.Thread(target=read_logs,daemon=True);reader.start()
    results={'binary':binary,'cases':[]}
    try:
        wait_until(lambda: bool(lines) or server.poll() is not None)
        assert server.poll() is None and 'Server listening' in lines[0],lines
        initial_fds=len(list(Path(f'/proc/{server.pid}/fd').iterdir()))
        def check(path,status,contains=None,raw=None,chunks=None):
            before=len(logs)
            actual,body,data=exchange(port,path,raw,chunks)
            assert actual==status,(path,status,actual,data[:500])
            if contains is not None: assert contains in body,(path,body[:500])
            wait_until(lambda:len(logs)==before+1)
            assert logs[-1][2]==status,(path,logs[-1])
            if path is not None: assert logs[-1][1]==path.split('?',1)[0],logs[-1]
            results['cases'].append({'path':path,'status':actual,'body_bytes':len(body)})
            return body
        check('/cgi-bin/echo?name=world',200,b'name=world')
        check('/cgi-bin/env?name=a%20b&x=2',200,b'GET|name=a%20b&x=2')
        check('/cgi-bin/env',200,b'GET|')
        check('/cgi-bin/forbidden',403,b'no')
        check('/cgi-bin/standard',418,b'tea')
        check('/cgi-bin/default',200,b'default')
        check('/cgi-bin/odd%20name',200,b'escaped')
        check('/cgi-bin/nested/ok',200,b'nested')
        for name in ['fail','signal','empty','garbage','nonexec','bad-interpreter']:
            body=check('/cgi-bin/'+name,500)
            assert b'LEAK' not in body
        for path in ['/','/index.html','/cgi-bin/missing','/cgi-bin/','/cgi-bin/../outside','/cgi-bin/%2e%2e/outside','/cgi-bin/%2foutside','/cgi-bin/env%00x','/cgi-bin/env%','/cgi-bin/link','/cgi-bin/dirlink/outside','/cgi-bin/fifo','/cgi-bin/nested','/cgi-bin//env','/cgi-bin/env/']:
            check(path,404)
        check('/cgi-bin/env',405,raw=b'POST /cgi-bin/env HTTP/1.1\r\nHost: x\r\n\r\n')
        check(None,400,raw=b'GET /cgi-bin/env WRONG\r\n\r\n')
        check(None,431,raw=b'GET /cgi-bin/env HTTP/1.1\r\nX: '+b'a'*16400)
        check('/cgi-bin/env?q=fragmented',200,b'GET|q=fragmented',chunks=[b'GE',b'T /cgi-bin/env?q=fragmented HTTP/1.1\r\nHo',b'st: x\r\n',b'\r\n'])
        body=check('/cgi-bin/blob',200)
        assert body==bytes(2097152),len(body)

        # Four CGI processes stay gated. The fifth connection must already
        # have been accepted, but its CGI cannot start until a worker is free.
        before=len(logs)
        with concurrent.futures.ThreadPoolExecutor(max_workers=5) as pool:
            try:
                futures=[]
                for i in range(4):
                    futures.append(pool.submit(exchange,port,f'/cgi-bin/gate{i}?{i}'))
                    wait_until(lambda i=i:(root/f'started-{i}').exists())
                futures.append(pool.submit(exchange,port,'/cgi-bin/gate4?4'))
                def socket_count():
                    count=0
                    for p in Path(f'/proc/{server.pid}/fd').iterdir():
                        try: count+=os.readlink(p).startswith('socket:')
                        except FileNotFoundError: pass
                    return count
                wait_until(lambda:socket_count()>=6)
                assert not (root/'started-4').exists()
                (root/'release-1').touch()
                assert futures[1].result(timeout=8)[:2]==(200,b'1')
                wait_until(lambda:(root/'started-4').exists())
                for i in [2,3,4]:
                    (root/f'release-{i}').touch()
                    assert futures[i].result(timeout=8)[:2]==(200,str(i).encode())
                assert not futures[0].done() and len(logs)==before
                (root/'release-0').touch()
                assert futures[0].result(timeout=8)[:2]==(200,b'0')
            finally:
                for i in range(5): (root/f'release-{i}').touch()
        wait_until(lambda:len(logs)==before+5)
        assert logs[before:]==[('GET',f'/cgi-bin/gate{i}',200) for i in range(5)],logs[before:]
        results['gated_concurrency']={'active':4,'fifth_accepted_while_four_active':True,'fifth_started_only_after_slot_release':True,'fast_responses_before_slow':True,'logs_in_accept_order':True,'logs_flushed_without_next_request':True}

        before=len(logs)
        with concurrent.futures.ThreadPoolExecutor(max_workers=12) as pool:
            futures={pool.submit(exchange,port,f'/cgi-bin/env?unique={i}'):i for i in range(48)}
            for future in concurrent.futures.as_completed(futures):
                i=futures[future];status,body,_=future.result()
                assert status==200 and body==f'GET|unique={i}'.encode(),(i,status,body)
        wait_until(lambda:len(logs)==before+48)
        results['isolated_environment_requests']=48
        before=len(logs)
        s=socket.create_connection(('127.0.0.1',port));s.sendall(b'GET /cgi-bin/env?disconnect HTTP/1.1\r\nHost: x\r\n\r\n')
        s.shutdown(socket.SHUT_WR);s.close()
        wait_until(lambda:len(logs)==before+1)
        check('/cgi-bin/env?still=alive',200,b'GET|still=alive')
        wait_until(lambda:len(list(Path(f'/proc/{server.pid}/fd').iterdir()))==initial_fds)
        children=[]
        for p in Path(f'/proc/{server.pid}/task').glob('*/children'): children.extend(p.read_text().split())
        assert not children,children
        assert server.poll() is None
        results['fd_count_restored']=initial_fds
        results['unreaped_children']=children
        results['logged_requests']=len(logs)
        results['official_test_cases']=0
    finally:
        server.terminate()
        try: server.wait(timeout=5)
        except subprocess.TimeoutExpired: server.kill();server.wait()
        reader.join(timeout=2);stderr.close()
        (evidence/f'{binary}.stdout').write_text(''.join(lines))
        (evidence/f'{binary}-results.json').write_text(json.dumps(results,indent=2)+'\n')
    errors=(evidence/f'{binary}.stderr').read_text()
    assert 'AddressSanitizer' not in errors and 'runtime error:' not in errors,errors
    all_results.append(results)
# Exercise the documented no-argument port independently of custom-port runs.
with tempfile.TemporaryDirectory(prefix='m7-default-') as directory:
    root=Path(directory);(root/'cgi-bin').mkdir()
    shutil.copy2('httpd/cgi-bin/echo',root/'cgi-bin/echo')
    server=subprocess.Popen([str(evidence/'httpd')],cwd=root,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
    output=[]
    try:
        assert select.select([server.stdout],[],[],5)[0],'default startup did not flush'
        output.append(server.stdout.readline())
        assert 'port 8080' in output[-1],output
        status,body,_=exchange(8080,'/cgi-bin/echo?default=8080')
        assert status==200 and b'default=8080' in body
        assert select.select([server.stdout],[],[],5)[0],'default request log did not flush'
        output.append(server.stdout.readline())
        assert '[GET] [/cgi-bin/echo] [200]' in output[-1],output
    finally:
        server.terminate()
        rest,errors=server.communicate(timeout=5)
        output.append(rest)
        (evidence/'default-port.stdout').write_text(''.join(output))
        (evidence/'default-port.stderr').write_text(errors)
    assert not errors,errors
    (evidence/'default-port.json').write_text(json.dumps({'port':8080,'argv':[],'response_status':status,'log_flushed':True})+'\n')
for port in ['0','65536','not-a-port']:
    q=subprocess.run([str(evidence/'httpd'),port],capture_output=True,text=True,timeout=3)
    assert q.returncode==2,(port,q.returncode)
q=subprocess.run([str(evidence/'httpd'),'8080','extra'],capture_output=True,text=True,timeout=3)
assert q.returncode==2
(evidence/'results.json').write_text(json.dumps(all_results,indent=2)+'\n')
(evidence/'checks.result').write_text('PASS: normal and ASan/UBSan real CGI; full/header-only responses and statuses; invalid paths and child failures; fragmented headers/2MiB binary; four active plus fifth accepted/queued; fast completion with ordered flushed logs; 48 concurrent isolated environments each; descriptors restored and no child zombies; default 8080 real request; invalid ports and excess args rejected; original tests empty\n')
PY
