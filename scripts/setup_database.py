#!/usr/bin/env python3
"""Initialize a local Ubuntu MySQL app account without changing root or existing data."""
import os
from pathlib import Path
import re
import secrets
import shlex
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parents[1]
    env_file = root / '.env'
    config = {}
    if env_file.exists():
        for raw in env_file.read_text().splitlines():
            line = raw.strip()
            if not line or line.startswith('#'):
                continue
            key, sep, value = line.removeprefix('export ').partition('=')
            if sep and key.startswith('CHAT_MYSQL_'):
                tokens = shlex.split(value, comments=True)
                if len(tokens) > 1:
                    raise ValueError(f'Quote the value of {key} in .env')
                config[key] = tokens[0] if tokens else ''
    else:
        config = {
            'CHAT_MYSQL_URL': 'tcp://127.0.0.1:3306',
            'CHAT_MYSQL_USER': 'cppaiservice',
            'CHAT_MYSQL_PASSWORD': secrets.token_hex(24),
            'CHAT_MYSQL_DATABASE': 'ChatHttpServer',
        }
    if config.get('CHAT_MYSQL_URL') != 'tcp://127.0.0.1:3306':
        raise ValueError('This setup script is for local MySQL on 127.0.0.1:3306 only.')
    user = config.get('CHAT_MYSQL_USER', '')
    database = config.get('CHAT_MYSQL_DATABASE', '')
    password = config.get('CHAT_MYSQL_PASSWORD', '')
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]{0,31}', user) or user == 'root':
        raise ValueError('Use a separate app username with letters, numbers and underscores.')
    if not re.fullmatch(r'[A-Za-z][A-Za-z0-9_]{0,63}', database):
        raise ValueError('Use a database name with letters, numbers and underscores.')
    if not password or any(c in password for c in '\r\n\x00'):
        raise ValueError('Set CHAT_MYSQL_PASSWORD in .env, or let the script create a new .env.')

    # Ask before writing credentials or changing MySQL. The password is entered in sudo's terminal.
    subprocess.run(['sudo', '-v'], check=True)
    if not env_file.exists():
        content = '# Local configuration; excluded from Git.\n'
        content += ''.join(f'{key}={shlex.quote(value)}\n' for key, value in config.items())
        content += '\nDEEPSEEK_API_KEY=\nDEEPSEEK_MODEL=deepseek-flash\nDASHSCOPE_API_KEY=\nDOUBAO_API_KEY=\nKnowledge_Base_ID=\nBAIDU_CLIENT_ID=\nBAIDU_CLIENT_SECRET=\n'
        descriptor = os.open(env_file, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(descriptor, 'w') as output:
            output.write(content)
    # Standard SQL escaping with NO_BACKSLASH_ESCAPES avoids interpreting password characters.
    escaped_password = password.replace("'", "''")
    sql = (
        "SET SESSION sql_mode='NO_BACKSLASH_ESCAPES';\n"
        f'CREATE DATABASE IF NOT EXISTS `{database}` CHARACTER SET utf8mb4;\n'
        f"CREATE USER IF NOT EXISTS '{user}'@'localhost' IDENTIFIED BY '{escaped_password}';\n"
        f"GRANT SELECT, INSERT, UPDATE, DELETE, CREATE, INDEX ON `{database}`.* TO '{user}'@'localhost';\n"
    )
    result = subprocess.run(['sudo', 'mysql', '--batch'], input=sql, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(result.stderr.replace(password, '<redacted>').replace(escaped_password, '<redacted>'))
    result = subprocess.run(
        ['mysql', '--protocol=TCP', '-h', '127.0.0.1', '-u', user, database],
        input=(root / 'scripts/schema.sql').read_text(), text=True, capture_output=True,
        env={**os.environ, 'MYSQL_PWD': password})
    if result.returncode:
        raise RuntimeError(result.stderr.replace(password, '<redacted>') +
                           '\nExisting account passwords are not changed. Check .env if the account already existed.')
    print('Database, app account and tables are ready. Local credentials are in .env (excluded from Git).')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'Setup failed: {error}', file=sys.stderr)
        sys.exit(1)
