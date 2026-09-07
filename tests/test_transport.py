"""Exercise the real libcurl transport against a deterministic local upstream."""
import http.server, json, subprocess, sys, threading, time

class Upstream(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_): pass
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        try:
            if self.path == '/error':
                self.send_response(429); self.end_headers(); self.wfile.write(b'{"error":"limited"}'); return
            self.send_response(200); self.send_header('Content-Type', 'text/event-stream'); self.end_headers()
            if self.path == '/silent': time.sleep(2); return
            if self.path == '/plain': self.wfile.write(b'{"choices":[{"message":{"content":"plain"}}]}'); return
            if self.path == '/malformed': self.wfile.write(b'data: {invalid}\n\n'); return
            if self.path == '/rag':
                assert self.headers.get('X-DashScope-SSE') == 'enable'
                assert body['parameters']['incremental_output'] is True
                values = [{'output': {'text': x, 'finish_reason': 'null'}} for x in ['你', '好']]
                values += [{'output': {'text': '', 'finish_reason': 'stop'}}]
            else:
                assert body['stream'] is True
                values = [{'choices':[{'delta':{'content':x}}]} for x in ['你','好']]
                if self.path != '/truncated': values += ['[DONE]']
            for value in values:
                data = value if isinstance(value,str) else json.dumps(value,ensure_ascii=False)
                wire = ('data: '+data+'\r\n\r\n').encode()
                for start in range(0, len(wire), 3): self.wfile.write(wire[start:start+3]); self.wfile.flush()
                time.sleep(.2)
        except (BrokenPipeError, ConnectionResetError): pass

server = http.server.ThreadingHTTPServer(('127.0.0.1',0), Upstream)
threading.Thread(target=server.serve_forever,daemon=True).start()
base = 'http://127.0.0.1:'+str(server.server_port)
def run(path, mode='stream'):
    return subprocess.run([sys.argv[1],base+path,mode],capture_output=True,text=True,timeout=5)
for path,mode in [('/stream','stream'),('/rag','rag')]:
    p=run(path,mode); assert p.returncode==0,(path,p.stderr)
    lines=[json.loads(x) for x in p.stdout.splitlines()]
    assert ''.join(x['delta'] for x in lines if 'delta' in x)=='你好',lines
    assert lines[0]['elapsedMs'] < 200,lines
    assert lines[-1]['count']==2,lines
for path in ['/error','malformed','truncated']:
    p=run('/'+path.lstrip('/')); assert p.returncode==1,(path,p.stdout,p.stderr)
start=time.monotonic(); p=run('/silent','cancel')
assert p.returncode==2 and time.monotonic()-start < 1.2,(p.stdout,p.stderr)
p=run('/plain','plain'); assert p.returncode==0,p.stderr
server.shutdown()
print('transport: streamed first token, UTF-8 fragments, RAG, upstream errors and prompt cancellation passed')
