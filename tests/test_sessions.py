"""Optional integration test: real MySQL, production API, local model, inline MQ stub.
Usage: python3 test_sessions.py build/chat_session_fixture /usr/sbin/mysqld
The database is initialized in a fresh temporary directory and listens on loopback.
"""
import concurrent.futures, hashlib, re
import http.client, http.server, json, os, pathlib, socket, subprocess, sys, tempfile, threading, time

def free_port():
    with socket.socket() as s: s.bind(('127.0.0.1',0)); return s.getsockname()[1]
class Model(http.server.BaseHTTPRequestHandler):
    def log_message(self,*_): pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        question=body['messages'][-1]['content']
        assert 64 <= body['max_tokens'] <= 8192, body
        assert len(body['messages']) <= 21, body
        if question=='你好 DeepSeek':
            assert body['model']=='deepseek-flash' and body['thinking']['type']=='disabled'
        try:
            if question=='fail': self.send_response(429);self.end_headers();return
            if not body.get('stream'):
                answer=json.dumps({'choices':[{'message':{'content':'第一段。第二段。最后一段。'}}]},ensure_ascii=False).encode()
                self.send_response(200);self.send_header('Content-Type','application/json');self.send_header('Content-Length',str(len(answer)));self.end_headers()
                self.wfile.write(answer);return
            self.send_response(200);self.send_header('Content-Type','text/event-stream');self.end_headers()
            for piece in ['第一段。','第二段。','最后一段。']:
                self.wfile.write(('data: '+json.dumps({'choices':[{'delta':{'content':piece}}]},ensure_ascii=False)+'\n\n').encode());self.wfile.flush();time.sleep(.3)
            self.wfile.write(b'data: [DONE]\n\n');self.wfile.flush()
        except (BrokenPipeError,ConnectionResetError): pass

api_port, db_port=free_port(),free_port()
model=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model)
threading.Thread(target=model.serve_forever,daemon=True).start()
def api(path,body=None,cookie=None):
    connection=http.client.HTTPConnection('127.0.0.1',api_port,timeout=10)
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
def stream(cookie,request_id,sid='',question='你好',model='1'):
    c=http.client.HTTPConnection('127.0.0.1',api_port,timeout=10)
    payload=dict(question=question,requestId=request_id,sessionId=sid)
    if model is not None: payload['modelType']=model
    c.request('POST','/chat/stream',json.dumps(payload),
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
    def start_app(extra=None):
        config={**os.environ,'CHAT_ALLOWED_MODELS':'1,5','CHAT_USER_DAILY_LIMIT':'100','CHAT_SITE_DAILY_LIMIT':'1000',
            'CHAT_USER_PER_MINUTE':'120','CHAT_REGISTRATIONS_IP_DAILY':'20','CHAT_MAX_CONCURRENT':'4',
            'CHAT_REGISTRATION_OPEN':'1','CHAT_PUBLIC_ORIGIN':'','CHAT_TRUST_PROXY':'0','CHAT_BIND_ADDRESS':'127.0.0.1'}
        config.update(extra or {})
        p=subprocess.Popen([sys.argv[1],str(api_port),str(db_port),'http://127.0.0.1:'+str(model.server_port)],env=config,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
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
        assert api('/register',{'username':'alice','password':'test passphrase for accounts'})[0]==200
        assert api('/register',{'username':'bob','password':'test passphrase for accounts'})[0]==200
        _,_,alice=api('/login',{'username':'alice','password':'test passphrase for accounts'})
        _,_,bob=api('/login',{'username':'bob','password':'test passphrase for accounts'})
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
        _,_,alice=api('/login',{'username':'alice','password':'test passphrase for accounts'})
        sessions=api('/chat/sessions',cookie=alice)[1]['sessions'];assert sessions==[{'sessionId':sid,'name':'学习计划'}],sessions
        assert api('/chat/history',{'sessionId':sid},alice)[1]['history']==history
        assert api('/chat/sessions/delete',{'sessionId':sid},alice)[0]==200
        assert api('/chat/history',{'sessionId':sid},alice)[0]==404
        app.terminate();app.wait(timeout=3);app=start_app()
        _,_,alice=api('/login',{'username':'alice','password':'test passphrase for accounts'})
        assert api('/chat/sessions',cookie=alice)[1]['sessions']==[]
        assert api('/chat/stream',{'question':'revive','requestId':'bad','sessionId':sid},alice)[0]==404
        # With modelType omitted, the production route must select DeepSeek.
        c,r=stream(alice,'deepseek-default',question='你好 DeepSeek',model=None)
        events=drain(r);c.close()
        assert events[0][0]=='meta' and events[-1][0]=='done',events
        assert events[-1][1]['text']=='第一段。第二段。最后一段。',events
        deepseek_sid=events[0][1]['sessionId']
        assert len(api('/chat/history',{'sessionId':deepseek_sid},alice)[1]['history'])==2

        # Inspect only this disposable database, never real application credentials.
        def sql(text):
            return subprocess.check_output(['mysql','--protocol=TCP','-h','127.0.0.1','-P',str(db_port),'-u','root',
                '--batch','--skip-column-names','chat_feature_test'],input=text.encode()).decode().strip()
        def raw_api(path,body=None,cookie=None,headers=None):
            c=http.client.HTTPConnection('127.0.0.1',api_port,timeout=10)
            h={'Content-Type':'application/json',**(headers or {})}
            if cookie:h['Cookie']=cookie
            c.request('POST' if body is not None else 'GET',path,json.dumps(body) if body is not None else None,h)
            r=c.getresponse();result=(r.status,r.read(),dict(r.getheaders()));c.close();return result
        def restart(extra=None,reset=True):
            global app
            app.terminate();app.wait(timeout=5)
            if reset:sql('DELETE FROM usage_daily')
            app=start_app(extra)
        def login(name='alice'):
            status,_,cookie=api('/login',{'username':name,'password':'test passphrase for accounts'})
            assert status==200,status
            return cookie

        # Cross-language verification of the actual stored format and work factor.
        encoded=sql("SELECT password FROM users WHERE username='alice'")
        algorithm,iterations,salt,digest=encoded.split('$')
        assert algorithm=='pbkdf2_sha256' and int(iterations)==600000
        assert hashlib.pbkdf2_hmac('sha256',b'test passphrase for accounts',bytes.fromhex(salt),600000).hex()==digest
        assert encoded != sql("SELECT password FROM users WHERE username='bob'")
        assert api('/register',{'username':'short','password':'weak'})[0]==400
        assert api('/register',{'username':'<script>','password':'test passphrase for accounts'})[0]==400
        assert api('/register',{'username':'alice','password':'test passphrase for accounts'})[0]==409
        assert api('/register',{'username':12,'password':False})[0]==400
        assert raw_api('/login',{},headers={'Content-Type':'text/plain'})[0]==415
        assert raw_api('/login',{},headers={'Origin':'https://evil.example'})[0]==403
        assert raw_api('/user/logout',{},alice,{'Origin':'null'})[0]==403
        # Rotating one browser's session invalidates its old token; independent devices still work.
        alice=login()
        status,_,new_cookie=api('/login',{'username':'alice','password':'test passphrase for accounts'},alice)
        assert status==200 and new_cookie != alice
        assert api('/chat/usage',cookie=alice)[0]==401
        alice=new_cookie
        other=login()
        assert api('/chat/usage',cookie=alice)[0]==200 and api('/chat/usage',cookie=other)[0]==200
        assert api('/user/logout',{},alice)[0]==200
        assert api('/chat/usage',cookie=alice)[0]==401
        assert api('/chat/usage',cookie=other)[0]==200
        for _ in range(10):assert api('/login',{'username':'unknown','password':'wrong'})[0]==401
        status,_,headers=raw_api('/login',{'username':'unknown','password':'wrong'})
        assert status==429 and int(headers['Retry-After'])>0

        # Upgrade an existing plaintext account before opening the listener.
        sql("INSERT INTO users(username,password) VALUES ('legacy','old-password')")
        restart()
        assert api('/login',{'username':'legacy','password':'old-password'})[0]==200
        assert sql("SELECT password FROM users WHERE username='legacy'").startswith('pbkdf2_sha256$600000$')

        # JSON legacy endpoints and the SSE endpoint consume one shared durable quota.
        limits={'CHAT_USER_DAILY_LIMIT':'2','CHAT_SITE_DAILY_LIMIT':'10'}
        restart(limits);alice=login()
        result=api('/chat/send-new-session',{'question':'legacy','modelType':'5'},alice)
        assert result[0]==200 and result[1]['Information']=='第一段。第二段。最后一段。',result
        legacy_sid=result[1]['sessionId']
        c,r=stream(alice,'quota-second',legacy_sid,model=None);assert drain(r)[-1][0]=='done';c.close()
        assert api('/chat/usage',cookie=alice)[1]['remaining']==0
        for endpoint,body in [('/chat/stream',{'question':'extra','requestId':'extra'}),
                              ('/chat/send',{'question':'extra','sessionId':legacy_sid}),
                              ('/chat/send-new-session',{'question':'extra'})]:
            assert raw_api(endpoint,body,alice,{'X-Forwarded-For':'198.51.100.9','X-Real-IP':'198.51.100.9'})[0]==429
        restart(limits,reset=False);alice=login()
        assert api('/chat/usage',cookie=alice)[1]['remaining']==0
        assert api('/chat/stream',{'question':'restart bypass','requestId':'restart-bypass'},alice)[0]==429
        sql('UPDATE usage_daily SET day=UTC_DATE()-INTERVAL 1 DAY')
        c,r=stream(alice,'new-day',model=None);assert drain(r)[-1][0]=='done';c.close()

        # Simultaneous users cannot both spend the final site-wide reservation.
        restart({'CHAT_SITE_DAILY_LIMIT':'1','CHAT_USER_DAILY_LIMIT':'10'});alice=login();bob=login('bob')
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures=[pool.submit(raw_api,'/chat/stream',{'question':'budget race','requestId':'race-'+str(i)},cookie)
                     for i,cookie in enumerate([alice,bob])]
            statuses=sorted(f.result()[0] for f in futures)
        assert statuses==[200,429],statuses
        assert sql("SELECT used FROM usage_daily WHERE day=UTC_DATE() AND scope='chat-site'")=='1'
        assert sql("SELECT SUM(used) FROM usage_daily WHERE day=UTC_DATE() AND scope LIKE 'chat-user:%'")=='1'

        # Concurrency is shared across legacy and streaming paths and released on completion.
        restart({'CHAT_MAX_CONCURRENT':'1'});alice=login();bob=login('bob')
        c,r=stream(alice,'concurrency');event(r)
        assert api('/chat/send-new-session',{'question':'busy'},bob)[0]==503
        assert api('/chat/stream',{'question':'overlap','requestId':'same-user'},alice)[0]==409
        drain(r);c.close()
        assert api('/chat/send-new-session',{'question':'now available'},bob)[0]==200
        assert api('/chat/tts',{'text':'disabled'},alice)[0]==403

        restart({'CHAT_USER_PER_MINUTE':'1'});alice=login()
        c,r=stream(alice,'minute-one');drain(r);c.close()
        assert api('/chat/send-new-session',{'question':'too fast'},alice)[0]==429
        restart({'CHAT_REGISTRATION_OPEN':'0'})
        assert api('/api/config')[1]['registrationOpen'] is False
        assert api('/register',{'username':'closed','password':'test passphrase for accounts'})[0]==403
        restart({'CHAT_REGISTRATIONS_IP_DAILY':'1'})
        assert api('/register',{'username':'new_one','password':'test passphrase for accounts'})[0]==200
        assert raw_api('/register',{'username':'new_two','password':'test passphrase for accounts'},
            headers={'X-Real-IP':'198.51.100.99','X-Forwarded-For':'198.51.100.99'})[0]==429

        # Public HTTPS mode sets secure cookies and accepts only the configured browser origin.
        restart({'CHAT_PUBLIC_ORIGIN':'https://chat.example.test','CHAT_ALLOWED_MODELS':'5'})
        status,_,headers=raw_api('/login',{'username':'alice','password':'test passphrase for accounts'},
            headers={'Origin':'https://chat.example.test'})
        assert status==200 and 'Secure' in headers['Set-Cookie'] and 'HttpOnly' in headers['Set-Cookie'] and 'SameSite=Lax' in headers['Set-Cookie']
        assert raw_api('/login',{},headers={'Origin':'http://chat.example.test'})[0]==403
        alice=login()
        assert api('/chat/stream',{'question':'unavailable model','requestId':'blocked-model','modelType':'4'},alice)[0]==400
        assert api('/api/config')[1]['models']==['5']
        print('sessions/public access: isolation, password migration, cookie rotation, CSRF, auth throttling, durable quotas, atomic site budget, concurrency, legacy endpoints and public settings passed')
    finally:
        if app:app.terminate();app.wait(timeout=3)
        db.terminate();db.wait(timeout=30);logfile.close();model.shutdown()
