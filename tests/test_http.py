"""Use real sockets to verify SSE framing, responsiveness and disconnect cleanup."""
import http.client, socket, subprocess, sys, time
with socket.socket() as probe:
    probe.bind(('127.0.0.1',0)); port=probe.getsockname()[1]
process=subprocess.Popen([sys.argv[1],str(port)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
try:
    for _ in range(100):
        try:
            with socket.create_connection(('127.0.0.1',port),timeout=.1): break
        except OSError: time.sleep(.03)
    # Removed or unknown routes must have a valid HTTP status line, not a protocol error.
    for method, path in [('GET', '/missing-page'), ('POST', '/missing-endpoint')]:
        missing=http.client.HTTPConnection('127.0.0.1',port,timeout=3)
        missing.request(method,path)
        response=missing.getresponse()
        assert response.status==404
        assert response.version==11
        assert response.getheader('Content-Length')=='0'
        assert response.read()==b''
        missing.close()
    conn=http.client.HTTPConnection('127.0.0.1',port,timeout=3)
    start=time.monotonic(); conn.request('POST','/stream',body='{}',headers={'Content-Type':'application/json'})
    response=conn.getresponse()
    assert response.status==200
    assert response.getheader('Content-Type').startswith('text/event-stream')
    assert response.getheader('Content-Length') is None
    assert response.getheader('Connection').lower()=='close'
    assert response.readline()==b'event: meta\n'
    assert time.monotonic()-start < .5
    health=http.client.HTTPConnection('127.0.0.1',port,timeout=1)
    start=time.monotonic(); health.request('GET','/health'); assert health.getresponse().read()==b'0'
    assert time.monotonic()-start < .3
    rest=response.read().decode(); assert rest.count('event: delta')==20 and 'event: done' in rest
    conn.close(); health.close()
    raw=socket.create_connection(('127.0.0.1',port))
    raw.sendall(b'POST /stream HTTP/1.1\r\nhost: localhost\r\ncontent-length: 2\r\n\r\n{}')
    raw.recv(4096); raw.close(); time.sleep(.3)
    health=http.client.HTTPConnection('127.0.0.1',port,timeout=1)
    health.request('GET','/health'); assert int(health.getresponse().read())>=1
    print('HTTP: first event, complete framing, concurrent health request and disconnect cancellation passed')
finally:
    process.terminate(); process.wait(timeout=3)
