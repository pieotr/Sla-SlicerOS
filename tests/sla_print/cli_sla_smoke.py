#!/usr/bin/env python3
"""Exercise the installed executable's SLA-only CLI with isolated local files."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import zipfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    executable = args.executable.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    repository = Path(__file__).resolve().parents[2]
    model = repository / 'tests/data/test_stl/ASCII/20mmbox-LF.stl'
    presets = output / 'presets'
    presets.mkdir(exist_ok=True)
    (presets / 'PrusaSlicer.ini').write_text('[presets]\nprinter = CLI SLA\nsla_print = CLI process\nsla_material = CLI resin\n')
    for directory in ('printer', 'sla_print', 'sla_material'):
        (presets / directory).mkdir(exist_ok=True)
    (presets / 'printer/CLI SLA.ini').write_text('printer_technology = SLA\ndisplay_width = 12\ndisplay_height = 8\n')
    (presets / 'printer/CLI FFF.ini').write_text('printer_technology = FFF\nnozzle_diameter = 0.4\n')
    (presets / 'sla_print/CLI process.ini').write_text('layer_height = 0.1\n')
    (presets / 'sla_material/CLI resin.ini').write_text('exposure_time = 2.5\n')
    checks = []
    environment = dict(os.environ, DISPLAY='')

    def run(name, arguments, success=True):
        completed = subprocess.run([str(executable), '--datadir', str(presets), *map(str, arguments)],
            cwd=output, env=environment, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=120)
        (output / (name + '.log')).write_text(completed.stdout)
        assert (completed.returncode == 0) == success, (name, completed.returncode, completed.stdout[-3000:])
        checks.append(name)
        return completed.stdout

    help_text = run('help', ['--help-sla'])
    for obsolete in ('--help-fff', '--export-gcode', '--gcodeviewer', '--fill-pattern', '--thumbnails-format', 'filament', 'G-code', 'FFF'):
        assert obsolete not in help_text, obsolete
    assert '--query-print-material-profiles' in help_text
    run('defaults', ['--save', output / 'defaults.ini'])
    defaults = (output / 'defaults.ini').read_text()
    assert 'printer_technology = SLA' in defaults
    assert 'output_filename_format = [input_filename_base][default_output_extension]' in defaults
    for name, arguments in (
        ('reject-fff', ['--printer-technology', 'FFF', '--save', output / 'fff.ini']),
        ('reject-gcode', ['--export-gcode']),
        ('reject-viewer', ['--gcodeviewer']),
        ('reject-fdm-setting', ['--fill-pattern', 'stars', '--slice']),
        ('reject-url', ['--info', 'prusaslicer://download?file=https://example.invalid/model.stl']),
        ('reject-empty-slice', ['--slice']),
        ('reject-empty-model-export', ['--export-stl']),
        ('reject-query-fff', ['--query-printer-models', '--printer-technology', 'FFF']),
        ('reject-query-missing-profile', ['--query-print-material-profiles']),
    ):
        run(name, arguments, success=False)
    assert not (output / 'fff.ini').exists()
    query = run('query-sla-printers', ['--query-printer-models'])
    models_json = json.loads(query)
    assert 'printer_models' in models_json
    assert 'CLI SLA' in query and 'CLI FFF' not in query
    materials = json.loads(run('query-resin-profiles', ['--query-print-material-profiles', '--printer-profile', 'CLI SLA']))
    assert materials['printer_profile'] == 'CLI SLA'
    assert 'CLI resin' in json.dumps(materials)
    run('reject-fff-material-query', ['--query-print-material-profiles', '--printer-profile', 'CLI FFF'], success=False)
    run('reject-query-output', ['--query-printer-models', '--output', output / 'missing-parent/models.json'], success=False)

    # Synthetic raster for software acceptance; the required CXDLP identifier
    # does not certify these dimensions for a physical CL-60 printer.
    configuration = '''printer_technology = SLA
printer_model = CL-60
layer_height = 0.1
initial_layer_height = 0.1
display_width = 12
display_height = 8
display_pixels_x = 64
display_pixels_y = 32
display_orientation = landscape
display_mirror_x = 0
display_mirror_y = 0
bed_shape = 0x0,300x0,300x300,0x300
supports_enable = 0
pad_enable = 0
hollowing_enable = 0
'''
    settings = output / 'printer.ini'
    settings.write_text(configuration)
    run('bed', ['--load', settings, '--save', output / 'canonical.ini'])
    assert 'bed_shape = 0x0,12x0,12x8,0x8' in (output / 'canonical.ini').read_text()
    rgb_settings=output/'rgb-printer.ini'
    rgb_settings.write_text(configuration.replace('display_pixels_x = 64','display_pixels_x = 66'))
    for format_id, suffix in (('uvj', '.uvj'), ('genericzip', '.zip'), ('cxdlpv4', '.cxdlpv4'), ('cwsxml', '.xml.cws'), ('cwsnovamaker', '.cws'), ('cwsrgb', '.rgb.cws')):
        directory = output / format_id
        directory.mkdir(exist_ok=True)
        run('slice-' + format_id, ['--load', rgb_settings if format_id=='cwsrgb' else settings, '--sla-archive-format', format_id,
            '--scale', '0.1', '--slice', '--output', directory, model])
        files = list(directory.glob('*' + suffix))
        assert len(files) == 1 and files[0].stat().st_size > 0, files
        if format_id == 'uvj':
            with zipfile.ZipFile(files[0]) as archive:
                manifest = json.loads(archive.read('config.json'))
                assert manifest['Properties']['Size']['X'] == 64
                assert manifest['Properties']['Size']['Y'] == 32
                assert len(manifest['Layers']) == 20
                assert manifest['Properties']['Size']['Millimeter'] == {'X': 12.0, 'Y': 8.0}
                assert abs(manifest['Layers'][-1]['Z'] - 2.0) < 1e-6
                for index in range(20):
                    png = archive.read('slice/' + str(index).zfill(8) + '.png')
                    assert png[:8] == b'\x89PNG\r\n\x1a\n'
                    assert int.from_bytes(png[16:20], 'big') == 64
                    assert int.from_bytes(png[20:24], 'big') == 32
        if format_id == 'cwsrgb':
            with zipfile.ZipFile(files[0]) as archive:
                fields=dict(line.split('=',1) for line in archive.read('slice.conf').decode().splitlines() if '=' in line)
                fields={key.strip():float(value.strip()) for key,value in fields.items()}
                assert fields['xres']==66 and fields['yres']==32 and fields['layers_num']==20
                for index in range(20):
                    png=archive.read('sla_job'+str(index).zfill(5)+'.png')
                    assert png[24:26]==bytes((8,2))
                    assert int.from_bytes(png[16:20],'big')==22 and int.from_bytes(png[20:24],'big')==32
        if format_id == 'cwsnovamaker':
            with zipfile.ZipFile(files[0]) as archive:
                assert 'manifest.xml' not in archive.namelist()
                fields = dict(line.split('=',1) for line in archive.read('slice.conf').decode().splitlines() if '=' in line)
                fields = {key.strip():float(value.strip()) for key,value in fields.items()}
                assert fields['xres']==64 and fields['yres']==32
                assert fields['layers_num']==20 and fields['thickness']==0.1
                for index in range(20):
                    png=archive.read('sla_job'+str(index).zfill(5)+'.png')
                    assert png[24:26]==bytes((8,0))
        if format_id == 'cwsxml':
            with zipfile.ZipFile(files[0]) as archive:
                from xml.etree import ElementTree
                manifest = ElementTree.fromstring(archive.read('manifest.xml'))
                slices = manifest.findall('Slices/Slice/name')
                assert len(slices) == 20
                for layer in slices:
                    png = archive.read(layer.text)
                    assert png[:8] == b'\x89PNG\r\n\x1a\n'
                    assert int.from_bytes(png[16:20], 'big') == 64
                    assert int.from_bytes(png[20:24], 'big') == 32
                    assert png[24:26] == bytes((8, 6)), 'XML.CWS requires PNG32'
    run('reject-rgb-width', ['--load', settings, '--sla-archive-format', 'cwsrgb', '--scale', '0.1',
        '--slice', '--output', output/'invalid-width.rgb.cws', model], success=False)
    assert not (output/'invalid-width.rgb.cws').exists()
    run('reject-blocked-format', ['--load', settings, '--sla-archive-format', 'qdt', '--scale', '0.1',
        '--slice', '--output', output / 'blocked.qdt', model], success=False)
    assert not (output / 'blocked.qdt').exists()
    unicode_model = repository / 'tests/data/test_stl/Geräte/20mmbox-čřšřěá.stl'
    unicode_directory = output / 'zażółć'
    unicode_directory.mkdir(exist_ok=True)
    run('slice-unicode', ['--load', settings, '--sla-archive-format', 'genericzip', '--scale', '0.1',
        '--slice', '--output', unicode_directory, unicode_model])
    assert (unicode_directory / (unicode_model.stem+'.zip')).is_file()
    run('reject-output', ['--load', settings, '--sla-archive-format', 'uvj', '--scale', '0.1',
        '--slice', '--output', output / 'missing-parent/job.uvj', model], success=False)
    report = {'status': 'passed', 'checks': checks}
    (output / 'result.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
