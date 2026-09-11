#!/usr/bin/env python3
"""Render deployment files locally; does not install, publish or request certificates."""
import argparse
import os
from pathlib import Path
import pwd
import re


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--domain', required=True)
    parser.add_argument('--user', default=pwd.getpwuid(os.getuid()).pw_name)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if not re.fullmatch(r'(?=.{1,253}$)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?\.)+[A-Za-z]{2,63}', args.domain):
        parser.error('Use a domain name such as chat.example.com, without https:// or a path.')
    account = pwd.getpwnam(args.user)
    if account.pw_uid == 0 or not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_-]*', args.user):
        parser.error('Use an existing non-root Linux service account.')
    root = Path(__file__).resolve().parents[1]
    if not re.fullmatch(r'/[A-Za-z0-9_./-]+', str(root)):
        parser.error('For these service templates, use a project path without spaces or special characters.')
    output = args.output or root / '.deployment'
    output.mkdir(parents=True, exist_ok=True, mode=0o700)
    values = {'@DOMAIN@': args.domain.lower(), '@PROJECT@': str(root), '@USER@': args.user}
    for source in (root/'deploy').iterdir():
        text = source.read_text()
        for token, value in values.items():
            text = text.replace(token, value)
        (output / source.name.removesuffix('.in')).write_text(text)
    (output / 'public.env').write_text(
        '# Merge these non-secret settings into .env; preserve database and provider credentials.\n'
        'CHAT_BIND_ADDRESS=127.0.0.1\nCHAT_TRUST_PROXY=1\n'
        f'CHAT_PUBLIC_ORIGIN=https://{args.domain.lower()}\n'
        'CHAT_ALLOWED_MODELS=5\nCHAT_ENABLE_TTS=0\nCHAT_REGISTRATION_OPEN=1\n')
    print(f'Deployment files prepared in {output}. Nothing has been installed or published.')


if __name__ == '__main__':
    main()
