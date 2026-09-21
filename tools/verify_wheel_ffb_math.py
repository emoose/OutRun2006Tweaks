"""Compile and execute the current production estimator/curve, not a Python copy."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parent.parent
compiler = os.environ.get('CXX') or shutil.which('c++') or shutil.which('g++') or shutil.which('clang++') or shutil.which('cl')
if not compiler:
    raise SystemExit('C++ compiler required: run in VS Developer Prompt or set CXX')
with tempfile.TemporaryDirectory(prefix='wheel-ffb-tests-') as tmp:
    exe = Path(tmp) / ('test.exe' if os.name == 'nt' else 'test')
    source = ROOT / 'tools/test_wheel_ffb_current.cpp'
    if Path(compiler).name.lower() in ('cl', 'cl.exe'):
        args = [compiler, '/nologo', '/EHsc', '/std:c++20', '/utf-8', '/I'+str(ROOT/'src'), str(source), '/Fe:'+str(exe)]
    else:
        args = [compiler, '-std=c++20', '-Wall', '-Wextra', '-pedantic', '-I'+str(ROOT/'src'), str(source), '-o', str(exe)]
    subprocess.run(args, check=True, cwd=tmp)
    subprocess.run([str(exe)], check=True)
