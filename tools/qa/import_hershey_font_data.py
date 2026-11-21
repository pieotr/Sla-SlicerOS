#!/usr/bin/env python3
"""One-time local import of Apache-2.0 OpenCV Hershey data from QA symbols.

The generated header is portable source data. No reference binary is used by
the production app. Run only against the known local unstripped QA library.
"""
import ctypes
import hashlib
import json
from pathlib import Path
import re
import subprocess

project = Path(__file__).resolve().parents[2]
source = project / 'build/implementation-2026-10-02-next/full-oracle/libcvextern.so'
symbols = subprocess.run(['nm', '-S', '-C', str(source)], capture_output=True, text=True, check=True).stdout
entries = {}
for line in symbols.splitlines():
    match = re.match(r'([0-9a-f]+) ([0-9a-f]+) \w (cv::.*)', line)
    if match:
        entries[match[3]] = (int(match[1], 16), int(match[2], 16))
handle = ctypes.CDLL(str(source))
base = next(int(line.split('-')[0],16) for line in Path('/proc/self/maps').read_text().splitlines()
            if str(source) in line and line.split()[2] == '00000000')
address, size = entries['cv::g_HersheyGlyphs']
glyphs = (ctypes.c_char_p * (size // ctypes.sizeof(ctypes.c_void_p))).from_address(base + address)
get_font = ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_int)(base + entries['cv::getFontData(int)'][0])
arrays = {}
font_maps = []
for face in list(range(8)) + list(range(16,24)):
    pointer = get_font(face)
    name, (_, length) = next((name, value) for name, value in entries.items()
                            if name.startswith('cv::Hershey') and base + value[0] == pointer)
    key = name.removeprefix('cv::')
    arrays[key] = list((ctypes.c_int * (length // 4)).from_address(pointer))
    font_maps.append(key)
header = ['// OpenCV 4.12 Hershey data, OpenCV contributors, Apache-2.0.',
          '// Imported locally; provenance: doc/licenses/Hershey-import.json.',
          '#pragma once', 'namespace Slic3r::sla::hershey {',
          'inline constexpr const char *glyphs[] = {']
header += [json.dumps((glyph or b'').decode('ascii')) + ',' for glyph in glyphs]
header += ['};']
for name, values in arrays.items():
    header += ['inline constexpr int ' + name + '[] = {' + ','.join(map(str,values)) + '};']
header += ['inline constexpr const int *fonts[] = {' + ','.join(font_maps) + '};', '}']
target = project / 'src/libslic3r/SLA/HersheyFontData.hpp'
target.write_text('\n'.join(header) + '\n')
manifest = {'reference': str(source.relative_to(project)), 'reference_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
            'glyphs': len(glyphs), 'font_faces': 16, 'arrays': {k:len(v) for k,v in arrays.items()},
            'generated_header_sha256': hashlib.sha256(target.read_bytes()).hexdigest(),
            'license': 'OpenCV 4.12 Apache-2.0; see OpenCV-Apache-2.0.txt'}
(project / 'doc/licenses/Hershey-import.json').write_text(json.dumps(manifest,indent=2) + '\n')
print(json.dumps(manifest,indent=2))
