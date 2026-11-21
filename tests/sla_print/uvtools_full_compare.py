#!/usr/bin/env python3
"""Compare pairs of original UVtools full-decoder reports, with exact pixels/parameters.

Development-only. Pair order is application output, UVtools output. A nonzero
exit code or an incomplete report is a failed comparison, never a certificate.
"""
import argparse
import hashlib
import json
from io import BytesIO
from pathlib import Path
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    files = json.loads(args.report.read_text())
    if not files or len(files) % 2:
        raise ValueError('Expected a nonempty array of application/reference pairs')
    pairs = []
    for application, reference in zip(files[::2], files[1::2]):
        differences = []
        for item in (application, reference):
            if item['status'] != 'fully_decoded' or item['LayerCount'] != len(item['decodedLayers']):
                raise ValueError('Incomplete full-decoder report')
            if item['family'] == 'SL1File':
                # Independent PNG decoding verifies the UVtools pixel hash as well.
                from PIL import Image
                with zipfile.ZipFile(item['filename']) as archive:
                    masks = sorted(n for n in archive.namelist() if n.endswith('.png') and not n.startswith('thumbnail'))
                    if len(masks) != item['LayerCount']:
                        raise ValueError('SL1 mask count differs from the decoder')
                    for name, layer in zip(masks, item['decodedLayers']):
                        image = Image.open(BytesIO(archive.read(name))).convert('L')
                        if image.size != (layer['width'], layer['height']) or hashlib.sha256(image.tobytes()).hexdigest() != layer['sha256']:
                            raise ValueError('UVtools SL1 pixels differ from independent PNG decoding')
        for key in sorted(application.keys() | reference.keys()):
            if key in ('filename', 'decodedLayers'):
                continue
            if application.get(key) != reference.get(key):
                differences.append({'global': key, 'application': application.get(key), 'uvtools': reference.get(key)})
        if len(application['decodedLayers']) == len(reference['decodedLayers']):
            for left, right in zip(application['decodedLayers'], reference['decodedLayers']):
                for key in sorted(left.keys() | right.keys()):
                    if left.get(key) != right.get(key):
                        differences.append({'layer': left['index'], 'field': key, 'application': left.get(key), 'uvtools': right.get(key)})
        pairs.append({'application': application['filename'], 'reference': reference['filename'],
                      'family': application['family'], 'layers': application['LayerCount'],
                      'global_fields': len(application) - 2, 'differences': differences})
    result = {'status': 'passed' if all(not p['differences'] for p in pairs) else 'failed', 'pairs': pairs}
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result['status'] == 'passed' else 1)


if __name__ == '__main__':
    main()
