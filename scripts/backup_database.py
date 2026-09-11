#!/usr/bin/env python3
"""Atomic, private MySQL dump. Defaults to the ignored .backups directory; retains 14 days."""
import datetime
import gzip
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile
import time
from urllib.parse import urlparse


def main():
    root = Path(__file__).resolve().parents[1]
    config = {}
    if (root/'.env').exists():
        for raw in (root/'.env').read_text().splitlines():
            key, sep, value = raw.strip().removeprefix('export ').partition('=')
            if sep and key.startswith('CHAT_'):
                tokens = shlex.split(value, comments=True)
                if len(tokens) > 1:
                    raise ValueError(f'Invalid quoting for {key}')
                config[key] = tokens[0] if tokens else ''
    config.update(os.environ)
    address = urlparse(config.get('CHAT_MYSQL_URL', 'tcp://127.0.0.1:3306'))
    database = config.get('CHAT_MYSQL_DATABASE', 'ChatHttpServer')
    if address.scheme != 'tcp' or not address.hostname or not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]{0,63}', database):
        raise ValueError('Invalid database address or name')
    target = Path(config.get('CHAT_BACKUP_DIR', str(root/'.backups'))).resolve()
    target.mkdir(mode=0o700, parents=True, exist_ok=True)
    target.chmod(0o700)
    retention = int(config.get('CHAT_BACKUP_RETENTION_DAYS', '14'))
    if not 1 <= retention <= 365:
        raise ValueError('CHAT_BACKUP_RETENTION_DAYS must be between 1 and 365')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%S%fZ')
    final = target / f'cppaiservice-{database}-{stamp}.sql.gz'
    command = ['mysqldump', '--protocol=TCP', '--host='+address.hostname, '--port='+str(address.port or 3306),
        '--user='+config.get('CHAT_MYSQL_USER','cppaiservice'), '--single-transaction', '--skip-lock-tables',
        '--no-tablespaces', '--skip-triggers', '--set-gtid-purged=OFF', '--hex-blob', '--default-character-set=utf8mb4', database]
    child_env = {**os.environ, 'MYSQL_PWD': config.get('CHAT_MYSQL_PASSWORD','')}
    fd, name = tempfile.mkstemp(prefix='.pending-',dir=target)
    pending = Path(name)
    process = None
    try:
        with os.fdopen(fd,'wb') as file, gzip.GzipFile(fileobj=file,mode='wb') as output, tempfile.TemporaryFile() as errors:
            process = subprocess.Popen(command,env=child_env,stdout=subprocess.PIPE,stderr=errors)
            for chunk in iter(lambda: process.stdout.read(1024*1024),b''):
                output.write(chunk)
            process.stdout.close()
            if process.wait() != 0:
                # Do not print stderr: database errors can include credentials or user data.
                raise RuntimeError('mysqldump failed; verify database connectivity and backup permissions')
        with pending.open('rb') as file:
            os.fsync(file.fileno())
        pending.replace(final)
        cutoff = time.time() - retention * 86400
        for old in target.glob(f'cppaiservice-{database}-*.sql.gz'):
            if old.is_file() and not old.is_symlink() and old.stat().st_mtime < cutoff:
                old.unlink()
        print(f'Private database backup ready: {final}')
    finally:
        if process and process.poll() is None:
            process.kill(); process.wait()
        if pending.exists():
            pending.unlink()


if __name__ == '__main__':
    main()
