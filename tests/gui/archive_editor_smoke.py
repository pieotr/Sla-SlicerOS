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


def chitubox(data):
    """Independent addressed v1–v5 masks; normalize only mutable table/stat fields."""
    import struct
    import sys
    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'sla_print'))
    from verify_ctb_v1_fixture import decode
    from compare_chitubox_binary import crypt_ctb
    u32=lambda at:struct.unpack_from('<I',data,at)[0]
    magic,version=u32(0),u32(4)
    if magic==0x12fd0107:
        from ctb_encrypted_wire import parse
        wire=parse(data)
        return wire['metadata'],(False,1),wire['masks']
    assert magic in (0x12fd0019,0x12fd0086,0x12fd0106,0xff220810) and 1 <= version <= 5
    binary=magic==0x12fd0019;planes=(1 if version==1 else u32(92)) if binary else 1
    count,width,height,table=u32(68),u32(52),u32(56),u32(64)
    metadata=bytearray(data[:112]);masks=[]
    for at in (u32(60),u32(72)):
        if at:metadata.extend(data[at:at+32]);metadata.extend(data[u32(at+8):u32(at+8)+u32(at+12)])
    if u32(84):
        params=bytearray(data[u32(84):u32(84)+u32(88)]);params[20:32]=bytes(12);metadata.extend(params)
    if version >= 3:
        # Preserve the complete addressed metadata prefix, excluding statistics.
        metadata=bytearray(data[:table])
        if u32(84): metadata[u32(84)+20:u32(84)+32]=bytes(12)
    for i in range(count):
        levels=bytearray(width*height)
        for plane in range(planes):
            at=table+36*(plane*count+i);row=bytearray(data[at:at+36]);row[12:20]=bytes(8);metadata.extend(row)
            if version >= 3:
                extended=u32(at+12)-84
                for field in (0,4,8,12,16,20,24):
                    assert u32(extended+field)==u32(at+field), 'Inconsistent duplicated CTB definition'
                assert u32(extended+36)==84+u32(at+16)
                metadata.extend(data[extended+40:extended+84])
            payload=data[u32(at+12):u32(at+12)+u32(at+16)]
            if binary:
                cursor=0
                for token in payload:
                    run=token&127;assert run and cursor+run<=len(levels)
                    if token&128:
                        for n in range(cursor,cursor+run):levels[n]+=1
                    cursor+=run
                assert cursor==len(levels)
            else:levels=decode(crypt_ctb(payload,u32(100),i),width*height)
        masks.append(bytes((value*(256//planes)-1 if value else 0) for value in levels) if binary else bytes(levels))
    return bytes(metadata),(binary,planes),masks


def anycubic(data, extension):
    """Independent local AnycubicFile layout/RLE expectations for GUI exports."""
    import struct
    def u32(at): return struct.unpack_from('<I', data, at)[0]
    assert data[:8] == b'ANYCUBIC'
    version, header, table = u32(12), u32(20), u32(36)
    width, height, planes, count = u32(header+60), u32(header+64), u32(header+56), u32(table+16)
    parameters, masks = [], []
    for layer in range(count):
        at = table+20+32*layer
        parameters.append(data[at+8:at+24] + data[at+28:at+32])
        payload = data[u32(at):u32(at)+u32(at+4)]
        pixels, cursor = [0]*(width*height), 0
        for plane in range(planes if extension.lower() == '.pws' else 1):
            position = 0
            while position < len(pixels):
                value = payload[cursor]; cursor += 1
                color, run = (value>>7, value&127) if extension.lower()=='.pws' else (value>>4, value&15)
                if extension.lower()!='.pws' and color in (0,15):
                    run = (run<<8)|payload[cursor]; cursor += 1
                assert run and position+run <= len(pixels)
                for i in range(position,position+run):
                    pixels[i] = pixels[i]+color if extension.lower()=='.pws' else color*17
                position += run
        assert cursor == len(payload)
        if extension.lower()=='.pws': pixels = [value*(256//planes)-1 if value else 0 for value in pixels]
        masks.append(bytes(pixels))
    return (version,width,height,planes), parameters, masks


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inspect', action='store_true')
    parser.add_argument('--cancel-range', action='store_true')
    parser.add_argument('--cancel-operation', choices=('dilate','median'), default='dilate')
    parser.add_argument('--save-image', action='store_true', help='Save an inverted standalone image in its original format')
    parser.add_argument('--save-png', action='store_true', help='Save an inverted standalone PNG through the native chooser')
    parser.add_argument('--save-archive', action='store_true', help='Save edited archive and check that non-layer entries remain unchanged')
    parser.add_argument('--cancel-open', action='store_true', help='Cancel native archive import and verify no document is published')
    parser.add_argument('--convert-archive', action='store_true', help='Convert edited CTB v4 to v5 without marking the source saved')
    parser.add_argument('--cancel-convert', action='store_true', help='Cancel conversion and retain source edits/history')
    parser.add_argument('--conversion-settings', type=Path, help='Explicit selected SLA printer settings for conversion')
    parser.add_argument('--cancel-save', action='store_true', help='Cancel archive save and verify no output or history changes')
    parser.add_argument('--profile-portrait', action='store_true', help='Rotate the selected physical profile axes to match a portrait archive')
    parser.add_argument('--profile-mismatch', action='store_true', help='Reject a selected printer with mismatching raster dimensions')
    parser.add_argument('--physical-profile', action='store_true', help='Explicitly use the selected matching printer/material to replace resin estimates')
    parser.add_argument('--save-anycubic', action='store_true', help='Save native Anycubic and independently verify every mask and original parameters')
    parser.add_argument('--edit-anycubic-motion', action='store_true', help='Edit stored lift distance and speed with unchanged global EXTRA')
    parser.add_argument('--edit-layer-parameters', action='store_true', help='Edit Z/exposure for the second layer through the typed parameter dialog')
    parser.add_argument('--invalid-layer-parameters', action='store_true', help='Check invalid exposure rejection before the valid parameter edit')
    parser.add_argument('--edit-secondary', action='store_true', help='Select and edit the SUBIMGS image through the GUI')
    parser.add_argument('--edit-secondary-range', action='store_true', help='Invert every selected SUBIMGS image as one undo step')
    parser.add_argument('--preserve-anycubic-subimages', action='store_true', help='Independently check independent SUBIMGS and linked-mask updates')
    parser.add_argument('--save-chitubox', action='store_true', help='Save native CTB/CBDDLP/Photon v1/v2 and independently check masks/metadata')
    parser.add_argument('--save-binary', action='store_true', help='Save CXDLP v1/v2/v3/v4 and independently compare pixels and preserved metadata')
    parser.add_argument('--expected-error', default='PNG chunk checksum mismatch', help='Exact expected import error message')
    parser.add_argument('--expect-invalid', action='store_true', help='Check checksum-error handling without opening an editor or terminating the app')
    args = parser.parse_args()
    if args.convert_archive and not args.conversion_settings:
        parser.error('--convert-archive requires --conversion-settings')
    if args.cancel_convert and not args.convert_archive:
        parser.error('--cancel-convert requires --convert-archive')
    args.output.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='archive-editor-', dir=args.output.resolve()))
    datadir = directory / 'presets'
    shutil.copytree(args.seed, datadir)
    command = [str(args.binary.resolve()), '--datadir', str(datadir)]
    if args.conversion_settings:
        command += ['--load', str(args.conversion_settings.resolve())]
    if args.physical_profile or args.profile_mismatch:
        configuration = directory / 'physical-profile.ini'
        configuration.write_text('printer_technology = SLA\ndisplay_pixels_x = 8\ndisplay_pixels_y = 4\n'
            'display_width = 16\ndisplay_height = 4\ndisplay_orientation = landscape\n'
            'material_density = 1.2\nbottle_cost = 100\nbottle_volume = 500\nsla_archive_format = pwc\n')
        if args.profile_portrait:
            configuration.write_text(configuration.read_text().replace('display_orientation = landscape\n', 'display_orientation = portrait\n'))
        if args.profile_mismatch:
            configuration.write_text(configuration.read_text().replace('display_pixels_x = 8\n', 'display_pixels_x = 7\n'))
        command += ['--load', str(configuration)]
    report = {'datadir': str(datadir), 'checks': []}
    application_env = dict(os.environ, GDK_BACKEND='x11', NO_AT_BRIDGE='0')
    audit_library = os.environ.get('SLA_APP_NETWORK_AUDIT_LIBRARY')
    if audit_library:
        application_env['LD_PRELOAD'] = str(Path(audit_library).resolve())
    with (directory / 'application.log').open('w') as log:
        process = subprocess.Popen(command,
            stdout=log, stderr=subprocess.STDOUT,
            env=application_env)
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
            assert row_node.get_component_iface().grab_focus(), 'File row rejected focus'
            click(named(chooser, 'Open'))
        if args.cancel_open:
            progress=wait_for(lambda:find(app,lambda n:n.get_name()=='Open layer archive' and
                visible(n) and n.get_role_name() in ('dialog','frame') and named(n,'Cancel') is not None
                and named(n,'Open') is None),'cancellable import')
            cancelled_at=time.monotonic();click(named(progress,'Cancel'))
            wait_for(lambda:not visible(progress),'import worker stopped')
            report['cancel_latency_seconds']=round(time.monotonic()-cancelled_at,3)
            assert process.poll() is None
            assert named(app,'Save edited archive') is None and named(app,'Save edited image') is None
            assert named(app,'SLA material:') is not None
            report['checks'].append('Cancel import stops the worker and publishes no partial document; main app remains usable')
            audit=os.environ.get('SLA_NETWORK_AUDIT_FILE')
            if audit:
                assert 'offline_network_audit.so' in Path(f'/proc/{process.pid}/maps').read_text()
                assert not (Path(audit).read_text() if Path(audit).exists() else '')
                report['checks'].append('network audit loaded; no network attempts')
            report['status']='passed';return
        if args.expect_invalid:
            error = wait_for(lambda: find(app, lambda n: n.get_name() == args.expected_error and visible(n)), 'invalid archive/image message')
            click(wait_for(lambda: find(app, lambda n: n.get_role_name() == 'button' and n.get_name() == 'OK' and visible(n)), 'error acknowledgement'))
            wait_for(lambda: not visible(error), 'dismissed import error')
            assert process.poll() is None, 'Invalid input terminated the application'
            assert named(app, 'Save edited image') is None and named(app, 'Save edited archive') is None, 'Invalid input opened an editable document'
            report['checks'].append('invalid input shows the expected error and leaves the application running without an editable document')
            audit = os.environ.get('SLA_NETWORK_AUDIT_FILE')
            if audit:
                assert 'offline_network_audit.so' in Path(f'/proc/{process.pid}/maps').read_text()
                assert not (Path(audit).read_text() if Path(audit).exists() else '')
                report['checks'].append('network audit loaded; no network attempts')
            report['status'] = 'passed'
            return
        def embedded_editor():
            back = find(app,lambda n:n.get_role_name()=='button' and n.get_name()=='Back to 3D' and visible(n))
            return app if back else None
        dialog = wait_for(embedded_editor, 'archive in the permanent Sliced editor')
        assert not find(app,lambda n:n.get_role_name() in ('dialog','frame') and
            n.get_name().startswith('Layer editor') and visible(n)), 'Archive menu opened a modal editor'
        def changed():
            return find(app,lambda n:visible(n) and n.get_name().startswith('Layer document: modified.')) is not None
        if args.save_anycubic:
            note = find(dialog, lambda n: n.get_name().startswith('Anycubic edits keep'))
            assert note is not None, 'Missing original-format edit guidance'
            assert 'zero values do not provide material properties' in note.get_name()
            report['checks'].append('original format, quantization and resin-estimate limits explained in editor')
        if args.profile_mismatch:
            toggle = named(dialog, 'Use selected printer and material for resin estimates')
            click(toggle)
            error = wait_for(lambda: find(app, lambda n: n.get_name() == 'Selected printer resolution differs from the imported layer raster' and visible(n)), 'profile resolution error')
            click(wait_for(lambda: find(app, lambda n: n.get_name() == 'OK' and n.get_role_name() == 'button' and visible(n)), 'profile error acknowledgment'))
            wait_for(lambda: not visible(error), 'profile error dismissed')
            toggle.clear_cache()
            assert not toggle.get_state_set().contains(Atspi.StateType.CHECKED), 'Invalid binding remains checked'
            report['checks'].append('mismatching printer rejected, checkbox reset and original estimates retained')
        if args.physical_profile:
            toggle = named(dialog, 'Use selected printer and material for resin estimates')
            assert toggle is not None, 'Missing explicit profile binding'
            click(toggle)
            summary = wait_for(lambda: find(dialog, lambda n: n.get_name().startswith('Resin estimates:')
                and '1.2' in n.get_name()), 'matching printer/material summary')
            assert 'max Z' in summary.get_name(), 'Selected printer height missing from profile summary'
            report['checks'].append('selected maximum Z visible in profile summary')
            report['checks'].append('explicit printer/material context accepted and shown in editor')
        if args.edit_layer_parameters:
            layer=find(dialog,lambda n:n.get_role_name()=='spin button')
            assert layer.get_value_iface().set_current_value(2), 'Layer 2 rejected'
            click(named(dialog,'Layer parameters'))
            settings=wait_for(lambda:find(app,lambda n:n.get_name().startswith('Layer parameters — layer 2') and visible(n)),'layer parameter dialog')
            def parameter_inputs():
                inputs=[]
                def visit(node):
                    if visible(node) and node.get_role_name() in ('text','entry') and node.get_state_set().contains(Atspi.StateType.EDITABLE):inputs.append(node)
                    for i in range(node.get_child_count()):visit(node.get_child_at_index(i))
                visit(settings)
                expected=7 if args.save_chitubox and '.v3.ctb' in args.archive.name else 14 if args.save_chitubox else 4
                assert len(inputs)==expected, 'Unexpected format capabilities';return inputs
            inputs=parameter_inputs()
            if args.invalid_layer_parameters:
                assert inputs[1].get_editable_text_iface().set_text_contents('-1')
                click(named(settings,'Apply parameters'))
                error=wait_for(lambda:find(app,lambda n:n.get_name()=='Layer parameters must be finite nonnegative values representable by the format' and visible(n)),'invalid exposure message')
                assert not changed(), 'Invalid exposure changes revision'
                click(wait_for(lambda:find(app,lambda n:n.get_role_name()=='button' and n.get_name()=='OK' and visible(n)),'invalid parameter acknowledgement'))
                wait_for(lambda:visible(settings),'parameter dialog returns after rejection');inputs=parameter_inputs()
                report['checks'].append('invalid exposure rejected with unchanged document/history and correction remains possible')
            assert inputs[0].get_editable_text_iface().set_text_contents('0.16')
            assert inputs[1].get_editable_text_iface().set_text_contents('7.25')
            if args.edit_anycubic_motion:
                assert inputs[2].get_editable_text_iface().set_text_contents('6')
                assert inputs[3].get_editable_text_iface().set_text_contents('80.3')
            if args.save_chitubox:
                numbers=['1.25','3','75','150','128','2','180','1.5','90','0.75','0.5','0.25']
                for input,value in zip(inputs[2:],numbers):assert input.get_editable_text_iface().set_text_contents(value)
            click(named(settings,'Apply parameters'))
            wait_for(lambda:not visible(settings),'parameters applied')
            report['checks'].append('CTB supported parameters edited with explicit units' if args.save_chitubox else 'Anycubic parameters edited with units; global motion context readonly')
        if args.edit_secondary or args.edit_secondary_range:
            plane=find(dialog,lambda n:n.get_role_name()=='combo box' and n.get_name() in ('Main mask','SUBIMGS mask'))
            assert plane is not None, 'Missing selectable SUBIMGS plane'
            assert plane.get_selection_iface().select_child(1), 'SUBIMGS selection rejected'
            wait_for(lambda:plane.get_selection_iface().get_selected_child(0).get_name()=='SUBIMGS mask','selected SUBIMGS mask')
            assert not changed(), 'Selection alone edits the document'
            assert find(dialog,lambda n:n.get_name().startswith(('Independent masks:','Shared mask:'))) is not None
            report['checks'].append('SUBIMGS selectable; shared/independent behavior explained; selection alone leaves revision clean')
        if args.edit_secondary_range:
            operation=find(dialog,lambda n:n.get_role_name()=='combo box' and n.get_name()=='Threshold')
            assert operation.get_selection_iface().select_child(1), 'Missing Invert range operation'
            click(named(dialog,'Apply to range'))
        elif not args.edit_layer_parameters:
            click(named(dialog, 'Invert layer'))
        wait_for(lambda: changed(), 'archive edit')
        click(named(dialog, 'Undo'))
        wait_for(lambda: changed() == bool(args.physical_profile), 'archive undo retains only unsaved context')
        if args.edit_secondary or args.edit_secondary_range:
            assert plane.get_selection_iface().select_child(0)
            assert not changed()
            assert plane.get_selection_iface().select_child(1)
            report['checks'].append('Undo restores both masks; main/secondary switching preserves history')
        assert named(dialog, 'Save edited image' if args.save_png or args.save_image else 'Save edited archive') is not None
        if args.save_png or args.save_image or args.save_archive or args.save_binary or args.save_chitubox or args.save_anycubic or args.cancel_save:
            click(named(dialog, 'Redo'))
            wait_for(lambda: changed(), 'restored PNG edit')
            click(named(dialog, 'Save edited image' if args.save_png or args.save_image else 'Save edited archive'))
            save = wait_for(lambda: find(app, lambda n: n.get_name() == 'Save edited layers' and visible(n)
                and n.get_role_name() != 'menu item'), 'PNG save chooser')
            field = find(save, lambda n: n.get_role_name() in ('text', 'entry') and visible(n)
                and n.get_state_set().contains(Atspi.StateType.EDITABLE))
            assert field is not None, 'Missing PNG filename field'
            extension = ('.cxdlpv4' if args.archive.name.endswith('.cxdlpv4') else '.v1.cxdlp' if args.archive.name.endswith('.v1.cxdlp') else '.v2.cxdlp' if args.archive.name.endswith('.v2.cxdlp') else '.cxdlp') if args.save_binary else '.png' if args.save_png else args.archive.suffix if args.save_image else '.rgb.cws' if args.archive.name.lower().endswith('.rgb.cws') else '.xml.cws' if args.archive.name.lower().endswith('.xml.cws') else args.archive.suffix
            if args.save_chitubox:
                extension=next((suffix for suffix in ('.v4.encrypted.ctb','.encrypted.ctb','.gktwo.ctb','.v5.ctb','.v4.ctb','.v3.ctb','.v1.ctb','.v1.cbddlp','.v1.photon')
                                if args.archive.name.lower().endswith(suffix)),args.archive.suffix)
            assert Atspi.Text.get_text(field, 0, -1).endswith(extension), 'Default container suffix is missing'
            output = directory / ('edited-mask' + extension)
            assert field.get_editable_text_iface().set_text_contents(str(output))
            click(named(save, 'Save'))
            if args.cancel_save:
                progress=wait_for(lambda:find(app,lambda n:n.get_name()=='Save layer archive' and visible(n)
                    and n.get_role_name() in ('dialog','frame')),'cancellable archive save')
                cancelled_at=time.monotonic();click(named(progress,'Cancel'))
                wait_for(lambda:not visible(progress),'save worker stopped')
                report['cancel_latency_seconds']=round(time.monotonic()-cancelled_at,3)
                assert not output.exists(),'Cancelled save published an output'
                assert not list(directory.glob('.sla-edit-*.tmp')),'Cancelled save left staging files'
                assert changed(),'Cancelled save marked the document clean'
                assert named(dialog,'Undo').get_state_set().contains(Atspi.StateType.SENSITIVE)
                click(named(dialog,'Undo'));wait_for(lambda:not changed(),'history survives cancelled save')
                report['checks'].append('Cancel archive save preserves dirty state and Undo, publishes no output and cleans staging files')
            else:
                wait_for(lambda: output.exists() and not changed(), 'saved clean PNG')
            if args.cancel_save:
                pass
            elif args.save_image:
                from PIL import Image, ImageOps
                with Image.open(args.archive) as image: expected=ImageOps.invert(image.convert('L')).tobytes()
                with Image.open(output) as image: actual=image.convert('L').tobytes()
                assert actual==expected,'Standalone image edit changed unrelated pixels'
                report['checks'].append('standalone import, Undo/Redo, original image suffix and exact saved inverted pixels')
            elif args.save_png:
                data = output.read_bytes()
                assert data[:8] == b'\x89PNG\r\n\x1a\n' and data[25] == 0, 'Output is not a grayscale PNG'
                report['checks'].append('Redo, automatic PNG suffix, raw grayscale PNG save and clean revision')
            elif args.save_anycubic:
                original = anycubic(args.archive.read_bytes(), args.archive.suffix)
                saved = anycubic(output.read_bytes(), output.suffix)
                if args.edit_layer_parameters:
                    import struct
                    assert original[0]==saved[0] and original[2]==saved[2], 'Parameter edit changed masks/version'
                    heights=[]
                    for i,(before,after) in enumerate(zip(original[1],saved[1])):
                        if args.edit_anycubic_motion and i==1:
                            distance,speed=struct.unpack_from('<ff',after,0);assert distance==6 and abs(speed*60-80.3)<1e-5
                        else:assert before[:8]==after[:8], 'Unedited motion changed'
                        assert before[16:]==after[16:], 'Unknown padding changed'
                        exposure,height=struct.unpack_from('<ff',after,8);heights.append(height)
                        assert exposure==(7.25 if i==1 else struct.unpack_from('<f',before,8)[0])
                    positions=[sum(heights[:i+1]) for i in range(len(heights))]
                    assert all(abs(value-expected)<1e-7 for value,expected in zip(positions,(.1,.16,.2)))
                    if args.edit_anycubic_motion:
                        import sys
                        sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'sla_print'))
                        from verify_anycubic_motion import verify
                        verify(args.archive,output,gui=True)
                    report['checks'].append('saved Anycubic parameters independently exact; all pixels/global EXTRA/unknown bytes preserved')
                else:
                    assert original[:2] == saved[:2], 'Anycubic version or per-layer motion/exposure/Z changed'
                if args.edit_layer_parameters:
                    pass
                elif args.edit_secondary or args.edit_secondary_range:
                    import sys,struct
                    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'sla_print'))
                    from verify_anycubic_subimages import planes
                    raw=args.archive.read_bytes();old_planes=planes(raw)
                    def word(data,at):return struct.unpack_from('<I',data,at)[0]
                    expected=[]
                    for i,mask in enumerate(original[2]):
                        sub=old_planes[2]+24+44*i;main=word(raw,36)+20+32*i
                        shared=(word(raw,sub),word(raw,sub+4))==(word(raw,main),word(raw,main+4))
                        expected.append(bytes(255-p for p in mask) if shared and (i==0 or args.edit_secondary_range) else mask)
                    assert saved[2]==expected, 'Secondary edits changed unrelated main masks'
                else:
                    assert saved[2][0] == bytes(255-value for value in original[2][0])
                    assert saved[2][1:] == original[2][1:], 'Unedited Anycubic masks changed'
                if args.physical_profile:
                    import struct
                    data = output.read_bytes()
                    header = struct.unpack_from('<I', data, 20)[0]
                    table = struct.unpack_from('<I', data, 36)[0]
                    volume = sum(sum(pixel != 0 for pixel in mask)*struct.unpack_from('<f', data, table+20+i*32+20)[0]
                                 for i, mask in enumerate(saved[2]))*16*4/32/1000
                    for offset, expected in [(52, volume), (68, volume*1.2), (72, volume*.2)]:
                        actual = struct.unpack_from('<f', data, header+offset)[0]
                        assert abs(actual-expected) < 1e-6, (offset, actual, expected)
                    report['checks'].append('profile-derived volume, weight and cost independently verified in saved header')
                if args.preserve_anycubic_subimages:
                    import sys,struct
                    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'sla_print'))
                    from verify_anycubic_subimages import planes
                    a,b=args.archive.read_bytes(),output.read_bytes();old,new=planes(a),planes(b)
                    def word(data,at):return struct.unpack_from('<I',data,at)[0]
                    main_table=word(a,36)
                    for i in range(len(old[1])):
                        sub=old[2]+24+44*i;main=main_table+20+32*i
                        shared=(word(a,sub),word(a,sub+4))==(word(a,main),word(a,main+4))
                        secondary_changed=(args.edit_secondary or args.edit_secondary_range) and (i==0 or args.edit_secondary_range)
                        expected=bytes(255-p for p in old[1][i]) if secondary_changed else new[0][i] if shared else old[1][i]
                        assert new[1][i]==expected, 'Saved secondary mask differs from selected GUI edit'
                        assert a[sub+12:sub+44]==b[sub+12:sub+44], 'Opaque SUBIMGS fields changed'
                        if not shared and not secondary_changed:assert a[sub:sub+44]==b[sub:sub+44], 'Unedited independent SUBIMGS record changed'
                    report['checks'].append('selected main/secondary edits exact; independent masks and padding preserved; shared masks stay linked')
                report['checks'].append('native Anycubic import, original suffix, Undo/Redo and save; independent version/parameters/pixels exact')
            elif args.save_chitubox:
                if args.edit_layer_parameters:
                    import sys
                    sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'sla_print'))
                    if ".encrypted.ctb" in args.archive.name:
                        from verify_ctb_encrypted_parameters import verify
                    else:
                        from verify_ctb_parameters import verify
                    verify(args.archive,output)
                    report['checks'].append('native CTB parameters/duplicate records exact; masks and unknown bytes preserved')
                else:
                    original=chitubox(args.archive.read_bytes());saved=chitubox(output.read_bytes())
                    assert original[:2]==saved[:2], 'Chitubox parameters or previews changed'
                    binary,planes=original[1];step=256//planes
                    def quantized(value):
                        if not binary:return 0 if value<2 else value|1
                        if value and (value+1)%step==0 and (value+1)//step<=planes:return value
                        count=planes if value==255 else min(planes-1,(value+1)//step)
                        return count*step-1 if count else 0
                    assert saved[2][0]==bytes(quantized(255-value) for value in original[2][0])
                    assert saved[2][1:]==original[2][1:], 'Unedited Chitubox masks changed'
                    report['checks'].append('native Chitubox import, Undo/Redo, original compound suffix and save; independent masks/metadata/previews exact')
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
                    from PIL import Image
                    from io import BytesIO
                    masks=[name for name in changed if name.lower().endswith('.png')]
                    assert len(masks)==1,'Save changed an unexpected number of masks'
                    if args.archive.suffix.lower() in ('.sl1','.sl1s'):
                        import configparser,re
                        assert set(changed)<=set(masks)|{'config.ini','config.json'},'Unknown SL1 entries changed'
                        def ini(data):
                            parsed=configparser.RawConfigParser();parsed.read_string('[DEFAULT]\n'+data.decode());return dict(parsed.defaults())
                        profile=ini(original.read('prusaslicer.ini'))
                        volume=0
                        names=sorted(n for n in saved.namelist() if re.match(r'^[^/\\]*[0-9]{5,}\.png$',n,re.I))
                        for index,name in enumerate(names):
                            with Image.open(BytesIO(saved.read(name))) as image:
                                pixels=image.convert('L').tobytes();area=float(profile['display_width'])*float(profile['display_height'])/(image.width*image.height)
                            thickness=float(profile['initial_layer_height'] if index==0 else profile['layer_height'])
                            volume+=sum(bool(v) for v in pixels)*area*thickness/1000
                        a,b=ini(original.read('config.ini')),ini(saved.read('config.ini'))
                        assert abs(float(b.pop('usedmaterial'))-volume)<1e-8
                        a.pop('usedmaterial');assert a==b,'SL1 settings changed beyond resin volume'
                        if 'config.json' in original.namelist():
                            a=json.loads(original.read('config.json'));b=json.loads(saved.read('config.json'))
                            old=a.pop('usedMaterial');new=b.pop('usedMaterial')
                            assert a==b and isinstance(new,str)==isinstance(old,str)
                            assert abs(float(new)-volume)<1e-8
                        report['checks'].append('SL1 resin volume independently recomputed; all remaining settings/entries preserved')
                    else:
                        assert len(changed)==1,'Save changed metadata'
                    before = Image.open(BytesIO(original.read(masks[0]))).convert('L').tobytes()
                    after = Image.open(BytesIO(saved.read(masks[0]))).convert('L').tobytes()
                    assert after == bytes(255-value for value in before), 'Edited mask pixels changed unexpectedly'
                report['checks'].append('Redo, automatic container suffix, archive save preserves metadata/commands and all edited pixels')
            if not args.cancel_save:report['saved_output'] = str(output)
        if args.convert_archive:
            click(named(dialog, 'Redo'))
            wait_for(lambda: changed(), 'edited source before conversion')
            click(named(dialog, 'Capture selected SLA settings'))
            click(named(dialog, 'Convert archive'))
            conversion = wait_for(lambda: find(app, lambda n: n.get_name() == 'Convert layer archive' and visible(n)), 'conversion choices')
            choice = find(conversion, lambda n: n.get_role_name() == 'combo box')
            option = find(choice, lambda n: n.get_role_name() in ('menu item', 'list item') and '.v5.ctb' in n.get_name())
            assert option is not None, 'CTB v5 preserving conversion is missing'
            assert choice.get_selection_iface().select_child(option.get_index_in_parent())
            click(named(conversion, 'Convert'))
            save = wait_for(lambda: find(app, lambda n: n.get_name() == 'Save converted layers' and visible(n)), 'conversion filename')
            field = find(save, lambda n: n.get_role_name() in ('text', 'entry') and visible(n) and
                n.get_state_set().contains(Atspi.StateType.EDITABLE))
            assert Atspi.Text.get_text(field, 0, -1).endswith('.v5.ctb'), 'Wrong automatic conversion suffix'
            converted = directory / 'converted.v5.ctb'
            assert field.get_editable_text_iface().set_text_contents(str(converted))
            if args.cancel_convert:
                assert named(save, 'Save').get_action_iface().do_action(0)
            else:
                click(named(save, 'Save'))
            if args.cancel_convert:
                progress = wait_for(lambda: find(app, lambda n: n.get_name() == 'Convert layers' and visible(n) and
                    n.get_role_name() in ('dialog', 'frame')), 'cancellable conversion')
                began = time.monotonic();click(named(progress, 'Cancel'))
                wait_for(lambda: not visible(progress), 'conversion worker stopped')
                report['cancel_latency_seconds'] = round(time.monotonic() - began, 3)
                assert not converted.exists(), 'Cancelled conversion published output'
                assert not list(directory.glob('.sla-export-*')) and not list(directory.glob('.sla-edit-*')), 'Conversion left staging'
                report['checks'].append('conversion Cancel preserves the source and cleans all staging')
            else:
                wait_for(lambda: converted.exists() and converted.stat().st_size > 0, 'converted native output', timeout=40)
                wait_for(lambda: not find(app, lambda n: n.get_name() == 'Convert layers' and visible(n)), 'conversion joined')
                report['converted_output'] = str(converted)
                report['checks'].append('explicit CTB v5 conversion and automatic suffix produce a separate output')
            assert changed(), 'Conversion marked source saved'
            assert named(dialog, 'Undo').get_state_set().contains(Atspi.StateType.SENSITIVE), 'Conversion lost source history'
            click(named(dialog, 'Undo'))
            assert named(dialog, 'Redo').get_state_set().contains(Atspi.StateType.SENSITIVE), 'Conversion damaged Undo/Redo'
            report['checks'].append('conversion keeps source dirty and preserves Undo/Redo independently of output')
        if args.cancel_range:
            choices, spins = [], []
            def controls(node):
                if visible(node):
                    if node.get_role_name() == 'combo box': choices.append(node)
                    if node.get_role_name() == 'spin button': spins.append(node)
                for index in range(node.get_child_count()): controls(node.get_child_at_index(index))
            controls(dialog)
            range_operation=find(dialog,lambda n:n.get_role_name()=='combo box' and n.get_name()=='Threshold')
            assert range_operation.get_selection_iface().select_child(14 if args.cancel_operation=='median' else 7), 'Missing selected filter option'
            assert len(spins) >= 5, 'Missing radius or range controls'
            assert spins[1].get_value_iface().set_current_value(4096), 'Radius rejected test value'
            report['radius'] = spins[1].get_value_iface().get_current_value()
            report['selection'] = range_operation.get_selection_iface().get_selected_child(0).get_name()
            click(named(dialog, 'Apply to range'))
            cancel = wait_for(lambda: find(app, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Cancel' and visible(n)), 'cancellable worker progress')
            click(cancel)
            wait_for(lambda: not visible(cancel), 'worker stopped after cancellation')
            assert not changed(), 'Cancelled job changed mask revisions'
            assert named(dialog, 'Redo').get_state_set().contains(Atspi.StateType.SENSITIVE), 'Cancellation discarded edit history'
            report['checks'].append(args.cancel_operation + ' background range job can be cancelled without changing masks or history')
        click(named(dialog,'Back to 3D'))
        wait_for(lambda:not visible(named(app,'Back to 3D')), 'imported document hidden in 3D')
        if args.convert_archive:
            click(named(app,'Sliced'))
            wait_for(lambda:visible(named(app,'Back to 3D')), 'retained conversion source')
            assert changed() and named(app,'Redo').get_state_set().contains(Atspi.StateType.SENSITIVE)
            click(named(app,'Back to 3D'))
        report['checks'].append('open archive from menu into Sliced, edit, Undo and return to 3D without discarding document')
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
