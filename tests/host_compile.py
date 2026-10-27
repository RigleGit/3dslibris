#!/usr/bin/env python3
"""Reuse validated native test objects; keep executable output isolated."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


def run(command):
    result = subprocess.run(command)
    if result.returncode:
        raise SystemExit(result.returncode)


def dependency_paths(text):
    # Make escapes whitespace/backslashes, but apostrophes and quotes are
    # literal filename characters, not shell quoting.
    text = text.replace('\\\n', ' ').split(':', 1)[1]
    paths, token = [], []
    escaped = False
    for char in text:
        if escaped:
            token.append(char)
            escaped = False
        elif char == '\\':
            escaped = True
        elif char.isspace():
            if token:
                paths.append(''.join(token).replace('$$', '$'))
                token = []
        else:
            token.append(char)
    if token:
        paths.append(''.join(token).replace('$$', '$'))
    return paths

def namespace_digest(root):
    signature = hashlib.sha256()
    stack, seen = [str(Path(root).resolve())], set()
    while stack:
        directory = stack.pop()
        if directory in seen:
            continue
        seen.add(directory)
        if not os.path.isdir(directory):
            signature.update(b'missing')
            continue
        with os.scandir(directory) as entries:
            for entry in sorted(entries, key=lambda e: e.name):
                signature.update(os.fsencode(entry.path) + b'\0')
                signature.update(b'd' if entry.is_dir() else b'f')
                if entry.is_symlink():
                    signature.update(os.fsencode(os.readlink(entry.path)))
                if entry.is_dir():
                    stack.append(os.path.realpath(entry.path) if entry.is_symlink() else entry.path)
    return signature.hexdigest()


def publish(path, content):
    fd, temporary = tempfile.mkstemp(dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as stream:
            stream.write(content)
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def main():
    kind, *args = sys.argv[1:]
    compiler = os.environ['DSLIBRIS_HOST_REAL_' + kind]
    raw = [compiler, *args]
    # Coverage must produce fresh instrumented objects/profiles in its own run.
    if (os.environ.get('DSLIBRIS_TEST_CACHE', '1') == '0' or
            os.environ.get('LLVM_PROFILE_FILE') or
            any(a.startswith(('-fprofile', '-ftest-coverage', '--coverage')) for a in args) or
            any(a in ('-E', '-S', '-M', '-MM', '-MD', '-MMD', '-shared', '-x')
                or a.startswith('@') for a in args)):
        run(raw)
        return

    sources, options, link = [], [], []
    output = None
    compile_only = '-c' in args
    compile_pairs = ('-I', '-D', '-U', '-isystem', '-iquote', '-idirafter', '-include', '-imacros')
    shared_pairs = ('-isysroot', '--sysroot', '-target', '-arch')
    link_pairs = ('-L', '-l', '-Xlinker', '-framework')
    iterator = iter(args)
    for arg in iterator:
        if arg == '-o':
            output = next(iterator)
            link.extend((arg, output))
        elif arg == '-c':
            continue
        elif arg in compile_pairs:
            options.extend((arg, next(iterator)))
        elif arg in shared_pairs:
            value = next(iterator)
            options.extend((arg, value))
            link.extend((arg, value))
        elif arg in link_pairs:
            link.extend((arg, next(iterator)))
        elif arg.startswith(('-I', '-D', '-U', '-std=')) or arg in (
                '-fno-exceptions', '-fno-rtti', '-ffunction-sections',
                '-fdata-sections', '-fcoverage-mapping'):
            options.append(arg)
        elif arg.startswith(('-L', '-l', '-Wl,')) or arg.endswith(('.o', '.a', '.so', '.dylib')):
            link.append(arg)
        elif arg.endswith(('.cpp', '.cc', '.cxx', '.c')) and not arg.startswith('-'):
            sources.append(arg)
            link.append(arg)
        elif arg.startswith(('-O', '-g', '-m', '--sysroot=', '--target=',
                             '-fsanitize=', '-fno-sanitize=')) or arg in (
                                 '-pthread', '-fno-omit-frame-pointer', '-fPIC', '-fPIE'):
            options.append(arg)
            link.append(arg)
        elif arg.startswith('-W') or arg in ('-w', '-pedantic'):
            options.append(arg)
        else:
            # Unrecognised driver options may have side outputs or language
            # semantics. Preserve them by using the original command unchanged.
            run(raw)
            return
    if not output or not sources or (compile_only and len(sources) != 1):
        run(raw)
        return

    executable = Path(shutil.which(compiler) or compiler).resolve()
    stat = executable.stat()
    version = subprocess.check_output([compiler, '--version'])
    config = [hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              kind, compiler, str(executable), stat.st_size, stat.st_mtime_ns, version.decode(errors='replace'),
              os.getcwd(), options,
              {k: os.environ.get(k) for k in ('SDKROOT', 'MACOSX_DEPLOYMENT_TARGET',
                  'SOURCE_DATE_EPOCH', 'CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH',
                  'GCC_EXEC_PREFIX', 'COMPILER_PATH', 'LANG', 'LC_ALL')}]
    language = 'c++' if kind == 'CXX' or any(not s.endswith('.c') for s in sources) else 'c'
    search = subprocess.run([compiler, *options, '-E', '-v', '-x', language, os.devnull],
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    if search.returncode:
        run(raw)
        return
    config.append(search.stderr.decode(errors='replace'))
    roots, inside = [], False
    for line in config[-1].splitlines():
        if 'search starts here:' in line:
            inside = True
        elif 'End of search list.' in line:
            inside = False
        elif inside:
            roots.append(Path(line.strip().replace(' (framework directory)', '')).resolve())
    if not roots:
        run(raw)
        return
    explicit = set()
    iterator = iter(options)
    for arg in iterator:
        if arg in ('-I', '-isystem', '-iquote', '-idirafter'):
            explicit.add(Path(next(iterator)).resolve())
        elif arg.startswith('-I'):
            explicit.add(Path(arg[2:]).resolve())
    for name in ('CPATH', 'C_INCLUDE_PATH', 'CPLUS_INCLUDE_PATH'):
        explicit.update(Path(s or '.').resolve() for s in os.environ.get(name, '').split(os.pathsep))
    project = Path(__file__).resolve().parent.parent
    scratch = Path(os.environ.get('TMPDIR', tempfile.gettempdir())).resolve()
    memo = os.environ.get('DSLIBRIS_HOST_NAMESPACE_DIR')
    namespaces = {}
    for root in roots:
        # System/toolchain directory names are frozen for one isolated run.
        # Project, temporary and explicit search directories are always rescanned.
        external = root not in explicit and project not in (root, *root.parents) and scratch not in (root, *root.parents)
        record = Path(memo) / hashlib.sha256(os.fsencode(str(root))).hexdigest() if memo and external else None
        if record and record.is_file():
            namespaces[root] = record.read_text()
        else:
            namespaces[root] = namespace_digest(root)
            if record:
                record.parent.mkdir(parents=True, exist_ok=True)
                publish(record, namespaces[root])
    cache = Path(os.environ['DSLIBRIS_HOST_CACHE_DIR'])
    cache.mkdir(parents=True, exist_ok=True)
    digests, objects = {}, {}

    def digest(path):
        if path not in digests:
            content = path.read_bytes()
            digests[path] = (hashlib.sha256(content).hexdigest(),
                bool(re.search(rb'\b__(?:DATE|TIME|TIMESTAMP)__\b', content)))
        return digests[path]

    def adjacent_namespaces(paths, source_parent):
        # A quoted include also searches beside its including header. Headers
        # reached via -include or ../ may live outside the compiler's -I roots.
        covered = [*roots, source_parent]
        adjacent = []
        for parent in sorted(set(Path(p).parent for p in paths)):
            if any(root == parent or root in parent.parents for root in covered):
                continue
            if parent not in namespaces:
                namespaces[parent] = namespace_digest(parent)
            adjacent.append([str(parent), namespaces[parent]])
        return adjacent

    with tempfile.TemporaryDirectory(dir=cache) as work:
        for number, source in enumerate(sources):
            parent = Path(source).resolve().parent
            if parent not in namespaces:
                namespaces[parent] = namespace_digest(parent)
            namespace = [list((str(p), namespaces[p])) for p in [*roots, parent]]
            base = hashlib.sha256(json.dumps([config, source]).encode()).hexdigest()
            manifest = cache / (base + '.json')
            previous = json.loads(manifest.read_text()) if manifest.is_file() else None
            if (previous and previous['namespace'] == namespace and
                    previous['adjacent'] == adjacent_namespaces(
                        [p for p, _ in previous['dependencies']], parent)):
                obj = cache / previous['object']
                if obj.is_file() and all(digest(Path(p))[0] == value for p, value in previous['dependencies']):
                    objects[source] = str(obj)
                    continue
            temporary = Path(work) / (str(number) + '.o')
            dependencies = Path(work) / (str(number) + '.d')
            run([compiler, *options, '-MD', '-MF', str(dependencies), '-c', source, '-o', str(temporary)])
            paths = sorted(set(str(Path(p).resolve()) for p in dependency_paths(dependencies.read_text())))
            values = [(p, digest(Path(p))[0]) for p in paths]
            if any(digest(Path(p))[1] for p in paths):
                objects[source] = str(temporary)
                continue
            signature = hashlib.sha256(json.dumps([base, values]).encode()).hexdigest()
            obj = cache / (signature + '.o')
            os.replace(temporary, obj)
            publish(manifest, json.dumps({'object': obj.name, 'namespace': namespace,
                'adjacent': adjacent_namespaces(paths, parent), 'dependencies': values}))
            objects[source] = str(obj)
        if compile_only:
            shutil.copyfile(objects[sources[0]], output)
        else:
            run([compiler, *(objects.get(arg, arg) for arg in link)])


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError, StopIteration, UnicodeError):
        # Cache I/O/metadata errors fall back to the original driver.
        kind, *args = sys.argv[1:]
        run([os.environ['DSLIBRIS_HOST_REAL_' + kind], *args])
