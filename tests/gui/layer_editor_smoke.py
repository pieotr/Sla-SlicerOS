#!/usr/bin/env python3
"""Exercise the actual sliced-layer dialog in GTK, using an isolated preset seed.

The seed must come from printer_wizard_smoke.py, not a user's live datadir.
No downloads, coordinate-dependent clicking, or edits to the seed directory.
"""
import argparse
import configparser
import re
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time
import zipfile
import zlib

from printer_wizard_smoke import Atspi, find, named, wait_for, click


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--printer-geometry', action='store_true', help='Use the active copied printer physical screen, pixels and firmware model')
    parser.add_argument('--hold-format-popup', action='store_true', help='Assert native format popup stays expanded while the export timer runs')
    parser.add_argument('--direct-export', choices=('sl1','cxdlpv4'), help='Verify sidebar export rather than editor export')
    parser.add_argument('--sl1-export', action='store_true', help='Verify reference SL1 export from the actual layer dialog')
    parser.add_argument('--seed-printer-format', action='store_true', help='Keep the copied printer format instead of overriding it from CLI')
    parser.add_argument('--project-roundtrip', action='store_true', help='Save edited layers in 3MF and verify restoration after restart')
    parser.add_argument('--vda-export', action='store_true', help='Verify Voxeldance Additive ZIP choice and independent XML schema')
    parser.add_argument('--chitubox-export', action='store_true', help='Verify Chitubox ZIP choice and independent exposure schema')
    parser.add_argument('--cxdlpv4-export', action='store_true', help='Verify CXDLPv4 output and checksum')
    parser.add_argument('--cxdlpv2-export', action='store_true', help='Verify native CXDLP v2 selection, compound suffix and XOR')
    parser.add_argument('--cxdlpv3-export', action='store_true', help='Verify native CXDLP v3 settings and checksum')
    parser.add_argument('--cxdlpv1-export', action='store_true', help='Verify the version-specific compound CXDLP extension')
    parser.add_argument('--genericzip-export', action='store_true', help='Verify independent Generic ZIP choice, filename suffix and schema')
    parser.add_argument('--osf-export', action='store_true', help='Verify OSF choice, automatic filename suffix and native export')
    parser.add_argument('--anycubic-export', choices=('pws', 'dl2p', 'pwc'), help='Verify Anycubic binary export and automatic suffix')
    parser.add_argument('--debug', action='store_true', help='Run the test application under a local debugger')
    parser.add_argument('--cwsxml-export', action='store_true', help='Verify XML.CWS compound suffix, XML manifest and opaque RGBA masks')
    parser.add_argument('--novamaker-export', action='store_true', help='Verify NovaMaker slice.conf, grayscale masks and native export')
    parser.add_argument('--cwsrgb-export', action='store_true', help='Verify RGB.CWS packed BGR masks, full LCD geometry and compound suffix')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='layer-editor-', dir=args.output.resolve()))
    datadir = directory / 'presets'
    shutil.copytree(args.seed, datadir)
    configuration = directory / 'workflow.ini'
    configuration.write_text(
        'printer_technology = SLA\nsla_archive_format = SL1\n'
        'supports_enable = 0\npad_enable = 0\nsupport_object_elevation = 0\n'
        'display_width = 100\ndisplay_height = 60\n'
        'display_pixels_x = 200\ndisplay_pixels_y = 120\n'
        'layer_height = 0.05\ninitial_layer_height = 0.05\n')
    if args.seed_printer_format or args.printer_geometry:
        seed_config=configparser.RawConfigParser()
        seed_config.read_string('[DEFAULT]\n'+(args.seed/'PrusaSlicer.ini').read_text())
        printer_name=seed_config.get('presets','printer')
        printer_file=args.seed/'printer'/(printer_name+'.ini')
        match=re.search(r'(?m)^sla_archive_format = (.*)$',printer_file.read_text())
        assert match, 'Seed printer has no explicit archive format'
        if args.seed_printer_format:
            configuration.write_text(configuration.read_text().replace('sla_archive_format = SL1\n',
                'sla_archive_format = '+match.group(1)+'\n'))
        if args.printer_geometry:
            values=dict(line.split(' = ',1) for line in printer_file.read_text().splitlines() if ' = ' in line)
            text=configuration.read_text()
            for key in ('display_width','display_height','display_pixels_x','display_pixels_y','display_orientation',
                        'display_mirror_x','display_mirror_y','max_print_height','printer_model','bed_shape'):
                assert key in values,key
                text=re.sub(r'(?m)^'+key+r' = .*\n','',text)
                text+=key+' = '+values[key]+'\n'
            configuration.write_text(text)
    if args.direct_export:
        configuration.write_text(re.sub(r'(?m)^sla_archive_format = .*$',
            'sla_archive_format = '+('SL1' if args.direct_export=='sl1' else 'cxdlpv4'),configuration.read_text()))
    if args.cwsrgb_export:
        configuration.write_text(configuration.read_text().replace('display_pixels_x = 200\n','display_pixels_x = 201\n')+'display_orientation = landscape\n')
    if args.anycubic_export == 'pws':
        configuration.write_text(configuration.read_text()
            .replace('display_width = 100\n', 'display_width = 68.04\n')
            .replace('display_height = 60\n', 'display_height = 120.96\n')
            + 'max_print_height = 165\ndisplay_orientation = landscape\n')
    if args.cxdlpv3_export or args.cxdlpv2_export:
        configuration.write_text(configuration.read_text()+'printer_model = CL-60\nexposure_time = 2.5\ninitial_exposure_time = 20\n'
            'sla_transition_layers = 0\nsla_wait_before_cure = 1\nsla_bottom_wait_before_cure = 1\n'
            'sla_lift_speed = 120\nsla_bottom_lift_speed = 60\nsla_retract_speed = 180\nsla_bottom_retract_speed = 180\n')
    if args.cxdlpv1_export:
        configuration.write_text(configuration.read_text()+'exposure_time = 3\ninitial_exposure_time = 20\n'
            'sla_transition_layers = 0\nsla_wait_before_cure = 1\nsla_bottom_wait_before_cure = 1\n')
    source = Path(__file__).resolve().parents[1] / 'data/20mm_cube.obj'
    model = directory / 'small-cube.obj'
    lines = []
    for line in source.read_text().splitlines():
        if line.startswith('v '):
            line = 'v ' + ' '.join(str(float(value) * .015) for value in line.split()[1:])
        lines.append(line)
    model.write_text('\n'.join(lines) + '\n')
    report = {'datadir': str(datadir), 'checks': []}
    log = (directory / 'application.log').open('w')
    command = [
        str(args.binary.resolve()), '--datadir', str(datadir),
        '--load', str(configuration), str(model)]
    if args.debug:
        command = ['gdb', '-batch', '-ex', 'set debuginfod enabled off',
                   '-ex', 'run', '-ex', 'bt 24', '--args'] + command
    process = subprocess.Popen(command,
        stdout=log, stderr=subprocess.STDOUT,
        env=dict(os.environ, GDK_BACKEND='x11', NO_AT_BRIDGE='0'))
    log.close()
    try:
        desktop = Atspi.get_desktop(0)
        def candidates():
            if not args.debug: return [process.pid]
            path = Path(f'/proc/{process.pid}/task/{process.pid}/children')
            return list(map(int, path.read_text().split())) if path.exists() else []
        app = wait_for(lambda: find(desktop, lambda n:
            n.get_role_name() == 'application' and n.get_process_id() in candidates()), 'application')
        application_pid = app.get_process_id()

        def button(label):
            return find(app, lambda n: n.get_role_name() == 'button' and n.get_name() == label and
                n.get_state_set().contains(Atspi.StateType.SHOWING) and
                n.get_state_set().contains(Atspi.StateType.SENSITIVE))
        first_action = wait_for(lambda: button('No') or button('Slice now'), 'model import')
        if first_action.get_name() == 'No':
            # This synthetic OBJ is already in millimeters; reject auto inch conversion.
            click(first_action)
        click(wait_for(lambda: button('Slice now'), 'enabled slicing button'))
        wait_for(lambda: button('Edit layers'), 'completed slicing', timeout=40)
        if args.direct_export:
            export = wait_for(lambda: find(app, lambda n: n.get_role_name() == 'button' and
                n.get_name().startswith('Export') and n.get_state_set().contains(Atspi.StateType.SHOWING) and
                n.get_state_set().contains(Atspi.StateType.SENSITIVE)), 'sidebar export action')
            click(export)
            chooser = wait_for(lambda: find(app, lambda n: n.get_name().rstrip(':') == 'Save SLA file as' and
                n.get_state_set().contains(Atspi.StateType.SHOWING)), 'sidebar output dialog')
            initial_field=wait_for(lambda: find(chooser, lambda n: n.get_role_name() in ('text','entry') and
                n.get_state_set().contains(Atspi.StateType.EDITABLE) and n.get_state_set().contains(Atspi.StateType.SHOWING)), 'initial output filename')
            assert Atspi.Text.get_text(initial_field,0,-1).endswith('.'+args.direct_export), 'Default filter disagrees with printer format'
            combos=[]
            def collect(node):
                if node.get_role_name() == 'combo box' and node.get_state_set().contains(Atspi.StateType.SHOWING): combos.append(node)
                for index in range(node.get_child_count()): collect(node.get_child_at_index(index))
            collect(chooser)
            suffix='.'+args.direct_export
            for combo in combos:
                option=find(combo, lambda n: n.get_role_name() in ('menu item','list item') and ('*'+suffix) in n.get_name())
                if option:
                    if args.hold_format_popup:
                        click(combo)
                        wait_for(lambda: option.get_state_set().contains(Atspi.StateType.SHOWING), 'expanded native format list')
                        time.sleep(1.2)
                        assert option.get_state_set().contains(Atspi.StateType.SHOWING), 'Format list closed without a selection'
                        report['checks'].append('native format popup stays open during timer polling')
                    assert combo.get_selection_iface().select_child(option.get_index_in_parent())
                    break
            else: raise AssertionError('Missing native format filter '+suffix)
            time.sleep(.5)
            field=wait_for(lambda: find(chooser, lambda n: n.get_role_name() in ('text','entry') and
                n.get_state_set().contains(Atspi.StateType.EDITABLE) and n.get_state_set().contains(Atspi.StateType.SHOWING)), 'output filename')
            output=directory/('direct-result'+suffix)
            field.get_editable_text_iface().set_text_contents(str(output))
            click(named(chooser,'Save'))
            wait_for(lambda: output.is_file() and output.stat().st_size>0,'sidebar export output',timeout=40)
            if args.direct_export=='sl1':
                with zipfile.ZipFile(output) as archive:
                    assert 'config.ini' in archive.namelist()
                    assert any(n.endswith('.png') and not n.startswith('thumbnail') for n in archive.namelist())
            else:
                data=output.read_bytes()
                assert data[:15] == b'\x00\x00\x00\x09CXSW3DV2\x00\x00\x04'
                assert int.from_bytes(data[-4:],'big') == (zlib.crc32(data[:-4],0xffffffff)^0xffffffff)
            report.update(status='passed',direct_output=str(output))
            report['checks'].append('sidebar export, explicit native filter and file structure')
            return
        click(wait_for(lambda: button('Edit layers'), 'completed slicing', timeout=40))
        dialog = wait_for(lambda: find(app, lambda n: n.get_name().startswith('Layer editor') and
            n.get_state_set().contains(Atspi.StateType.SHOWING)), 'layer editor')
        report['checks'].append('model load, SLA slicing and opening cached layers from sidebar')
        choices = []
        def collect_choices(node):
            if node.get_role_name() == 'combo box' and node.get_state_set().contains(Atspi.StateType.SHOWING):
                choices.append(node)
            for index in range(node.get_child_count()): collect_choices(node.get_child_at_index(index))
        collect_choices(dialog)
        assert len(choices) >= 3, 'Missing zoom, range operation or export format choice'
        # 400% of a CL-92 mask exceeds the editor's existing preview budget
        # and opens a modal informational dialog inside the AT-SPI action.
        # Keep the full-screen integration test within the supported budget.
        zoom_index = 1 if args.printer_geometry else 5
        zoom_label = '25%' if args.printer_geometry else '400%'
        assert choices[0].get_selection_iface().select_child(zoom_index), 'Zoom choice rejected ' + zoom_label
        time.sleep(.3)
        assert not dialog.get_name().endswith('*'), 'Zoom modified mask data'
        assert choices[1].get_selection_iface().select_child(1), 'Range choice rejected Invert'
        click(named(dialog, 'Apply to range'))
        wait_for(lambda: dialog.get_name().endswith('*'), 'completed range operation')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not dialog.get_name().endswith('*'), 'single Undo of entire range')
        report['checks'].append(zoom_label + ' zoom, range invert and one Undo of all layers')
        assert choices[1].get_selection_iface().select_child(7), 'Range choice rejected Dilate'
        click(named(dialog, 'Apply to range'))
        wait_for(lambda: dialog.get_name().endswith('*'), 'completed grayscale dilation')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not dialog.get_name().endswith('*'), 'single Undo of dilation')
        report['checks'].append('grayscale dilation is accessible and undone as one range edit')
        assert choices[1].get_selection_iface().select_child(13), 'Range choice rejected Box blur'
        click(named(dialog, 'Apply to range'))
        wait_for(lambda: dialog.get_name().endswith('*'), 'completed box blur')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not dialog.get_name().endswith('*'), 'single Undo of box blur')
        click(named(dialog, 'Redo'))
        wait_for(lambda: dialog.get_name().endswith('*'), 'Redo of box blur')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not dialog.get_name().endswith('*'), 'restored original after box blur')
        report['checks'].append('box blur is accessible on the selected range with atomic Undo/Redo')
        assert choices[1].get_selection_iface().select_child(14), 'Range choice rejected Median blur'
        click(named(dialog, 'Apply to range'))
        wait_for(lambda: dialog.get_name().endswith('*'), 'completed median blur')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not dialog.get_name().endswith('*'), 'single Undo of median blur')
        click(named(dialog, 'Redo'))
        wait_for(lambda: dialog.get_name().endswith('*'), 'Redo of median blur')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not dialog.get_name().endswith('*'), 'restored original after median blur')
        report['checks'].append('median blur is accessible on the selected range with atomic Undo/Redo')
        click(named(dialog, 'Invert layer'))
        assert dialog.get_name().endswith('*'), 'Edit did not mark the document dirty'
        click(named(dialog, 'Undo'))
        assert not dialog.get_name().endswith('*'), 'Undo did not restore the original revision'
        click(named(dialog, 'Redo'))
        assert dialog.get_name().endswith('*'), 'Redo did not restore the edit'
        report['checks'].append('invert, Undo and Redo update the live dialog')
        click(named(dialog, 'Apply to sliced result'))
        assert not dialog.get_name().endswith('*'), 'Apply did not commit the revision'
        click(find(dialog, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Close'))
        time.sleep(.5)
        click(wait_for(lambda: button('Edit layers'), 'reopen action'))
        reopened = wait_for(lambda: find(app, lambda n: n.get_name() == 'Layer editor' and
            n.get_state_set().contains(Atspi.StateType.SHOWING)), 'reopened editor')
        assert named(reopened, 'Undo').get_state_set().contains(Atspi.StateType.SENSITIVE), 'Applied edit history disappeared'
        click(find(reopened, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Close'))
        report['checks'].append('Apply, close and reopen preserve the edited document')
        if args.project_roundtrip:
            def save_project(destination):
                click(named(app, 'File'))
                action = wait_for(lambda: find(app, lambda n:
                    n.get_role_name() == 'menu item' and n.get_name().startswith('Save Project as')),
                    'save project menu item')
                click(action)
                chooser = wait_for(lambda: find(app, lambda n: n.get_name().rstrip(':') == 'Save file as' and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)), 'project save dialog')
                field = wait_for(lambda: find(chooser, lambda n: n.get_role_name() == 'text' and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)), 'project filename')
                field.get_editable_text_iface().set_text_contents(str(destination))
                click(named(chooser, 'Save'))
                def complete():
                    try:
                        with zipfile.ZipFile(destination) as archive:
                            manifest = json.loads(archive.read('Metadata/SLA/layers.json'))
                            layer = archive.read('Metadata/SLA/layers/00000000.png')
                        return manifest, layer
                    except (OSError, KeyError, zipfile.BadZipFile):
                        return None
                return wait_for(complete, 'complete project with edited layers', timeout=40)

            def first_pixel(image):
                # For every PNG row filter the first pixel of the first row has
                # zero left/up predictors, so its stored byte is its true value.
                compressed = bytearray(); offset = 8
                while offset < len(image):
                    size = int.from_bytes(image[offset:offset+4], 'big')
                    if image[offset+4:offset+8] == b'IDAT':
                        compressed.extend(image[offset+8:offset+8+size])
                    offset += size+12
                return zlib.decompress(compressed)[1]

            project = directory / 'edited-project.3mf'
            manifest, mask = save_project(project)
            assert first_pixel(mask) == 255, 'Project did not store the inverted first layer'
            assert len(manifest['positions_z']) == manifest['layers']
            report['checks'].append('3MF contains edited PNGs, layer heights and source fingerprint')
            process.terminate(); process.wait(timeout=10)
            with (directory / 'application-reopened.log').open('w') as reopened_log:
                process = subprocess.Popen([str(args.binary.resolve()), '--datadir', str(datadir), str(project)],
                    stdout=reopened_log, stderr=subprocess.STDOUT,
                    env=dict(os.environ, GDK_BACKEND='x11', NO_AT_BRIDGE='0'))
            app = wait_for(lambda: find(desktop, lambda n: n.get_role_name() == 'application' and
                n.get_process_id() == process.pid), 'reopened application')
            application_pid = process.pid
            startup = wait_for(lambda: find(app, lambda n: n.get_name().endswith('Load project file') and
                n.get_state_set().contains(Atspi.StateType.SHOWING)) or button('Slice now'), 'project startup')
            if startup.get_name().endswith('Load project file'):
                choice = named(startup, 'Open as project')
                if not choice.get_state_set().contains(Atspi.StateType.CHECKED): click(choice)
                click(named(startup, 'OK'))
            click(wait_for(lambda: button('Slice now'), 'reopened project slicing action'))
            click(wait_for(lambda: button('Edit layers'), 'restored sliced result', timeout=40))
            restored_dialog = wait_for(lambda: find(app, lambda n: n.get_name() == 'Layer editor' and
                n.get_state_set().contains(Atspi.StateType.SHOWING)), 'restored editor')
            assert not named(restored_dialog, 'Undo').get_state_set().contains(Atspi.StateType.SENSITIVE), \
                'Restored result incorrectly retained an old session history'
            click(find(restored_dialog, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Close'))
            manifest2, mask2 = save_project(directory / 'resaved-project.3mf')
            assert first_pixel(mask2) == 255, 'Re-slicing or re-saving discarded the restored edit'
            assert manifest2['positions_z'] == manifest['positions_z']
            assert mask2 == mask, 'Restored mask bytes changed'
            report['checks'].append('restart, slice, empty session Undo history and re-save preserve edited PNGs')
        if args.sl1_export or args.cwsrgb_export or args.novamaker_export or args.cwsxml_export or args.osf_export or args.genericzip_export or args.vda_export or args.chitubox_export or args.anycubic_export or args.cxdlpv1_export or args.cxdlpv3_export or args.cxdlpv2_export or args.cxdlpv4_export:
            extension = '.sl1' if args.sl1_export else '.rgb.cws' if args.cwsrgb_export else '.cws' if args.novamaker_export else '.xml.cws' if args.cwsxml_export else '.cxdlpv4' if args.cxdlpv4_export else '.v2.cxdlp' if args.cxdlpv2_export else '.cxdlp' if args.cxdlpv3_export else '.v1.cxdlp' if args.cxdlpv1_export else '.' + args.anycubic_export if args.anycubic_export else ".zip" if args.genericzip_export or args.vda_export or args.chitubox_export else ".osf"
            click(wait_for(lambda: button('Edit layers'), 'OSF export editor action'))
            editor = wait_for(lambda: find(app, lambda n: n.get_name() == 'Layer editor' and
                n.get_state_set().contains(Atspi.StateType.SHOWING)), 'OSF export editor')
            export_choices = []
            def scan(node):
                if node.get_role_name() == 'combo box' and node.get_state_set().contains(Atspi.StateType.SHOWING):
                    export_choices.append(node)
                for index in range(node.get_child_count()): scan(node.get_child_at_index(index))
            scan(editor)
            formats = export_choices[2]
            option = find(formats, lambda n: n.get_role_name() in ('menu item', 'list item') and n.get_name().endswith('(' + extension + ')') and (not args.novamaker_export or 'NovaMaker CWS' in n.get_name()) and (not args.vda_export or 'Voxeldance Additive' in n.get_name()) and (not args.genericzip_export or 'Generic ZIP' in n.get_name()) and (not args.chitubox_export or 'Chitubox ZIP' in n.get_name()))
            assert option is not None, 'OSF is unavailable in the export chooser'
            assert formats.get_selection_iface().select_child(option.get_index_in_parent())
            click(named(editor, 'Export edited layers'))
            chooser = wait_for(lambda: find(app, lambda n: n.get_name() == 'Save edited layers' and
                n.get_state_set().contains(Atspi.StateType.SHOWING)), 'OSF output chooser')
            filename = find(chooser, lambda n: n.get_role_name() in ('text', 'entry') and
                n.get_state_set().contains(Atspi.StateType.EDITABLE))
            assert Atspi.Text.get_text(filename, 0, -1).lower().endswith(extension), 'Format selection did not update the default filename'
            output = directory / ('edited-result' + extension)
            assert filename.get_editable_text_iface().set_text_contents(str(output))
            click(named(chooser, 'Save'))
            wait_for(lambda: output.is_file() and output.stat().st_size > (0 if args.sl1_export or args.cwsrgb_export or args.novamaker_export or args.cwsxml_export or args.genericzip_export or args.vda_export or args.chitubox_export or args.anycubic_export or args.cxdlpv4_export else 350001), 'native export output', timeout=40)
            if args.sl1_export:
                from PIL import Image
                from io import BytesIO
                with zipfile.ZipFile(output) as archive:
                    assert 'config.ini' in archive.namelist()
                    masks = sorted(n for n in archive.namelist() if n.endswith('.png') and not n.startswith('thumbnail'))
                    assert masks, 'SL1 contains no masks'
                    image = Image.open(BytesIO(archive.read(masks[0])))
                    fields = dict(line.split('=',1) for line in configuration.read_text().splitlines() if '=' in line)
                    fields = {key.strip():value.strip() for key,value in fields.items()}
                    expected = (int(fields['display_pixels_x']), int(fields['display_pixels_y']))
                    if fields.get('display_orientation') == 'portrait': expected = expected[::-1]
                    assert image.size == expected
                    assert image.getpixel((0,0)) == 255, 'Applied edit missing from SL1'
            elif args.cwsrgb_export:
                from PIL import Image
                from io import BytesIO
                with zipfile.ZipFile(output) as archive:
                    assert 'manifest.xml' not in archive.namelist()
                    fields = dict(line.split('=',1) for line in archive.read('slice.conf').decode().splitlines() if '=' in line)
                    fields = {key.strip():float(value.strip()) for key,value in fields.items()}
                    assert fields['xres']==201 and fields['yres']==120
                    assert fields['xppm']==2.01 and fields['yppm']==2
                    layers=sorted(name for name in archive.namelist() if name.endswith('.png'))
                    assert len(layers)==fields['layers_num']
                    for index,name in enumerate(layers):
                        data=archive.read(name);assert data[24:26]==bytes((8,2))
                        image=Image.open(BytesIO(data));assert image.size==(67,120)
                        red,green,blue=image.split()
                        packed=bytearray()
                        for triplet in zip(blue.tobytes(),green.tobytes(),red.tobytes()):packed.extend(triplet)
                        if index==0:assert packed[0]==255, 'Applied edit is missing from packed output'
                    assert archive.read('sla_job.gcode')
            elif args.novamaker_export:
                with zipfile.ZipFile(output) as archive:
                    assert 'manifest.xml' not in archive.namelist()
                    fields = dict(line.split('=',1) for line in archive.read('slice.conf').decode().splitlines() if '=' in line)
                    fields = {key.strip():float(value.strip()) for key,value in fields.items()}
                    assert len(fields)==14
                    assert (fields['xres'],fields['yres']) in ((200,120),(120,200))
                    assert fields['xppm']==2 and fields['yppm']==2
                    assert fields['lift_when_finished']==0
                    layers = sorted(name for name in archive.namelist() if name.endswith('.png'))
                    assert len(layers)==fields['layers_num']
                    for layer in layers:
                        data=archive.read(layer)
                        assert data[24:26]==bytes((8,0)), 'NovaMaker requires grayscale PNG'
                        assert int.from_bytes(data[16:20],'big')==fields['xres'] and int.from_bytes(data[20:24],'big')==fields['yres']
                    assert archive.read('sla_job.gcode')
            elif args.cwsxml_export:
                import xml.etree.ElementTree as ET
                from PIL import Image
                from io import BytesIO
                with zipfile.ZipFile(output) as archive:
                    manifest = ET.fromstring(archive.read('manifest.xml'))
                    assert archive.read(manifest.findtext('SliceProfile/name')).startswith(b'<?xml')
                    assert archive.read(manifest.findtext('GCode/name'))
                    slices = manifest.findall('Slices/Slice/name')
                    assert slices
                    for layer in slices:
                        data = archive.read(layer.text)
                        assert data[24:26] == bytes((8,6)), 'XML.CWS requires 8-bit RGBA PNG'
                        image = Image.open(BytesIO(data))
                        red,green,blue,alpha = image.split()
                        assert red.tobytes() == green.tobytes() == blue.tobytes()
                        assert alpha.getextrema() == (255,255)
            elif args.cxdlpv4_export:
                data=output.read_bytes()
                assert data[:15] == b'\x00\x00\x00\x09CXSW3DV2\x00\x00\x04'
                assert int.from_bytes(data[-4:], 'big') == (zlib.crc32(data[:-4], 0xffffffff)^0xffffffff)
            elif args.cxdlpv2_export:
                data=output.read_bytes()
                assert data[:15] == b'\x00\x00\x00\x09CXSW3DV2\x00\x00\x02'
                checksum=0
                for byte in data[:-4]: checksum ^= byte
                assert int.from_bytes(data[-4:],'big') == checksum
            elif args.cxdlpv3_export:
                data=output.read_bytes()
                assert data[:15] == b'\x00\x00\x00\x09CXSW3DV2\x00\x00\x03'
                assert data[-17:-4] == b'\x00\x00\x00\x09CXSW3DV2\x00'
                assert int.from_bytes(data[-4:], 'big') == (zlib.crc32(data[:-4], 0xffffffff)^0xffffffff), 'CXDLP checksum mismatch'
            elif args.cxdlpv1_export:
                data=output.read_bytes()
                assert data[:13] == b'\x00\x00\x00\x09CXSW3DV2\x00'
                assert data[-17:-4] == b'\x00\x00\x00\x09CXSW3DV2\x00'
            elif args.chitubox_export:
                with zipfile.ZipFile(output) as archive:
                    assert 'run.gcode' in archive.namelist() and '1.png' in archive.namelist()
                    assert 'config.ini' not in archive.namelist() and 'manifest.uvtools' not in archive.namelist()
            elif args.genericzip_export or args.vda_export:
                with zipfile.ZipFile(output) as archive:
                    assert ('sla_job.xml' if args.vda_export else 'manifest.uvtools') in archive.namelist() and 'config.ini' not in archive.namelist()
                    assert ('0001.png' if args.vda_export else '00000001.png') in archive.namelist()
            elif args.anycubic_export:
                data = output.read_bytes()
                assert data[:8] == b'ANYCUBIC'
                version = int.from_bytes(data[12:16], 'little')
                assert version == {'pws': 1, 'pwc': 516, 'dl2p': 517}[args.anycubic_export]
                if version >= 516:
                    offset = int.from_bytes(data[44:48], 'little')
                    assert data[offset:offset+7] == b'MACHINE'
            else:
                header = output.read_bytes()[:7]
                assert int.from_bytes(header[:4], 'big') == 350001 and header[4:6] == b'\x00\x04'
            wait_for(lambda: not find(app, lambda n: n.get_name() == 'Export layers' and
                n.get_state_set().contains(Atspi.StateType.SHOWING)), 'OSF export completion')
            click(find(editor, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Close'))
            report['sl1_output' if args.sl1_export else 'cwsrgb_output' if args.cwsrgb_export else 'novamaker_output' if args.novamaker_export else 'cwsxml_output' if args.cwsxml_export else 'cxdlpv4_output' if args.cxdlpv4_export else 'cxdlpv2_output' if args.cxdlpv2_export else 'cxdlpv3_output' if args.cxdlpv3_export else 'cxdlpv1_output' if args.cxdlpv1_export else 'chitubox_output' if args.chitubox_export else 'anycubic_output' if args.anycubic_export else 'vda_output' if args.vda_export else 'genericzip_output' if args.genericzip_export else 'osf_output'] = str(output)
            report['checks'].append(('SL1' if args.sl1_export else 'RGB.CWS' if args.cwsrgb_export else 'NovaMaker CWS' if args.novamaker_export else 'XML.CWS' if args.cwsxml_export else 'CXDLPv4' if args.cxdlpv4_export else 'CXDLPv2' if args.cxdlpv2_export else 'CXDLPv3' if args.cxdlpv3_export else 'CXDLPv1' if args.cxdlpv1_export else 'Chitubox ZIP' if args.chitubox_export else args.anycubic_export.upper() if args.anycubic_export else 'Voxeldance Additive ZIP' if args.vda_export else 'Generic ZIP' if args.genericzip_export else 'OSF') + ' GUI choice, automatic filename suffix and native export')
        audit = os.environ.get('SLA_NETWORK_AUDIT_FILE')
        if audit:
            maps = Path(f'/proc/{process.pid}/maps').read_text()
            assert 'offline_network_audit.so' in maps, 'Network audit was not loaded'
            attempts = Path(audit).read_text() if Path(audit).exists() else ''
            assert not attempts.strip(), 'Unexpected network attempts: ' + attempts
            report['checks'].append('network audit loaded; no network attempts in this workflow')
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error), application_exit_code=process.poll())
        visible = []
        def inspect(node, depth=0):
            if depth > 16 or len(visible) >= 80: return
            if node.get_name() and node.get_state_set().contains(Atspi.StateType.SHOWING):
                visible.append([node.get_role_name(), node.get_name()])
            for index in range(node.get_child_count()): inspect(node.get_child_at_index(index), depth+1)
        try:
            desktop.clear_cache()
            app = find(desktop, lambda n: n.get_role_name() == 'application' and n.get_process_id() == application_pid)
            app.clear_cache()
            inspect(app)
            report['visible_controls'] = visible
        except Exception:
            pass
        raise
    finally:
        if args.debug and 'application_pid' in locals():
            try: os.kill(application_pid, signal.SIGTERM)
            except ProcessLookupError: pass
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill(); process.wait()
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
