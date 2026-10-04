#!/usr/bin/env python3
"""Compare actual UVtools extension declarations with the native registry.

Inputs come from the managed oracle --registry and [.sla_export_audit].
An extension overlap cannot prove a matching container, version, or firmware;
Shared CWS/ZIP variants require explicit schema identification. No network access.
"""
import argparse
from collections import defaultdict
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reference', type=Path)
    parser.add_argument('native', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    reference = json.loads(args.reference.read_text())
    native = json.loads(args.native.read_text())
    reference_extensions = defaultdict(list)
    native_extensions = defaultdict(list)
    for family in reference:
        for extension in family['extensions']:
            reference_extensions[extension.lower()].append(family['family'])
    for entry in native:
        for extension in entry['extensions']:
            native_extensions[extension.lower()].append(entry)
    rows = []
    for extension, families in sorted(reference_extensions.items()):
        entries = native_extensions.get(extension, [])
        rows.append({
            'extension': extension,
            'reference_families': families,
            'native_ids': [entry['id'] for entry in entries],
            'exporter_ids': [entry['id'] for entry in entries if not entry['export_block']],
            'reader_ids': [entry['id'] for entry in entries if entry['reader_registered']],
            'requires_schema_identification': len(families) > 1 or len({entry['id'] for entry in entries}) > 1,
            'variant_export_rejection': 'Encrypted CTB is unavailable; native encoder writes unencrypted v2'
                if extension == 'encrypted.ctb' else None,
        })
    report = {
        'scope': 'Extension registration overlaps only; no container, version, image decoding or firmware certification',
        'reference_families': len(reference),
        'reference_unique_extensions': len(reference_extensions),
        'native_ids': len(native),
        'missing_extensions': [row['extension'] for row in rows if not row['native_ids']],
        'shared_extensions': [row['extension'] for row in rows if row['requires_schema_identification']],
        'rows': rows,
    }
    args.output.write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({key: value for key, value in report.items() if key != 'rows'}, indent=2))


if __name__ == '__main__':
    main()
