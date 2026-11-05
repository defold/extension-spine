#!/usr/bin/env python3
"""Compile the actual cache/dispatch functions with deterministic engine test doubles."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def section(source, begin, end):
    return source[source.index(begin):source.index(end, source.index(begin))]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cxx', default=os.environ.get('CXX', 'g++'))
    parser.add_argument('--source', type=Path, default=ROOT / 'defold-spine/src/comp_spine_model.cpp')
    args = parser.parse_args()
    compiler = shutil.which(args.cxx)
    if not compiler:
        parser.error('C++ compiler not found; set CXX or pass --cxx')

    source = args.source.read_text(encoding='utf-8')
    vertices = (ROOT / 'defold-spine/include/common/vertices.h').read_text(encoding='utf-8')
    # Compile verbatim production code, including the real BEGIN/BATCH/END call site.
    # No cache algorithm is reimplemented in the test harness.
    parts = [
        section(vertices, 'struct SpineVertex', 'uint32_t CalcVertexBufferSize'),
        section(source, '    struct SpinePrefixEntry', '    struct SpineModelContext'),
        section(source, '    static inline uint32_t GetIndexTypeSize', '    // Render object submission'),
        section(source, '    static void InitRenderObject' if '    static void InitRenderObject' in source else '    static void FillRenderObject', '    template <typename T>\n    static uint32_t ExtendCacheArray'),
        section(source, '    template <typename T>\n    static uint32_t ExtendCacheArray', '    static void RenderListFrustumCulling'),
        section(source, '    static void RenderListDispatch(', '    dmGameObject::UpdateResult CompSpineModelRender'),
    ]
    harness = (Path(__file__).parent / 'test_geometry_cache.cpp').read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='spine-cache-') as directory:
        directory = Path(directory)
        (directory / 'cache_types.inc').write_text(parts[0] + parts[1], encoding='utf-8')
        (directory / 'cache_functions.inc').write_text('\n'.join(parts[2:]), encoding='utf-8')
        cpp = directory / 'test.cpp'
        cpp.write_text(harness, encoding='utf-8')
        executable = directory / ('test.exe' if os.name == 'nt' else 'test')
        subprocess.run([compiler, '-std=c++11', '-O1', '-g', '-Wall', '-Wextra',
                        '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-sign-compare',
                        '-static-libgcc', '-static-libstdc++', str(cpp), '-o', str(executable)], check=True)
        return subprocess.run([str(executable)], check=False).returncode


if __name__ == '__main__':
    raise SystemExit(main())
