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
    (presets / 'SLA-Slicer.ini').write_text('[presets]\nprinter = CLI SLA\nsla_print = CLI process\nsla_material = CLI resin\n')
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
    (output / 'old.gcode').write_text('G1 X1 Y1\n')
    for suffix in ('gcode', 'bgcode'):
        (output / ('old.' + suffix)).write_text('G1 X1 Y1\n')
        message = run('reject-%s-input' % suffix, [output / ('old.' + suffix), '--info'], success=False)
        assert 'not supported' in message and 'Traceback' not in message, message
    message = run('reject-fff-profile-message', ['--load', presets / 'printer/CLI FFF.ini', '--info'], success=False)
    assert 'not supported' in message, message
    message = run('reject-export-gcode-message', ['--export-gcode'], success=False)
    assert message.strip(), message
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
    # Regression: the final name differs from the requested one because the stem holds a
    # print-statistics placeholder. The export must succeed, land at the finalized path and
    # leave neither the unexpanded name nor a '.sla-export-*' temporary behind.
    template_directory = output / 'template'
    template_directory.mkdir(exist_ok=True)
    template_output = template_directory / 'job-{int(0*objects_used_material)}.zip'
    stdout = run('slice-finalized-name', ['--load', settings, '--sla-archive-format', 'genericzip', '--scale', '0.1',
        '--slice', '--output', template_output, model])
    assert 'Renaming file' not in stdout, stdout
    leftovers = sorted(path.name for path in template_directory.iterdir())
    assert leftovers == ['job-0.zip'], leftovers
    assert (template_directory / 'job-0.zip').stat().st_size > 0
    assert not list(template_directory.glob('.sla-export-*')) and not list(template_directory.glob('*sla-export*'))
    run('reject-output', ['--load', settings, '--sla-archive-format', 'uvj', '--scale', '0.1',
        '--slice', '--output', output / 'missing-parent/job.uvj', model], success=False)
    # --- approvals (--sla-accept-adjustments / --sla-accept-conversion): strict by default, same changes as the GUI dialogs
    uvtools = os.environ.get('UVTOOLS_CMD', '')
    uvtools = uvtools if uvtools and Path(uvtools).is_file() else ''
    def uv_properties(path):
        completed = subprocess.run([uvtools, 'print-properties', str(path)], stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=300)
        assert completed.returncode == 0, completed.stdout[-2000:]
        return completed.stdout
    stock = output / 'stock-printer.ini'
    stock.write_text(configuration.replace('printer_model = CL-60', 'printer_model = PHOTON MONO')
        .replace('display_width = 12', 'display_width = 82.62').replace('display_height = 8', 'display_height = 130.56')
        .replace('display_pixels_x = 64', 'display_pixels_x = 1620').replace('display_pixels_y = 32', 'display_pixels_y = 2560')
        + 'exposure_time = 2.5\ninitial_exposure_time = 15\n')
    approvals = output / 'approvals'
    approvals.mkdir(exist_ok=True)
    def sl1_job(name, target, extra=(), success=True):
        return run(name, ['--load', stock, '--sla-archive-format', 'SL1', '--scale', '0.2', '--center', '40,60',
                          *extra, '--slice', '--output', target, model], success=success)
    adjustments = ('Wait before exposure 0 s -> 1 s', 'Lift speed 80 mm/min -> 60 mm/min',
                   'Retract speed 150 mm/min -> 120 mm/min', 'Printer model PHOTON MONO -> CL-60')
    for flags in ((), ('--sla-accept-conversion',)):
        message = sl1_job('adjust-refused' + ''.join(flags), approvals / 'refused.cxdlp', flags, success=False)
        for text in adjustments:
            assert text in message, (text, message)
        assert '--sla-accept-adjustments' in message
        assert not (approvals / 'refused.cxdlp').exists() and not list(approvals.glob('.sla-export-*'))
    message = sl1_job('adjust-accepted', approvals / 'adjusted.cxdlp', ('--sla-accept-adjustments',))
    for text in adjustments:
        assert 'Adjusted for export: ' + text in message, (text, message)
    adjusted = (approvals / 'adjusted.cxdlp').read_bytes()
    assert b'CXSW3DV2' in adjusted and b'CL-60' in adjusted and b'PHOTON MONO' not in adjusted
    if uvtools:
        properties = uv_properties(approvals / 'adjusted.cxdlp')
        for text in ('PrinterModel: CL-60', 'LayerCount: 40', 'WaitTimeBeforeCure: 1', 'ExposureTime: 2.5', 'LiftSpeed: 60', 'RetractSpeed: 120'):
            assert text in properties, (text, properties)
        checks.append('adjust-accepted-uvtools-readback')
    # The refusal did not touch the profile: strict again without the flag.
    sl1_job('adjust-refused-again', approvals / 'again.cxdlp', success=False)
    # A format that needs a palette conversion: CTB v4 stores 128 gray levels.
    message = sl1_job('palette-refused', approvals / 'refused.v4.ctb', ('--sla-accept-adjustments',), success=False)
    assert 'palette conversion' in message and '--sla-accept-conversion' in message and not (approvals / 'refused.v4.ctb').exists(), message
    message = sl1_job('palette-accepted', approvals / 'converted.v4.ctb', ('--sla-accept-conversion',))
    assert 'Converted for export: palette conversion: ctb_v4 uses 128 grayscale levels' in message, message
    if uvtools:
        properties = uv_properties(approvals / 'converted.v4.ctb')
        for text in ('LayerCount: 40', 'AntiAliasing: 8', 'ResolutionX: 1620', 'ExposureTime: 2.5'):
            assert text in properties, (text, properties)
        checks.append('palette-accepted-uvtools-readback')
    # GOO needs neither: the flags change nothing and the file is the strict one.
    message = sl1_job('goo-with-flags', approvals / 'job.goo', ('--sla-accept-adjustments', '--sla-accept-conversion'))
    assert 'Adjusted for export' not in message and 'Converted for export' not in message, message
    if uvtools:
        properties = uv_properties(approvals / 'job.goo')
        for text in ('LayerCount: 40', 'ExposureTime: 2.5', 'MachineName: PHOTON MONO'):
            assert text in properties, (text, properties)
        checks.append('goo-uvtools-readback')
    # Printer format and output extension differ: values are adjusted only for the writer actually used.
    # The printer's own format is Klipper (exposure stored with 0.01 s precision); 2.504 s needs an adjustment there.
    klipper_ini = output / 'klipper-printer.ini'
    klipper_ini.write_text(stock.read_text().replace('exposure_time = 2.5\n', 'exposure_time = 2.504\n'))
    def klipper_job(name, target, extra=(), success=True):
        return run(name, ['--load', klipper_ini, '--sla-archive-format', 'klipper', '--scale', '0.2', '--center', '40,60',
                          *extra, '--slice', '--output', target, model], success=success)
    both = ('--sla-accept-adjustments', '--sla-accept-conversion')
    # Real writer GOO represents 2.504 exactly: no adjustment line, strict or approved, and no Klipper refusal.
    for name, flags in (('klipper-printer-goo-strict', ()), ('klipper-printer-goo-approved', both)):
        message = klipper_job(name, approvals / (name + '.goo'), flags)
        assert 'Adjusted for export' not in message, message
        assert (approvals / (name + '.goo')).is_file()
        if uvtools:
            assert 'ExposureTime: 2.504' in uv_properties(approvals / (name + '.goo'))
    # Real writer Klipper cannot: refused with the list, then adjusted after approval.
    message = klipper_job('klipper-printer-klipper-strict', approvals / 'strict.zip', success=False)
    assert 'Exposure 2.504 s -> 2.5 s' in message and not (approvals / 'strict.zip').exists(), message
    message = klipper_job('klipper-printer-klipper-approved', approvals / 'approved.zip', both)
    assert 'Adjusted for export: Exposure 2.504 s -> 2.5 s' in message, message
    assert (approvals / 'approved.zip').is_file()
    help_text = run('help-approvals', ['--help'])
    assert '--sla-accept-adjustments' in help_text and '--sla-accept-conversion' in help_text
    def run_recipe(name, arguments, success=True):
        completed = subprocess.run([str(executable), *map(str, arguments)], cwd=output, env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=120)
        (output / (name + '.log')).write_text(completed.stdout)
        assert (completed.returncode == 0) == success, (name, completed.returncode, completed.stdout[-3000:])
        checks.append(name)
        return completed.stdout
    # Recipes: a convert step takes "accept_adjustments" (default false) and --sla-accept-adjustments. Imported
    # documents convert only to destinations with a parameter-preserving conversion, and none of the formats with
    # representation rules is reachable from an imported table yet (CXDLP v2/v3 has none), so these runs prove the
    # field, the flag and that a destination without adjustments is unaffected.
    source = approvals / 'converted.v4.ctb'
    printer_ini = approvals / 'printer-for-recipe.ini'
    printer_ini.write_text(stock.read_text())
    def recipe_with(path, extra):
        recipe = approvals / 'recipe.json'
        recipe.write_text(json.dumps({'version': 1, 'steps': [{'op': 'open', 'path': str(source)},
            dict({'op': 'convert', 'format': 'ctb_v5', 'path': str(path), 'printer_config': str(printer_ini)}, **extra)]}))
        return recipe
    run_recipe('recipe-default', ['--sla-run-recipe', recipe_with(approvals / 'recipe-default.v5.ctb', {})])
    run_recipe('recipe-step-field', ['--sla-run-recipe', recipe_with(approvals / 'recipe-step.v5.ctb', {'accept_adjustments': True})])
    run_recipe('recipe-cli-flag', ['--sla-run-recipe', recipe_with(approvals / 'recipe-flag.v5.ctb', {}), '--sla-accept-adjustments'])
    for name in ('recipe-default.v5.ctb', 'recipe-step.v5.ctb', 'recipe-flag.v5.ctb'):
        assert (approvals / name).is_file(), name
    message = run_recipe('recipe-bad-field', ['--sla-run-recipe', recipe_with(approvals / 'x.v5.ctb', {'accept_adjustment': True})], success=False)
    assert 'accept_adjustments' in message, message
    report = {'status': 'passed', 'checks': checks}
    (output / 'result.json').write_text(json.dumps(report, indent=2))
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
