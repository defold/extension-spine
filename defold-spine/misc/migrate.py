#!/usr/bin/env python3

import os, sys

EXTS=['.spinescene', '.gui']

def patch_line(line):
    if 'spine_json:' in line and '.json' in line:
        line = line.replace('.json', '.spinejson')
    return line

def patch_file(path):
    with open(path) as f:
        lines = f.readlines()

    newlines = [patch_line(x) for x in lines]

    if newlines != lines:

        with open(path, 'wt') as f:
            f.writelines(newlines)

        print("Patched", path)

if __name__ == '__main__':
    for root, dirs, files in os.walk('.'):
        for f in files:
            basename, ext = os.path.splitext(f)
            if not ext in EXTS:
                continue

            path = os.path.join(root, f)
            patch_file(path)
