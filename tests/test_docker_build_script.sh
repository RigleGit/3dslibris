#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
bash -n "$ROOT/scripts/build_docker_release.sh"
python3 - "$ROOT" <<'PY'
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

source = Path(sys.argv[1]) / 'scripts/build_docker_release.sh'
# These files are the local CLI's release contract, independently of its help text.
artifacts = ['3dslibris.cia', '3dslibris-debug.cia', '3dslibris.3dsx',
             '3dslibris-debug.3dsx', '3dslibris.smdh', '3dslibris.elf',
             'dist/3dslibris-sdmc.zip', 'dist/3dslibris-source.tar.gz',
             'dist/romfs/3ds/3dslibris/font/LiberationSerif-Regular.ttf',
             'dist/romfs/3ds/3dslibris/resources/3DSLibris_dark_small.jpg',
             'dist/romfs/3ds/3dslibris/resources/3DSLibris_light_small.jpg']


def run(args=(), fail='', missing='', overrides=None):
    with tempfile.TemporaryDirectory(prefix='3dslibris-docker-test-') as work:
        root = Path(work)
        (root / 'scripts').mkdir()
        shutil.copyfile(source, root / 'scripts/build_docker_release.sh')
        tools = root / 'bin'
        tools.mkdir()
        docker = tools / 'docker'
        docker.write_text('''#!/usr/bin/env python3
import json, os, pathlib, sys
args = sys.argv[1:]
with open(os.environ['CALLS'], 'a') as f:
    f.write(json.dumps(args) + '\\n')
if args[0] == os.environ['FAIL_COMMAND']:
    sys.exit(19)
if args[0] == 'run':
    root = pathlib.Path(os.environ['OUTPUT_ROOT'])
    for name in json.loads(os.environ['ARTIFACTS']):
        if name == os.environ['MISSING']:
            continue
        p = root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.touch()
''')
        docker.chmod(0o755)
        env = dict(os.environ)
        env.update(PATH=str(tools) + os.pathsep + env['PATH'], CALLS=str(root/'calls'),
                   FAIL_COMMAND=fail, MISSING=missing, OUTPUT_ROOT=str(root),
                   ARTIFACTS=json.dumps(artifacts), IMAGE_TAG='fixture-image',
                   PLATFORM='linux/amd64', JOBS='2', DEVKITPRO='/fixture/pro',
                   DEVKITARM='/fixture/arm')
        env.update(overrides or {})
        result = subprocess.run(['bash', str(root/'scripts/build_docker_release.sh'),
                                 *args], env=env, capture_output=True, text=True)
        calls_path = root / 'calls'
        calls = [json.loads(s) for s in calls_path.read_text().splitlines()] if calls_path.exists() else []
        return result, calls, root


def run_contract(args=(), overrides=None):
    result, calls, root = run(args, overrides=overrides)
    assert result.returncode == 0, result.stderr
    return calls, root


calls, root = run_contract()
assert calls[0] == ['build', '--platform', 'linux/amd64', '-f', 'docker/Dockerfile.cia',
                    '-t', 'fixture-image', '.'], calls
command = calls[1]
assert command[:2] == ['run', '--rm'] and len(calls) == 2
assert command[command.index('--user')+1] == '%d:%d' % (os.getuid(), os.getgid())
assert command[command.index('-v')+1] == str(root) + ':/project'
assert command[command.index('-w')+1] == '/project'
assert 'DEVKITPRO=/fixture/pro' in command and 'DEVKITARM=/fixture/arm' in command
assert command[-4:-1] == ['fixture-image', 'sh', '-lc']
assert command[-1].split(' && ') == ['make clean', 'make -j2', 'make zip-sdmc',
    'make debug-3dsx', 'make cia', 'make debug-cia', 'make source-release']
calls, _ = run_contract(['--skip-image-build', '--jobs', '7', '--platform', 'linux/arm64'])
assert len(calls) == 1 and calls[0][0] == 'run'
assert calls[0][calls[0].index('--platform')+1] == 'linux/arm64'
assert 'make -j7' in calls[0][-1].split(' && ')
calls, _ = run_contract(overrides={'JOBS':'5', 'IMAGE_TAG':'env-image', 'PLATFORM':'linux/arm64'})
assert calls[0][2] == 'linux/arm64' and calls[0][6] == 'env-image'
assert calls[1][-4] == 'env-image' and 'make -j5' in calls[1][-1].split(' && ')
for args in (['--jobs'], ['--platform'], ['--unknown']):
    result, calls, _ = run(args)
    assert result.returncode != 0 and not calls
result, calls, _ = run(['--help'])
assert result.returncode == 0 and not calls
result, calls, _ = run(fail='build')
assert result.returncode == 19 and len(calls) == 1
result, calls, _ = run(fail='run')
assert result.returncode == 19 and len(calls) == 2
for missing in ('3dslibris-debug.cia', 'dist/3dslibris-source.tar.gz'):
    result, calls, _ = run(missing=missing)
    assert result.returncode != 0 and len(calls) == 2
print('PASS: Docker CLI arguments, failures and release output verification')
PY
