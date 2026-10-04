#!/usr/bin/env python3
"""Open and edit an exported layer archive in an isolated GTK application."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time
import zipfile
from printer_wizard_smoke import Atspi, find, named, wait_for, click


def cxdlpv1(data):
    """Independent inclusive-run decoder for checking edited GUI output."""
    cursor = 0
    def read(size):
        nonlocal cursor
        result = data[cursor:cursor+size]
        assert len(result) == size, 'Truncated CXDLPv1 table'
        cursor += size
        return result
    def integer(size): return int.from_bytes(read(size), 'big')
    assert read(13) == b'\x00\x00\x00\x09CXSW3DV2\x00'
    version = 1
    count = integer(2)
    model_length = int.from_bytes(data[cursor:cursor+4], 'big')
    if count in (2,3) and 0 < model_length <= 256:
        version = count
        read(integer(4)); count = integer(2)
    cols, rows = integer(2), integer(2)
    if version >= 2:
        read(64)
        expected = 0
        if version == 2:
            for byte in data[:-4]: expected ^= byte
        else:
            import zlib
            expected = zlib.crc32(data[:-4], 0xffffffff)^0xffffffff
        assert int.from_bytes(data[-4:], 'big') == expected, 'CXDLP checksum mismatch' 
    for size in (116, 290, 290):
        read(size*size*2); assert read(2) == b'\r\n'
    for unused in range(3): read(integer(4))
    read(22); prefix = data[:cursor]
    counts = [integer(4) for unused in range(count)]
    assert read(2) == b'\r\n'
    if version >= 3:
        informative = cursor
        for unused in range(2): read(integer(4))
        read(20); assert read(2) == b'\r\n'
        prefix += data[informative:cursor]
    masks = []
    for layer in range(count):
        assert integer(4) == counts[layer]
        lines = integer(4); pixels = bytearray(cols*rows)
        for unused in range(lines):
            p = read(6)
            y0 = ((p[0] << 8 | p[1]) >> 3) & 0x1fff
            y1 = ((p[1] << 16 | p[2] << 8 | p[3]) >> 6) & 0x1fff
            x = (p[3] << 8 | p[4]) & 0x3fff
            assert y0 <= y1 < rows and x < cols and p[5] > 0
            for y in range(y0, y1+1):
                assert pixels[y*cols+x] == 0
                pixels[y*cols+x] = p[5]
        if version == 1: assert sum(value != 0 for value in pixels) == counts[layer]
        masks.append(bytes(pixels)); assert read(2) == b'\r\n'
    footer = read(17)
    assert footer[:13] == b'\x00\x00\x00\x09CXSW3DV2\x00' and cursor == len(data)
    return prefix, footer if version == 1 else footer[:-4], masks


def cxdlpv4(data):
    """Independent v4 addresses, cipher and seven-bit RLE decoder."""
    import zlib
    assert data[:15] == b'\x00\x00\x00\x09CXSW3DV2\x00\x00\x04'
    assert int.from_bytes(data[-4:],'big') == (zlib.crc32(data[:-4],0xffffffff)^0xffffffff)
    def integer(offset, size=4):
        assert 0 <= offset and offset+size <= len(data)-4
        return int.from_bytes(data[offset:offset+size],'little')
    base=19+int.from_bytes(data[15:19],'big')
    cols,rows=integer(base,2),integer(base+2,2)
    table,count,seed=integer(base+32),integer(base+36),integer(base+68)
    metadata=bytearray(data[:base+80])
    for offset in (28,32,40,52,72): metadata[base+offset:base+offset+4]=bytes(4)
    for offset in (28,40):
        address=integer(base+offset)
        if not address: continue
        image=integer(address+8); size=integer(address+12)
        header=bytearray(data[address:address+32]);header[8:12]=bytes(4)
        metadata.extend(header);metadata.extend(data[image:image+size])
    parameters=integer(base+52)
    if parameters:
        values=bytearray(data[parameters:parameters+integer(base+56)])
        values[20:32]=bytes(12) # Volume/weight/cost legitimately follow edits.
        metadata.extend(values)
    slicer=integer(base+72)
    if slicer: metadata.extend(data[slicer:slicer+integer(base+76)])
    masks=[]
    for layer in range(count):
        row=table+40*layer;address,size=integer(row+12),integer(row+16)
        assert size>=44 and address+size<=len(data)-4
        metadata.extend(data[row:row+12]);metadata.extend(data[row+20:row+28]);metadata.extend(data[row+32:row+40])
        metadata.extend(data[address:address+44])
        payload=bytearray(data[address+44:address+size])
        if seed:
            increment=(seed*0x2d83cdac+0xd8a83423)&0xffffffff
            key=((layer*0x1e1530cd+0xec3d47cd)*increment)&0xffffffff
            for index in range(len(payload)):
                payload[index]^=(key>>(8*(index%4)))&255
                if index%4==3: key=(key+increment)&0xffffffff
        pixels=bytearray();cursor=0
        while cursor<len(payload):
            code=payload[cursor];cursor+=1;run=1
            if code&128:
                assert cursor<len(payload)
                prefix=payload[cursor];cursor+=1
                if prefix<128: run,continuation=prefix,0
                elif prefix<192: run,continuation=prefix&63,1
                elif prefix<224: run,continuation=prefix&31,2
                elif prefix<240: run,continuation=prefix&15,3
                else: raise AssertionError('Invalid v4 run prefix')
                for unused in range(continuation):
                    assert cursor<len(payload)
                    run=(run<<8)|payload[cursor];cursor+=1
            gray=code&127
            assert run>0 and len(pixels)+run<=cols*rows
            pixels.extend(bytes((0 if gray==0 else (gray<<1)|1,))*run)
        assert len(pixels)==cols*rows;masks.append(bytes(pixels))
    return bytes(metadata), b'', masks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inspect', action='store_true')
    parser.add_argument('--cancel-range', action='store_true')
    parser.add_argument('--cancel-operation', choices=('dilate','median'), default='dilate')
    parser.add_argument('--save-png', action='store_true', help='Save an inverted standalone PNG through the native chooser')
    parser.add_argument('--save-archive', action='store_true', help='Save edited archive and check that non-layer entries remain unchanged')
    parser.add_argument('--save-binary', action='store_true', help='Save CXDLP v1/v2/v3/v4 and independently compare pixels and preserved metadata')
    parser.add_argument('--expect-invalid', action='store_true', help='Check checksum-error handling without opening an editor or terminating the app')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='archive-editor-', dir=args.output.resolve()))
    datadir = directory / 'presets'
    shutil.copytree(args.seed, datadir)
    report = {'datadir': str(datadir), 'checks': []}
    with (directory / 'application.log').open('w') as log:
        process = subprocess.Popen([str(args.binary.resolve()), '--datadir', str(datadir)],
            stdout=log, stderr=subprocess.STDOUT,
            env=dict(os.environ, GDK_BACKEND='x11', NO_AT_BRIDGE='0'))
    def visible(node): return node.get_state_set().contains(Atspi.StateType.SHOWING)
    def dump(node, depth=0):
        if depth > 15: return []
        rows = []
        if visible(node): rows.append([node.get_role_name(), node.get_name(), node.get_description()])
        for index in range(node.get_child_count()): rows.extend(dump(node.get_child_at_index(index), depth+1))
        return rows
    try:
        desktop = Atspi.get_desktop(0)
        app = wait_for(lambda: find(desktop, lambda n:
            n.get_role_name() == 'application' and n.get_process_id() == process.pid), 'application')
        wait_for(lambda: named(app, 'SLA material:'), 'loaded settings')
        menu = wait_for(lambda: find(app, lambda n: n.get_role_name() == 'menu item' and
            n.get_name().startswith('Open layer archive')), 'archive editor menu')
        click(menu)
        chooser = wait_for(lambda: find(app, lambda n: n.get_name() == 'Open layer archive' and visible(n)
            and n.get_role_name() != 'menu item'), 'open archive chooser')
        if args.inspect:
            report['tree'] = dump(chooser); report['status'] = 'inspected'; return
        # Navigate the native chooser through its accessible table rows. This
        # avoids synthetic keyboard events, which this desktop bridge drops.
        root = Path(__file__).resolve().parents[2]
        for component in args.archive.resolve().relative_to(root).parts:
            cell = wait_for(lambda: find(chooser, lambda n: n.get_role_name() == 'table cell'
                and n.get_name() == component and visible(n)), 'chooser row: ' + component)
            table = cell.get_parent()
            while table and table.get_role_name() not in ('table', 'tree table'):
                table = table.get_parent()
            assert table is not None, 'File chooser row has no table'
            row_node = cell
            while row_node.get_parent() != table: row_node = row_node.get_parent()
            interface = table.get_table_iface()
            row = interface.get_row_at_index(row_node.get_index_in_parent())
            report.setdefault('table_trace', []).append({'component': component, 'index': row_node.get_index_in_parent(),
                'row': row, 'rows': interface.get_n_rows(), 'columns': interface.get_n_columns()})
            wait_for(lambda: row >= 0 and (interface.is_row_selected(row) or interface.add_row_selection(row)), 'selected file row')
            click(named(chooser, 'Open'))
        if args.expect_invalid:
            error = wait_for(lambda: find(app, lambda n: n.get_name() == 'PNG chunk checksum mismatch' and visible(n)), 'invalid PNG message')
            click(wait_for(lambda: find(app, lambda n: n.get_role_name() == 'button' and n.get_name() == 'OK' and visible(n)), 'error acknowledgement'))
            wait_for(lambda: not visible(error), 'dismissed import error')
            assert process.poll() is None, 'Invalid input terminated the application'
            assert named(app, 'Save edited PNG') is None, 'Invalid input opened an editable document'
            report['checks'].append('invalid PNG ends progress, shows an error and leaves the application running')
            audit = os.environ.get('SLA_NETWORK_AUDIT_FILE')
            if audit:
                assert 'offline_network_audit.so' in Path(f'/proc/{process.pid}/maps').read_text()
                assert not (Path(audit).read_text() if Path(audit).exists() else '')
                report['checks'].append('network audit loaded; no network attempts')
            report['status'] = 'passed'
            return
        dialog = wait_for(lambda: find(app, lambda n: n.get_name().startswith('Layer editor') and visible(n)), 'loaded archive layers')
        click(named(dialog, 'Invert layer'))
        wait_for(lambda: dialog.get_name().endswith('*'), 'archive edit')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not dialog.get_name().endswith('*'), 'archive undo')
        assert named(dialog, 'Save edited PNG' if args.save_png else 'Save edited archive') is not None
        if args.save_png or args.save_archive or args.save_binary:
            click(named(dialog, 'Redo'))
            wait_for(lambda: dialog.get_name().endswith('*'), 'restored PNG edit')
            click(named(dialog, 'Save edited PNG' if args.save_png else 'Save edited archive'))
            save = wait_for(lambda: find(app, lambda n: n.get_name() == 'Save edited layers' and visible(n)
                and n.get_role_name() != 'menu item'), 'PNG save chooser')
            field = find(save, lambda n: n.get_role_name() in ('text', 'entry') and visible(n)
                and n.get_state_set().contains(Atspi.StateType.EDITABLE))
            assert field is not None, 'Missing PNG filename field'
            extension = ('.cxdlpv4' if args.archive.name.endswith('.cxdlpv4') else '.v1.cxdlp' if args.archive.name.endswith('.v1.cxdlp') else '.v2.cxdlp' if args.archive.name.endswith('.v2.cxdlp') else '.cxdlp') if args.save_binary else '.png' if args.save_png else '.rgb.cws' if args.archive.name.lower().endswith('.rgb.cws') else '.xml.cws' if args.archive.name.lower().endswith('.xml.cws') else args.archive.suffix
            assert Atspi.Text.get_text(field, 0, -1).endswith(extension), 'Default container suffix is missing'
            output = directory / ('edited-mask' + extension)
            assert field.get_editable_text_iface().set_text_contents(str(output))
            click(named(save, 'Save'))
            wait_for(lambda: output.exists() and not dialog.get_name().endswith('*'), 'saved clean PNG')
            if args.save_png:
                data = output.read_bytes()
                assert data[:8] == b'\x89PNG\r\n\x1a\n' and data[25] == 0, 'Output is not a grayscale PNG'
                report['checks'].append('Redo, automatic PNG suffix, raw grayscale PNG save and clean revision')
            elif args.save_binary:
                decoder = cxdlpv4 if args.archive.name.endswith('.cxdlpv4') else cxdlpv1
                original = decoder(args.archive.read_bytes()); saved = decoder(output.read_bytes())
                assert original[:2] == saved[:2], 'Binary metadata or previews changed'
                expected = bytes(255-value for value in original[2][0])
                if args.archive.name.endswith('.cxdlpv4'):
                    expected = bytes(0 if value<2 else (value&254)|1 for value in expected)
                assert saved[2][0] == expected
                assert saved[2][1:] == original[2][1:], 'Unedited layers changed'
                report['checks'].append('Redo, compound suffix and binary save; all pixels and preserved metadata independently verified')
            elif args.archive.name.lower().endswith('.rgb.cws'):
                from PIL import Image
                from io import BytesIO
                def unpack(data):
                    assert data[24:26]==bytes((8,2))
                    image=Image.open(BytesIO(data));assert image.mode=='RGB'
                    rgb=image.tobytes();gray=bytearray(rgb)
                    for i in range(0,len(gray),3):gray[i],gray[i+2]=gray[i+2],gray[i]
                    return image.size,bytes(gray)
                with zipfile.ZipFile(args.archive) as original,zipfile.ZipFile(output) as saved:
                    assert original.namelist()==saved.namelist()
                    names=sorted(name for name in original.namelist() if name.endswith('.png') and name[-5].isdigit())
                    assert names
                    for name in original.namelist():
                        if name not in names:assert original.read(name)==saved.read(name),name
                    for index,name in enumerate(names):
                        dimensions,before=unpack(original.read(name));saved_dimensions,after=unpack(saved.read(name))
                        assert dimensions==saved_dimensions
                        assert after==(bytes(255-value for value in before) if index==0 else before)
                report['checks'].append('RGB.CWS compound suffix and BGR save: all pixels, slice.conf, commands and unknown entries preserved')
            elif args.archive.name.lower().endswith('.xml.cws'):
                import xml.etree.ElementTree as ET
                from PIL import Image
                from io import BytesIO
                with zipfile.ZipFile(args.archive) as original, zipfile.ZipFile(output) as saved:
                    assert original.namelist() == saved.namelist()
                    names = [node.text for node in ET.fromstring(original.read('manifest.xml')).findall('Slices/Slice/name')]
                    assert names
                    for name in original.namelist():
                        if name not in names: assert original.read(name) == saved.read(name), name
                    for index,name in enumerate(names):
                        before = Image.open(BytesIO(original.read(name))).convert('L').tobytes()
                        data = saved.read(name)
                        assert data[24:26] == bytes((8,6))
                        image = Image.open(BytesIO(data))
                        red,green,blue,alpha = image.split()
                        expected = bytes(255-value for value in before) if index==0 else before
                        assert red.tobytes() == green.tobytes() == blue.tobytes() == expected
                        assert alpha.getextrema() == (255,255)
                report['checks'].append('XML.CWS compound suffix and RGBA save: all pixels, XML, exposure/motion commands and unknown entries preserved')
            else:
                with zipfile.ZipFile(args.archive) as original, zipfile.ZipFile(output) as saved:
                    assert original.namelist() == saved.namelist(), 'Archive entries changed'
                    changed = [name for name in original.namelist() if original.read(name) != saved.read(name)]
                    assert len(changed) == 1 and changed[0].endswith('.png'), 'Save changed metadata or an unexpected number of masks'
                    from PIL import Image
                    from io import BytesIO
                    before = Image.open(BytesIO(original.read(changed[0]))).convert('L').tobytes()
                    after = Image.open(BytesIO(saved.read(changed[0]))).convert('L').tobytes()
                    assert after == bytes(255-value for value in before), 'Edited mask pixels changed unexpectedly'
                report['checks'].append('Redo, automatic container suffix, archive save preserves metadata/commands and all edited pixels')
            report['saved_output'] = str(output)
        if args.cancel_range:
            choices, spins = [], []
            def controls(node):
                if visible(node):
                    if node.get_role_name() == 'combo box': choices.append(node)
                    if node.get_role_name() == 'spin button': spins.append(node)
                for index in range(node.get_child_count()): controls(node.get_child_at_index(index))
            controls(dialog)
            assert choices[1].get_selection_iface().select_child(14 if args.cancel_operation=='median' else 7), 'Missing selected filter option'
            assert len(spins) >= 5, 'Missing radius or range controls'
            assert spins[1].get_value_iface().set_current_value(4096), 'Radius rejected test value'
            report['radius'] = spins[1].get_value_iface().get_current_value()
            report['selection'] = choices[1].get_selection_iface().get_selected_child(0).get_name()
            click(named(dialog, 'Apply to range'))
            cancel = wait_for(lambda: find(app, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Cancel' and visible(n)), 'cancellable worker progress')
            click(cancel)
            wait_for(lambda: not visible(cancel), 'worker stopped after cancellation')
            assert not dialog.get_name().endswith('*'), 'Cancelled job changed mask revisions'
            assert named(dialog, 'Redo').get_state_set().contains(Atspi.StateType.SENSITIVE), 'Cancellation discarded edit history'
            report['checks'].append(args.cancel_operation + ' background range job can be cancelled without changing masks or history')
        click(find(dialog, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Close'))
        report['checks'].append('open archive from menu, edit, Undo and close')
        audit = os.environ.get('SLA_NETWORK_AUDIT_FILE')
        if audit:
            assert 'offline_network_audit.so' in Path(f'/proc/{process.pid}/maps').read_text()
            assert not (Path(audit).read_text() if Path(audit).exists() else ''), 'Workflow attempted a network connection'
            report['checks'].append('network audit loaded; no network attempts')
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'; report['error'] = str(error)
        if 'app' in locals(): report['tree'] = dump(app)
        raise
    finally:
        process.terminate()
        try: process.wait(timeout=10)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        (args.output / 'result.json').write_text(json.dumps(report, indent=2))
        print(json.dumps({key: value for key, value in report.items() if key != "tree"}, indent=2))


if __name__ == '__main__': main()
