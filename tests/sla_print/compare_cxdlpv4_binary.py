#!/usr/bin/env python3
"""Compare addressed CXDLPv4 sections rather than misaligned whole-file offsets.

Pair with UVtools full decoding to validate pixels. This reports raw metadata,
including fields UVtools recomputes on loading, and exact RLE/extended bytes.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path


def read(path):
    data = path.read_bytes()
    if data[:15] != b'\0\0\0\x09CXSW3DV2\0\0\x04':
        raise ValueError('Not CXDLPv4')
    length, = struct.unpack_from('>I', data, 15)
    base = 19 + length
    unpack = lambda fmt, offset: struct.unpack_from('<' + fmt, data, offset)
    model = data[19:base-1].decode()
    header = dict(zip(('ResolutionX', 'ResolutionY', 'BedX', 'BedY', 'BedZ', 'PrintHeight', 'LayerHeight',
                       'BottomLayers', 'PreviewSmallOffset', 'LayerTableOffset', 'LayerCount', 'PreviewLargeOffset',
                       'PrintTime', 'ProjectorType', 'ParametersOffset', 'ParametersSize', 'AntiAlias',
                       'LightPWM', 'BottomLightPWM', 'EncryptionKey', 'SlicerOffset', 'SlicerSize'),
                      unpack('HH5f10IHH3I', base)))
    header['PrinterModel'] = model
    if header['EncryptionKey']:
        raise ValueError('This comparison requires unencrypted payloads')
    previews = []
    for key in ('PreviewSmallOffset', 'PreviewLargeOffset'):
        width, height, offset, size, *reserved = unpack('8I', header[key])
        cursor = offset
        pixels = bytearray()
        while cursor < offset + size:
            color, = unpack('H', cursor)
            cursor += 2
            run = 1
            if color & 32:
                count, = unpack('H', cursor)
                cursor += 2
                run += count & 4095
            pixels.extend(struct.pack('<H', color & ~32) * run)
        if len(pixels) != width * height * 2:
            raise ValueError('Incomplete preview')
        previews.append(dict(width=width, height=height, bytes=size,
                             sha256=hashlib.sha256(pixels).hexdigest(), reserved=reserved))
    parameters = unpack('10fI2f4I', header['ParametersOffset'])
    slicer = unpack('7f4I5f3I', header['SlicerOffset'])
    layers = []
    for i in range(header['LayerCount']):
        row = unpack('3f7I', header['LayerTableOffset'] + i*40)
        address, size = row[3:5]
        layers.append(dict(definition=row[:3] + row[5:], extended=unpack('11f', address),
                           rle_bytes=size-44, rle_sha256=hashlib.sha256(data[address+44:address+size]).hexdigest()))
    return dict(filename=str(path), bytes=len(data), sha256=hashlib.sha256(data).hexdigest(),
                header=header, previews=previews, parameters=parameters, slicer=slicer, layers=layers)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('application', type=Path)
    parser.add_argument('reference', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    a, b = read(args.application), read(args.reference)
    differences = []
    for section in ('header', 'parameters', 'slicer', 'previews'):
        left, right = a[section], b[section]
        for key in left if isinstance(left, dict) else range(len(left)):
            if left[key] != right[key]:
                differences.append(dict(section=section, field=key, application=left[key], reference=right[key]))
    layer_differences = []
    for i, (left, right) in enumerate(zip(a['layers'], b['layers'])):
        for key in left:
            if left[key] != right[key]:
                layer_differences.append(dict(layer=i, field=key, application=left[key], reference=right[key]))
    result = dict(application={k:a[k] for k in ('filename', 'bytes', 'sha256')},
                  reference={k:b[k] for k in ('filename', 'bytes', 'sha256')},
                  size_difference=a['bytes']-b['bytes'],
                  layer_counts=[len(a['layers']), len(b['layers'])],
                  identical_rle_layers=sum(x['rle_sha256']==y['rle_sha256'] for x,y in zip(a['layers'],b['layers'])),
                  differences=differences, layer_differences=layer_differences)
    args.output.write_text(json.dumps(result, indent=2)+'\n')
    print(json.dumps({k:v for k,v in result.items() if k!='layer_differences'}, indent=2))
    print('Layer metadata differences:', len(layer_differences))


if __name__ == '__main__':
    main()
