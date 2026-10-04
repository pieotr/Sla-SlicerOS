#!/usr/bin/env python3
"""Generate PCB/calibration layers in GTK using an isolated preset seed."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import zipfile
from printer_wizard_smoke import Atspi, find, named, wait_for, click


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--advanced-pcb', action='store_true', help='Exercise moire, thermal and G85 slot geometry through the PCB GUI')
    parser.add_argument('--multiple-pcb', action='store_true', help='Verify merged and separate Gerber files retain common registration')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='raster-tools-', dir=args.output.resolve()))
    datadir = directory / 'presets'
    shutil.copytree(args.seed, datadir)
    gerber = directory / 'copper.gbr'
    gerber.write_text('%FSLAX24Y24*%\n%MOMM*%\n%ADD10R,10X8*%\nD10*X100000Y100000D03*M02*\n')
    drill = directory / 'drill.drl'
    drill.write_text('M48\nMETRIC,LZ,000.000\nT1C2\n%\nT1\nX010000Y010000\nM30\n')
    if args.advanced_pcb:
        gerber.write_text('%FSLAX24Y24*%\n%MOMM*%\n'
            '%AMTARGET*6,0,0,6,0.4,0.4,2,0.3,7,0*%\n'
            '%AMTHERMAL*7,0,0,8,4,1,45*%\n'
            '%ADD10TARGET*%\n%ADD11THERMAL*%\n'
            'D10*X100000Y100000D03*D11*X220000Y100000D03*M02*\n')
        drill.write_text('M48\nMETRIC,TZ,000.000\nT1C1\n%\nT1\nX20.0Y10.0G85X24.0Y10.0\nM30\n')
    second_gerber = directory / 'second.gbr'
    second_gerber.write_text('%FSLAX24Y24*%\n%MOMM*%\n%ADD10R,4X2*%\nD10*X220000Y100000D03*M02*\n')
    if args.advanced_pcb:
        second_gerber.write_text('%FSLAX24Y24*%\n%MOMM*%\n%ADD10R,4X2*%\nD10*X360000Y100000D03*M02*\n')
    report = {'datadir': str(datadir), 'checks': []}
    with (directory / 'application.log').open('w') as log:
        process = subprocess.Popen([str(args.binary.resolve()), '--datadir', str(datadir)],
            stdout=log, stderr=subprocess.STDOUT,
            env=dict(os.environ, GDK_BACKEND='x11', NO_AT_BRIDGE='0'))
    def visible(node): return node.get_state_set().contains(Atspi.StateType.SHOWING)
    def find_visible(node, label): return find(node, lambda n: n.get_name() == label and visible(n))
    def collect(node, role):
        result = []
        def walk(n):
            if n.get_role_name() == role and visible(n): result.append(n)
            for index in range(n.get_child_count()): walk(n.get_child_at_index(index))
        walk(node)
        return result
    def discard_generated(dialog):
        assert named(dialog, 'Apply to sliced result') is None, 'Ephemeral job offers misleading Apply'
        assert named(dialog, 'Export generated layers') is not None
        click(find(dialog, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Close'))
        click(wait_for(lambda: find_visible(app, 'Yes'), 'discard generated layers confirmation'))
    def export_generated(dialog, name):
        choices = collect(dialog, 'combo box')
        assert len(choices) == 3, 'Missing generated export formats'
        formats = choices[2].get_selection_iface()
        option = find(choices[2], lambda n: n.get_role_name() in ('menu item', 'list item') and
            n.get_name().endswith('(.uvj)'))
        assert option is not None, 'Missing UVJ export option'
        assert formats.select_child(option.get_index_in_parent()), 'UVJ selection rejected'
        wait_for(lambda: '(.uvj)' in formats.get_selected_child(0).get_name(), 'UVJ format selection')
        click(named(dialog, 'Export generated layers'))
        save = wait_for(lambda: find(app, lambda n: n.get_name() == 'Save edited layers' and visible(n)), 'generated output chooser')
        filename = collect(save, 'text')[0]
        path = directory / (name + '.uvj')
        assert filename.get_editable_text_iface().set_text_contents(str(path)), 'Output path rejected'
        click(named(save, 'Save'))
        wait_for(lambda: path.is_file() and path.stat().st_size > 0, 'generated UVJ file', timeout=40)
        with zipfile.ZipFile(path) as archive:
            settings = json.loads(archive.read('config.json'))
            layers = settings['Layers']
            assert len(layers) == settings['Properties']['Size']['Layers']
            for index in range(len(layers)):
                assert archive.read(f'slice/{index:08d}.png').startswith(b'\x89PNG\r\n\x1a\n')
        report.setdefault('exports', []).append({'path': str(path), 'layers': len(layers), 'settings': settings})
        # Atomic rename can make the file visible just before the GUI worker
        # completion callback marks the generated document saved.
        wait_for(lambda: not find(app, lambda n: n.get_name() == 'Export layers' and visible(n)), 'completed export dialog')
        close = wait_for(lambda: find(dialog, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Close' and
            n.get_state_set().contains(Atspi.StateType.SENSITIVE)), 'editor enabled after export')
        click(close)
        wait_for(lambda: not visible(dialog), 'close exported generated job')
        return settings
    try:
        app = wait_for(lambda: find(Atspi.get_desktop(0), lambda n:
            n.get_role_name() == 'application' and n.get_process_id() == process.pid), 'application')
        if os.environ.get('SLA_NETWORK_AUDIT_FILE'):
            maps = Path(f'/proc/{process.pid}/maps').read_text()
            assert 'offline_network_audit.so' in maps, 'Network audit was not loaded into the application'
            report['network_audit_loaded'] = True
        wait_for(lambda: named(app, 'SLA material:'), 'loaded settings')
        forbidden = find(app, lambda n: n.get_role_name() == 'menu item' and
            ('G-code' in n.get_name() or 'Toolpaths' in n.get_name()))
        assert forbidden is None, 'FFF action remains in the SLA menu: ' + (forbidden.get_name() if forbidden else '')
        report['checks'].append('SLA menus contain no G-code conversion, viewer or toolpath export actions')

        click(wait_for(lambda: find(app, lambda n: n.get_role_name() == 'menu item' and
            n.get_name() == 'PCB exposure'), 'PCB menu'))
        options = wait_for(lambda: find(app, lambda n: n.get_role_name() in ('dialog', 'frame') and
            n.get_name() == 'PCB exposure' and visible(n)), 'PCB settings')
        paths = collect(options, 'text')
        # Spin controls also contain text children. The two file path inputs
        # precede numeric controls and are identified by their explicit names.
        assert len(paths) >= 2, 'Missing Gerber/drill input controls'
        for field, path in zip(paths[:2], (str(gerber) + ('\n' + str(second_gerber) if args.multiple_pcb else ''), drill)):
            assert field.get_editable_text_iface().set_text_contents(str(path)), 'File path rejected'
        click(named(options, 'Generate PCB layers'))
        dialog = wait_for(lambda: find(app, lambda n: n.get_role_name() in ('dialog', 'frame') and
            n.get_name() == 'Layer editor' and visible(n)), 'generated PCB editor')
        report['checks'].append('PCB menu, Gerber + Excellon input and one-layer job in editor')
        if args.advanced_pcb:
            report['checks'].append('moire, thermal and Excellon G85 slot imported through the PCB GUI')
        pcb = export_generated(dialog, 'pcb')
        assert pcb['Properties']['Size']['Layers'] == 1
        assert pcb['Layers'][0]['Exposure']['LightOnTime'] == 30
        assert pcb['Layers'][0]['Exposure']['LiftHeight'] == 0
        report['checks'].append('PCB exports through native chooser to UVJ with stationary 30 s exposure')
        if args.multiple_pcb:
            click(wait_for(lambda: find(app, lambda n: n.get_role_name() == 'menu item' and
                n.get_name() == 'PCB exposure'), 'second PCB menu'))
            options = wait_for(lambda: find(app, lambda n: n.get_name() == 'PCB exposure' and visible(n)), 'separate PCB settings')
            paths = collect(options, 'text')
            assert paths[0].get_editable_text_iface().set_text_contents(str(gerber)+'\n'+str(second_gerber))
            assert paths[1].get_editable_text_iface().set_text_contents(str(drill))
            click(named(options, 'Merge Gerber files into one exposure'))
            click(named(options, 'Generate PCB layers'))
            dialog = wait_for(lambda: find(app, lambda n: n.get_name() == 'Layer editor' and visible(n)), 'separate PCB editor')
            separate = export_generated(dialog, 'pcb-separate')
            assert len(separate['Layers']) == 2
            for index, layer in enumerate(separate['Layers']):
                assert abs(layer['Z']-(index+1)*separate['Properties']['Size']['LayerHeight']) < 1e-6
                assert layer['Exposure']['LightOnTime'] == 30 and layer['Exposure']['LiftHeight'] == 0
            # Source PNG pixels prove shared centering rather than separate
            # centering of each board. Pillow is only an independent test reader.
            from PIL import Image
            from io import BytesIO
            with zipfile.ZipFile(directory/'pcb.uvj') as archive:
                combined = Image.open(BytesIO(archive.read('slice/00000000.png'))).convert('L').tobytes()
            with zipfile.ZipFile(directory/'pcb-separate.uvj') as archive:
                masks = [Image.open(BytesIO(archive.read(f'slice/{i:08d}.png'))).convert('L').tobytes() for i in range(2)]
            assert all(any(mask) for mask in masks)
            assert not any(a and b for a,b in zip(*masks)), 'Copper registrations overlap unexpectedly'
            assert bytes(max(a,b) for a,b in zip(*masks)) == combined, 'Separate masks differ from merged registration'
            report['checks'].append('multiple Gerbers: merged mask equals pixel union of separately exported aligned masks; shared drills and stationary parameters')
        click(wait_for(lambda: find(app, lambda n: n.get_role_name() == 'menu item' and
            n.get_name() == 'Printer calibration'), 'calibration menu'))
        options = wait_for(lambda: find(app, lambda n: n.get_role_name() in ('dialog', 'frame') and
            n.get_name() == 'Printer calibration' and visible(n)), 'calibration settings')
        choices = collect(options, 'combo box')
        assert choices[0].get_selection_iface().select_child(1), 'Missing grayscale pattern'
        spins = collect(options, 'spin button')
        assert len(spins) == 7, 'Missing calibration numeric controls'
        spin = spins[4]
        assert spin.get_state_set().contains(Atspi.StateType.SENSITIVE), 'Grayscale controls stay disabled'
        assert spin.get_value_iface().set_current_value(4), 'Grayscale steps rejected value'
        click(named(options, 'Generate calibration layers'))
        dialog = wait_for(lambda: find(app, lambda n: n.get_role_name() in ('dialog', 'frame') and
            n.get_name() == 'Layer editor' and visible(n)), 'generated calibration editor')
        report['checks'].append('calibration menu, live pattern choice, grayscale controls and native masks')
        discard_generated(dialog)
        report['checks'].append('generated jobs require export or explicit discard; scene Apply is absent')
        click(wait_for(lambda: find(app, lambda n: n.get_role_name() == 'menu item' and
            n.get_name() == 'Printer calibration'), 'reopen calibration menu'))
        options = wait_for(lambda: find(app, lambda n: n.get_role_name() in ('dialog', 'frame') and
            n.get_name() == 'Printer calibration' and visible(n)), 'second calibration settings')
        choices = collect(options, 'combo box')
        assert choices[0].get_selection_iface().select_child(1)
        spins = collect(options, 'spin button')
        assert spins[4].get_value_iface().set_current_value(4)
        click(named(options, 'Generate calibration layers'))
        dialog = wait_for(lambda: find(app, lambda n: n.get_role_name() in ('dialog', 'frame') and
            n.get_name() == 'Layer editor' and visible(n)), 'calibration export editor')
        calibration = export_generated(dialog, 'grayscale')
        assert calibration['Properties']['Size']['Layers'] == 20
        assert all(abs(layer['Z'] - (i + 1) * .05) < 1e-6 for i, layer in enumerate(calibration['Layers']))
        with zipfile.ZipFile(directory / 'grayscale.uvj') as archive:
            first = archive.read('slice/00000000.png')
            assert all(archive.read(f'slice/{i:08d}.png') == first for i in range(20)), 'Repeated calibration masks differ'
        report['checks'].append('grayscale calibration exports all 20 masks with exact Z and unchanged PNGs')
        audit = os.environ.get('SLA_NETWORK_AUDIT_FILE')
        if audit:
            attempts = Path(audit).read_text() if Path(audit).exists() else ''
            assert not attempts.strip(), 'Unexpected network attempts: ' + attempts
            report['checks'].append('no Internet socket or DNS attempts in the exercised workflow')
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'; report['error'] = str(error)
        if 'app' in locals():
            report['visible'] = []
            def inspect(n, depth=0):
                if depth>18: return
                if visible(n): report['visible'].append([n.get_role_name(), n.get_name()])
                for index in range(n.get_child_count()): inspect(n.get_child_at_index(index), depth+1)
            inspect(app)
        raise
    finally:
        process.terminate()
        try: process.wait(timeout=10)
        except subprocess.TimeoutExpired: process.kill(); process.wait()
        (args.output / 'result.json').write_text(json.dumps(report, indent=2))
        print(json.dumps({key: value for key, value in report.items() if key not in ('visible', 'exports')}, indent=2))


if __name__ == '__main__': main()
