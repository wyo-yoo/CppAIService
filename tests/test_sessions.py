"""Optional integration test: real MySQL, production API, local model, inline MQ stub.
Usage: python3 test_sessions.py build/chat_session_fixture /usr/sbin/mysqld
The database is initialized in a fresh temporary directory and listens on loopback.
"""
import http.client, http.server, json, os, pathlib, socket, subprocess, sys, tempfile, threading, time

def free_port():
    with socket.socket() as s: s.bind(('127.0.0.1',0)); return s.getsockname()[1]
class Model(http.server.BaseHTTPRequestHandler):
    def log_message(self,*_): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        question=body['messages'][-1]['content']
        try:
            if question=='fail': self.send_response(429);self.end_headers();return
            self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
            for piece in ['第一段。','第二段。','最后一段。']:
                self.wfile.write(('data: '+json.dumps({'choices':[{'delta':{'content':piece}}]},ensure_ascii=False)+'\n\n').encode());self.wfile.flush();time.sleep(.3)
            self.wfile.write(b'data: [DONE]\n\n');self.wfile.flush()
        except (BrokenPipeError,ConnectionResetError): pass

api_port, db_port=free_port(),free_port()
model=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model)
threading.Thread(target=model.serve_forever,daemon=True).start()
def api(path,body=None,cookie=None):
    connection=http.client.HTTPConnection('127.0.0.1',api_port,timeout=4)
    headers={'Content-Type':'application/json'}
    if cookie: headers['Cookie']=cookie
    connection.request('POST' if body is not None else 'GET',path,json.dumps(body) if body is not None else None,headers)
    response=connection.getresponse();status=response.status;new_cookie=response.getheader('Set-Cookie');data=json.loads(response.read())
    connection.close();return status,data,new_cookie.split(';')[0] if new_cookie else None
def event(response):
    name='message';data=[]
    while True:
        line=response.readline().decode().rstrip('\r\n')
        if not line:
            if data:return name,json.loads('\n'.join(data))
            raise RuntimeError('Unexpected stream EOF')
        if line.startswith('event:'):name=line[6:].strip()
        elif line.startswith('data:'):data.append(line[5:].lstrip())
def stream(cookie,request_id,sid='',question='你好'):
    c=http.client.HTTPConnection('127.0.0.1',api_port,timeout=4)
    c.request('POST','/chat/stream',json.dumps(dict(question=question,requestId=request_id,sessionId=sid,modelType='1')),
              {'Content-Type':'application/json','Cookie':cookie})
    r=c.getresponse();assert r.status==200,(r.status,r.read());return c,r
def drain(response):
    events=[]
    while True:
        e=event(response);events.append(e)
        if e[0] in ('done','error'):return events
with tempfile.TemporaryDirectory(prefix='cpp-chat-mysql-') as temp:
    root=pathlib.Path(temp);data=root/'data';data.mkdir()
    init=subprocess.run([sys.argv[2],'--no-defaults','--initialize-insecure','--datadir='+str(data)],capture_output=True,timeout=90)
    assert init.returncode==0,init.stderr.decode()
    logfile=open(root/'mysql.log','wb')
    db=subprocess.Popen([sys.argv[2],'--no-defaults','--datadir='+str(data),'--port='+str(db_port),
       '--bind-address=127.0.0.1','--socket='+str(root/'mysql.sock'),'--pid-file='+str(root/'mysql.pid'),
       '--mysqlx=OFF','--secure-file-priv='+str(root),'--innodb-buffer-pool-size=32M'],stdout=logfile,stderr=logfile)
    app=None
    def start_app():
        p=subprocess.Popen([sys.argv[1],str(api_port),str(db_port),'http://127.0.0.1:'+str(model.server_port)],stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
        for _ in range(150):
            if p.poll() is not None: raise RuntimeError(p.stderr.read().decode())
            try:
                with socket.create_connection(('127.0.0.1',api_port),timeout=.1):return p
            except OSError:time.sleep(.03)
        raise RuntimeError('app startup timeout')
    try:
        for _ in range(200):
            if db.poll() is not None:
                logfile.flush();raise RuntimeError((root/'mysql.log').read_text())
            try:
                with socket.create_connection(('127.0.0.1',db_port),timeout=.1):break
            except OSError:time.sleep(.05)
        else:
            logfile.flush();raise RuntimeError('database startup timeout: '+(root/'mysql.log').read_text())
        app=start_app()
        assert api('/chat/sessions')[0]==401
        assert api('/register',{'username':'alice','password':'test'})[0]==200
        assert api('/register',{'username':'bob','password':'test'})[0]==200
        _,_,alice=api('/login',{'username':'alice','password':'test'})
        _,_,bob=api('/login',{'username':'bob','password':'test'})
        c,r=stream(alice,'first');name,meta=event(r);assert name=='meta';sid=meta['sessionId']
        while event(r)[0]!='delta':pass
        start=time.monotonic();assert api('/chat/history',{'sessionId':sid},alice)[0]==200;assert time.monotonic()-start<.3
        assert api('/chat/cancel',{'requestId':'first'},bob)[0]==404
        assert api('/chat/sessions/delete',{'sessionId':sid},alice)[0]==409
        assert api('/chat/stream',{'question':'overlap','requestId':'duplicate','sessionId':sid},alice)[0]==409
        start=time.monotonic();assert api('/chat/cancel',{'requestId':'first'},alice)[0]==200
        events=drain(r);assert events[-1][0]=='done' and events[-1][1]['stopped'];assert time.monotonic()-start<1
        partial=events[-1][1]['text'];assert partial and partial!='第一段。第二段。最后一段。';c.close()
        assert api('/chat/sessions/rename',{'sessionId':sid,'name':'学习计划'},bob)[0]==404
        assert api('/chat/sessions/rename',{'sessionId':sid,'name':'学习计划'},alice)[0]==200
        c,r=stream(alice,'continue',sid);assert drain(r)[-1][1]['stopped'] is False;c.close()
        history=api('/chat/history',{'sessionId':sid},alice)[1]['history'];assert len(history)==4 and history[1]['content']==partial
        c,r=stream(alice,'failed',sid,'fail');assert drain(r)[-1][0]=='error';c.close()
        assert len(api('/chat/history',{'sessionId':sid},alice)[1]['history'])==4
        # Restart the C++ process so metadata/history must be restored from MySQL.
        app.terminate();app.wait(timeout=3);app=start_app()
        _,_,alice=api('/login',{'username':'alice','password':'test'})
        sessions=api('/chat/sessions',cookie=alice)[1]['sessions'];assert sessions==[{'sessionId':sid,'name':'学习计划'}],sessions
        assert api('/chat/history',{'sessionId':sid},alice)[1]['history']==history
        assert api('/chat/sessions/delete',{'sessionId':sid},alice)[0]==200
        assert api('/chat/history',{'sessionId':sid},alice)[0]==404
        app.terminate();app.wait(timeout=3);app=start_app()
        _,_,alice=api('/login',{'username':'alice','password':'test'})
        assert api('/chat/sessions',cookie=alice)[1]['sessions']==[]
        assert api('/chat/stream',{'question':'revive','requestId':'bad','sessionId':sid},alice)[0]==404
        print('sessions: auth/isolation, streaming, stop/continue, failure rollback, rename/history restart and durable deletion passed')
    finally:
        if app:app.terminate();app.wait(timeout=3)
        db.terminate();db.wait(timeout=30);logfile.close();model.shutdown()
