"""Local deployment test with disposable MySQL, real Nginx/TLS and a mock model.
Usage: python3 tests/test_deployment.py build-chat/chat_session_fixture /usr/sbin/mysqld /usr/sbin/nginx
No public server, domain, sudo, real provider or production database is used.
"""
import gzip
import http.client
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import time


def free_port():
    with socket.socket() as connection:
        connection.bind(('127.0.0.1',0))
        return connection.getsockname()[1]


def wait_for(process, port):
    for _ in range(200):
        if process.poll() is not None:
            raise RuntimeError(f'Process exited with {process.returncode}')
        try:
            with socket.create_connection(('127.0.0.1',port),timeout=.1):
                return
        except OSError:
            time.sleep(.05)
    raise RuntimeError('Process startup timed out')


class Model(http.server.BaseHTTPRequestHandler):
    def log_message(self,*_):
        pass
    def do_POST(self):
        body=json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        assert body['max_tokens']==1024
        self.send_response(200)
        self.send_header('Content-Type','text/event-stream')
        self.end_headers()
        for piece in ['一','二','三']:
            self.wfile.write(('data: '+json.dumps({'choices':[{'delta':{'content':piece}}]})+'\n\n').encode())
            self.wfile.flush()
            time.sleep(.35)
        self.wfile.write(b'data: [DONE]\n\n')


repo=Path(__file__).resolve().parents[1]
fixture,mysqld,nginx=map(lambda value:str(Path(value).resolve()),sys.argv[1:4])
with tempfile.TemporaryDirectory(prefix='cppai-deployment-') as temporary:
    root=Path(temporary)
    db_port,api_port,tls_port,http_port=[free_port() for _ in range(4)]
    data=root/'data';data.mkdir()
    initialized=subprocess.run([mysqld,'--no-defaults','--initialize-insecure','--datadir='+str(data)],capture_output=True,timeout=90)
    assert initialized.returncode==0,'Temporary database initialization failed'
    log=(root/'processes.log').open('wb')
    db=app=proxy=None
    model=http.server.ThreadingHTTPServer(('127.0.0.1',0),Model)
    threading.Thread(target=model.serve_forever,daemon=True).start()
    def database(sql, name='chat_feature_test'):
        return subprocess.check_output(['mysql','--protocol=TCP','-h','127.0.0.1','-P',str(db_port),'-u','root',
            '--batch','--skip-column-names',name],input=sql.encode()).decode().strip()
    try:
        db=subprocess.Popen([mysqld,'--no-defaults','--datadir='+str(data),'--port='+str(db_port),
            '--bind-address=127.0.0.1','--socket='+str(root/'mysql.sock'),'--pid-file='+str(root/'mysql.pid'),
            '--mysqlx=OFF','--secure-file-priv='+str(root),'--innodb-buffer-pool-size=32M'],stdout=log,stderr=log)
        wait_for(db,db_port)
        env={**os.environ,'CHAT_BIND_ADDRESS':'127.0.0.1','CHAT_PUBLIC_ORIGIN':'https://chat.example.test',
            'CHAT_TRUST_PROXY':'1','CHAT_REGISTRATIONS_IP_DAILY':'1','CHAT_MAX_OUTPUT_TOKENS':'1024'}
        app=subprocess.Popen([fixture,str(api_port),str(db_port),'http://127.0.0.1:'+str(model.server_port)],env=env,stdout=log,stderr=log)
        wait_for(app,api_port)
        output=root/'generated'
        subprocess.run([sys.executable,str(repo/'scripts/prepare_deployment.py'),'--domain','chat.example.test','--output',str(output)],check=True,stdout=subprocess.DEVNULL)
        subprocess.run(['openssl','req','-x509','-newkey','rsa:2048','-nodes','-days','1','-subj','/CN=chat.example.test',
            '-addext','subjectAltName=IP:127.0.0.1,DNS:chat.example.test','-keyout',str(root/'key.pem'),'-out',str(root/'cert.pem')],check=True,stdout=log,stderr=log)
        config=(output/'nginx.conf').read_text().replace('listen 80;',f'listen {http_port};').replace('listen 443 ssl;',f'listen {tls_port} ssl;')
        config=config.replace('127.0.0.1:8080','127.0.0.1:'+str(api_port))
        config=config.replace('/etc/letsencrypt/live/chat.example.test/fullchain.pem',str(root/'cert.pem')).replace('/etc/letsencrypt/live/chat.example.test/privkey.pem',str(root/'key.pem'))
        config=f'pid {root}/nginx.pid; error_log {root}/error.log; events {{ worker_connections 256; }} http {{ access_log off; client_body_temp_path {root}/body; proxy_temp_path {root}/proxy; fastcgi_temp_path {root}/fastcgi; uwsgi_temp_path {root}/uwsgi; scgi_temp_path {root}/scgi;\n'+config+'\n}\n'
        (root/'nginx.conf').write_text(config)
        validated=subprocess.run([nginx,'-t','-e',str(root/'error.log'),'-p',str(root),'-c',str(root/'nginx.conf')],capture_output=True)
        assert validated.returncode==0,validated.stderr.decode()
        proxy=subprocess.Popen([nginx,'-e',str(root/'error.log'),'-p',str(root),'-c',str(root/'nginx.conf'),'-g','daemon off;'],stdout=log,stderr=log)
        wait_for(proxy,tls_port)
        context=ssl.create_default_context(cafile=str(root/'cert.pem'))
        def request(path, body=None, cookie=None, extra=None):
            connection=http.client.HTTPSConnection('127.0.0.1',tls_port,context=context,timeout=10)
            headers={'Host':'chat.example.test','Origin':'https://chat.example.test','Content-Type':'application/json',**(extra or {})}
            if cookie:headers['Cookie']=cookie
            connection.request('POST' if body is not None else 'GET',path,json.dumps(body) if body is not None else None,headers)
            response=connection.getresponse()
            return connection,response
        c,r=request('/register',{'username':'tls_user','password':'a long test password for TLS'})
        assert r.status==200,(r.status,r.read());r.read();c.close()
        c,r=request('/register',{'username':'tls_user_two','password':'another long test password'},extra={'X-Real-IP':'198.51.100.4','X-Forwarded-For':'198.51.100.4'})
        assert r.status==429,(r.status,r.read());r.read();c.close()
        assert database("SELECT scope FROM usage_daily WHERE scope LIKE 'register-ip:%'")=='register-ip:127.0.0.1'
        c,r=request('/login',{'username':'tls_user','password':'a long test password for TLS'})
        assert r.status==200,(r.status,r.read())
        cookie=r.getheader('Set-Cookie');assert 'Secure' in cookie and 'HttpOnly' in cookie
        cookie=cookie.split(';')[0];r.read();c.close()
        started=time.monotonic()
        c,r=request('/chat/stream',{'question':'TLS stream','requestId':'tls-stream'},cookie)
        assert r.status==200 and r.getheader('Content-Type').startswith('text/event-stream')
        while r.readline().decode().strip()!='event: delta':
            assert time.monotonic()-started<3
        assert time.monotonic()-started<.8,'Proxy buffered the first generated text'
        tail=r.read().decode();assert 'event: done' in tail and time.monotonic()-started>1.0
        c.close()
        c,r=request('/.env');assert r.status==404;r.read();c.close()
        c,r=request('/healthz');assert r.status==404;r.read();c.close()
        c,r=request('/login',{},extra={'Origin':'https://evil.example'});assert r.status==403;r.read();c.close()
        # Dump with the script, then restore into another disposable schema.
        backup_env={**os.environ,'CHAT_MYSQL_URL':'tcp://127.0.0.1:'+str(db_port),'CHAT_MYSQL_USER':'root',
            'CHAT_MYSQL_PASSWORD':'','CHAT_MYSQL_DATABASE':'chat_feature_test','CHAT_BACKUP_DIR':str(root/'backups')}
        subprocess.run([sys.executable,str(repo/'scripts/backup_database.py')],env=backup_env,check=True,stdout=subprocess.DEVNULL)
        backups=list((root/'backups').glob('*.sql.gz'));assert len(backups)==1
        assert backups[0].stat().st_mode & 0o077 == 0
        database('CREATE DATABASE backup_restore CHARACTER SET utf8mb4')
        with gzip.open(backups[0],'rt') as dump:
            database(dump.read(),'backup_restore')
        for table in ['users','chat_sessions','chat_message','usage_daily']:
            assert database(f'SELECT COUNT(*) FROM {table}')==database(f'SELECT COUNT(*) FROM {table}','backup_restore')
        assert database('SELECT password FROM users')==database('SELECT password FROM users','backup_restore')
        print('deployment: real Nginx HTTPS, secure cookies, unbuffered SSE, proxy IP overwrite, CSRF, hidden private paths and database backup/restore passed')
    finally:
        for process in [proxy,app,db]:
            if process and process.poll() is None:
                process.terminate();process.wait(timeout=30)
        model.shutdown();log.close()
