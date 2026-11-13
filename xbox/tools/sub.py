#!/usr/bin/env python3
"""Byte-preserving search/replace for files with mixed CRLF/LF line endings.

usage: python tools/sub.py <file> < edits
edits: blocks of
<<<<
old text
====
new text
>>>>
Each old text must occur exactly once (tried as written, then with CRLF). New text takes the matched block's line ending.
"""
import sys


def main():
    path = sys.argv[1]
    data = open(path, 'rb').read()
    spec = sys.stdin.read().replace('\r\n', '\n')
    blocks = spec.split('<<<<\n')[1:]
    for blk in blocks:
        old, rest = blk.split('\n====\n', 1)
        new = rest.rsplit('\n>>>>', 1)[0]
        done = False
        for eol in (b'\n', b'\r\n'):
            o = old.encode('utf-8').replace(b'\n', eol)
            n = new.encode('utf-8').replace(b'\n', eol)
            c = data.count(o)
            if c == 1:
                data = data.replace(o, n)
                done = True
                break
            if c > 1:
                sys.exit(f'{path}: old text occurs {c} times:\n{old[:200]}')
        if not done:
            sys.exit(f'{path}: old text not found:\n{old[:300]}')
    open(path, 'wb').write(data)
    print(f'{path}: {len(blocks)} edit(s)')


if __name__ == '__main__':
    main()
