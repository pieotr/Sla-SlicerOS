#!/usr/bin/env python3
"""Compare opt-in native mesh benchmarks without imposing a noisy time limit.

Generate each input with SLA_MESH_BENCHMARK_PATH and [.sla_mesh_benchmark].
The equality checks cover the complete PNG stack and its layer positions.
"""
import argparse
import json
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('before', type=Path)
    parser.add_argument('after', type=Path)
    arguments = parser.parse_args()
    before = json.loads(arguments.before.read_text())
    after = json.loads(arguments.after.read_text())
    before = {entry['shape']: entry for entry in before}
    after = {entry['shape']: entry for entry in after}
    if before.keys() != after.keys():
        raise ValueError('Benchmark cases differ')
    comparison = []
    for shape, original in before.items():
        current = after[shape]
        # Compression bytes may change while pixels remain exact. Re-encoding
        # the new pixels with the old encoder provides comparable fingerprints.
        comparable = dict(current)
        if current.get('legacy_png_sha1'):
            comparable['png_sha1'] = current['legacy_png_sha1']
        for field in ('resolution', 'positions_z', 'png_sha1', 'identical_adjacent_geometry'):
            if original[field] != comparable[field]:
                raise ValueError(f'{shape}: {field} changed')
        comparison.append({
            'shape': shape,
            'layers': len(current['positions_z']),
            'all_masks_and_z_equal': True,
            'before_ms': original['total_ms'],
            'after_ms': current['total_ms'],
            'speed_ratio': original['total_ms'] / current['total_ms'],
        })
    print(json.dumps(comparison, indent=2))


if __name__ == '__main__':
    main()
