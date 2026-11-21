#!/usr/bin/env python3
"""Collect an ELF app's resolved native dependency closure for offline packaging.

Copies libraries, never executes the app or accesses the network. This is a
packaging component: resources, launcher and full relocation QA remain required.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def bundle(binary: Path, output: Path, runtime_libraries=()) -> dict:
    binary = binary.resolve(strict=True)
    output = output.resolve()
    project = Path(__file__).resolve().parents[1]
    if not output.is_relative_to(project):
        raise ValueError('Bundle output must stay inside the app project')
    if output.exists():
        raise ValueError('Refusing to replace an existing native dependency bundle')
    # Late dlopen roots cannot be discovered from the application's ldd output.
    # Validate every explicit root before invoking tools or creating output.
    runtime_roots = sorted({Path(path).resolve(strict=True) for path in runtime_libraries})
    for root in runtime_roots:
        if not root.is_file():
            raise ValueError(f'Runtime library must be an ELF file: {root}')
        with root.open('rb') as stream:
            if stream.read(4) != b'\x7fELF':
                raise ValueError(f'Runtime library must be an ELF file: {root}')
    libraries = {}
    hashes = {}

    def register(name, source):
        if name in ('', '.', '..', 'manifest.json', 'licenses') or Path(name).name != name:
            raise ValueError(f'Invalid native dependency name: {name}')
        source = source.resolve(strict=True)
        if not source.is_file():
            raise ValueError(f'Native dependency is not a file: {source}')
        digest = hashlib.sha256(source.read_bytes()).hexdigest()
        if name in libraries and (libraries[name] != source or hashes[name] != digest):
            raise ValueError(f'Native dependency SONAME collision: {name}: {libraries[name]} versus {source}')
        libraries[name], hashes[name] = source, digest

    def collect_closure(root):
        # ldd reports the recursive DT_NEEDED closure for each root, including
        # runtime-only roots; the root itself is registered by its ELF SONAME.
        result = subprocess.run(['ldd', str(root)], text=True, capture_output=True, check=True)
        if 'not found' in result.stdout:
            raise RuntimeError(result.stdout)
        for line in result.stdout.splitlines():
            match = re.search(r'^\s*(\S+)\s+=>\s+(/.+?)\s+\(', line)
            if match:
                # Preserve the existing absolute-loader-name handling while
                # keeping every destination a basename within output.
                register(Path(match[1]).name, Path(match[2]))

    collect_closure(binary)
    runtime_records = []
    for root in runtime_roots:
        result = subprocess.run(['readelf', '-d', str(root)], text=True, capture_output=True, check=True)
        names = re.findall(r'\(SONAME\).*?\[([^\]]+)\]', result.stdout)
        if len(names) != 1:
            raise ValueError(f'Runtime ELF must declare exactly one SONAME: {root}')
        name = names[0]
        register(name, root)
        collect_closure(root)
        runtime_records.append({'soname': name, 'source': str(root), 'sha256': hashes[name]})
    if not libraries:
        raise RuntimeError('No dynamic libraries found')
    # Validate fixed notices and optional installed notice trees as well.
    # Missing/unreadable inputs must not leave a partially validated bundle.
    notice_files = [(project / 'doc/licenses' / name, Path('licenses') / name)
                    for name in ('OpenCV-Apache-2.0.txt', 'OpenCV-NOTICE.md', 'Hershey-import.json')]
    notice_trees = [directory for directory in (Path('/usr/share/licenses/opencv5'), Path('/usr/share/licenses/opencv'),
                                                      Path('/opt/opencv/share/licenses/opencv4'))
                    if directory.is_dir()]
    for directory in notice_trees:
        notice_files.extend((source, Path('licenses') / directory.name / source.relative_to(directory))
                            for source in directory.rglob('*') if source.is_file())
    for source, _ in notice_files:
        source.read_bytes()
    binary_digest = hashlib.sha256(binary.read_bytes()).hexdigest()
    for name in libraries:
        if not (output / name).resolve().is_relative_to(output):
            raise ValueError('Native dependency target escaped the bundle')
    # All inputs are validated before the first write.
    output.mkdir(parents=True)
    records = []
    for name, source in sorted(libraries.items()):
        target = output / name
        shutil.copy2(source, target)
        records.append({'soname': name, 'source': str(source),
                        'sha256': hashlib.sha256(target.read_bytes()).hexdigest()})
        if records[-1]['sha256'] != hashes[name]:
            raise RuntimeError(f'Native dependency changed during copying: {source}')
    notices = output / 'licenses'
    notices.mkdir()
    for source, relative in notice_files:
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    manifest = {'binary': str(binary), 'binary_sha256': binary_digest,
                'libraries': records, 'runtime_libraries': runtime_records,
                'scope': 'native DT_NEEDED closure plus explicitly supplied runtime ELF roots only; unobserved dlopen roots, complete notices, installer and standalone app relocation remain separate'}
    (output / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('output', type=Path)
    parser.add_argument('--runtime-library', type=Path, action='append', default=[],
                        help='Additional observed dlopen ELF root (repeatable); copied under its declared SONAME with its ldd closure')
    args = parser.parse_args()
    manifest = bundle(args.binary, args.output, runtime_libraries=args.runtime_library)
    print(json.dumps({'libraries': len(manifest['libraries']), 'output': str(args.output)}))
