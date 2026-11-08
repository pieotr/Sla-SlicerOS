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
    parser.add_argument('--sidebar-export-failure', action='store_true', help='Fail final publication, preserve sliced result and retry without slicing')
    parser.add_argument('--sidebar-owned-export', action='store_true', help='Exercise sidebar No/Yes export after painted layers and 3MF restart')
    parser.add_argument('--deferred-direct-export', action='store_true', help='Accept the direct export before initial slicing through File > Export')
    parser.add_argument('--range-brush', action='store_true', help='Exercise real mouse drawing, range overlay and whole-stroke Undo/Redo')
    parser.add_argument('--printer-geometry', action='store_true', help='Use the active copied printer physical screen, pixels and firmware model')
    parser.add_argument('--hold-format-popup', action='store_true', help='Assert native format popup stays expanded while the export timer runs')
    parser.add_argument('--direct-export', choices=('klipper','klipper_rgb','sl1','sl1s','cxdlpv4','ctb','ctb_v1','ctb_v3','ctb_v4','ctb_v5','ctb_gktwo','ctb_encrypted','ctb_encrypted_v4','cbddlp','cbddlp_v1','photon','photon_v1','photons','phz','fdg','svgx','nanodlp','nanodlp_v1','nanodlp_rgb','nanodlp_rgb_v1','lgs','lgs30','lgs120','lgs4k','gr1','mdlp','qdt','vdt','jxs','zcodex','zcode'), help='Verify sidebar export rather than editor export')
    parser.add_argument('--cbddlp-aa', type=int, default=8, help='CBDDLP/Photon v2 exposure plane count, 1 to 16')
    parser.add_argument('--nanodlp-profile', type=Path, help='Explicit test controller profile.json for NanoDLP exports')
    parser.add_argument('--phz-encryption-key', default='0', help='PHZ v2 seed (decimal or 0x hexadecimal)')
    parser.add_argument('--exercise-format-switch', action='store_true', help='Switch native export filters and verify filename suffix before typing an output path')
    parser.add_argument('--shared-zip-switch', action='store_true', help='Export Generic ZIP then Klipper from the same sliced Mono job despite their shared suffix')
    parser.add_argument('--image-mask-export', action='store_true', help='Verify current-mask and range image exports from sliced layers')
    parser.add_argument('--klipper-export', choices=('klipper','klipper_rgb'), help='Verify native Klipper MSLA ZIP masks, controls and suffix')
    parser.add_argument('--sl1s-export', action='store_true', help='Verify SL1S choice and automatic filename suffix in the layer dialog')
    parser.add_argument('--sl1-export', action='store_true', help='Verify reference SL1 export from the actual layer dialog')
    parser.add_argument('--seed-printer-format', action='store_true', help='Keep the copied printer format instead of overriding it from CLI')
    parser.add_argument('--project-roundtrip', action='store_true', help='Save edited layers in 3MF and verify restoration after restart')
    parser.add_argument('--owned-parameters', action='store_true', help='Edit actual model exposure through Layer parameters, Apply and project restart')
    parser.add_argument('--vda-export', action='store_true', help='Verify Voxeldance Additive ZIP choice and independent XML schema')
    parser.add_argument('--chitubox-export', action='store_true', help='Verify Chitubox ZIP choice and independent exposure schema')
    parser.add_argument('--missing-printer-identifier',action='store_true',help='Check early CXDLPv4 identity refusal before chooser or palette conversion')
    parser.add_argument('--cxdlpv4-export', action='store_true', help='Verify CXDLPv4 output and checksum')
    parser.add_argument('--cxdlpv2-export', action='store_true', help='Verify native CXDLP v2 selection, compound suffix and XOR')
    parser.add_argument('--cxdlpv3-export', action='store_true', help='Verify native CXDLP v3 settings and checksum')
    parser.add_argument('--cxdlpv1-export', action='store_true', help='Verify the version-specific compound CXDLP extension')
    parser.add_argument('--genericzip-export', action='store_true', help='Verify independent Generic ZIP choice, filename suffix and schema')
    parser.add_argument('--osf-export', action='store_true', help='Verify OSF choice, automatic filename suffix and native export')
    parser.add_argument('--anycubic-export', choices=('pws', 'dl2p', 'pwc', 'pm5', 'pm3n', 'px6s', 'pm5s', 'm5sp'), help='Verify Anycubic binary export and automatic suffix')
    parser.add_argument('--anycubic-version', choices=('auto','1','515','516','517','518'), default='auto', help='Printer-persistent Anycubic file version')
    parser.add_argument('--anycubic-tsmc', action='store_true', help='Use distinct bottom/normal two-stage lift and retract speeds for Anycubic 518')
    parser.add_argument('--debug', action='store_true', help='Run the test application under a local debugger')
    parser.add_argument('--cwsxml-export', action='store_true', help='Verify XML.CWS compound suffix, XML manifest and opaque RGBA masks')
    parser.add_argument('--novamaker-export', action='store_true', help='Verify NovaMaker slice.conf, grayscale masks and native export')
    parser.add_argument('--cwsrgb-export', action='store_true', help='Verify RGB.CWS packed BGR masks, full LCD geometry and compound suffix')
    parser.add_argument('--installed-preset-settings',type=Path,help='Private resolved role settings fixture for an installed preset with different metadata')
    parser.add_argument('--preset-metadata-roundtrip', action='store_true', help='Verify role-specific library metadata and missing-parent independence through 3MF restart')
    parser.add_argument('--printer-preference-roundtrip', action='store_true', help='Verify source-printer format preference after a 3MF restart')
    parser.add_argument('--settings-reopen', action='store_true', help='Reopen the existing model editor from Printer Settings through Window menu')
    args = parser.parse_args()
    if (args.printer_preference_roundtrip or args.preset_metadata_roundtrip) and not args.project_roundtrip:
        parser.error('preference/metadata roundtrip requires --project-roundtrip')
    if args.sidebar_export_failure and not args.sidebar_owned_export:
        parser.error('--sidebar-export-failure requires --sidebar-owned-export')
    if args.sidebar_owned_export and not (args.owned_parameters and args.cxdlpv4_export and args.project_roundtrip):
        parser.error('--sidebar-owned-export requires --owned-parameters --cxdlpv4-export --project-roundtrip')
    if args.deferred_direct_export and not args.direct_export:
        parser.error('--deferred-direct-export requires --direct-export')
    args.sl1_export = args.sl1_export or args.sl1s_export
    if args.installed_preset_settings and not args.preset_metadata_roundtrip:
        parser.error('--installed-preset-settings requires --preset-metadata-roundtrip')
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
    configuration.write_text(configuration.read_text()+'sla_anycubic_version = '+args.anycubic_version+'\n')
    if args.project_roundtrip:
        configuration.write_text(configuration.read_text()+'exposure_time = 2\n')
    if args.missing_printer_identifier:
        configuration.write_text(configuration.read_text()+'printer_model = \n')
    if args.owned_parameters and args.cxdlpv4_export and not args.missing_printer_identifier:
        # CXDLPv4 requires an explicit printer identifier. This fixture is
        # generic software validation, never a claim about real hardware.
        configuration.write_text(configuration.read_text()+'printer_model = GENERIC\n')
    if args.image_mask_export or args.sl1_export:
        configuration.write_text(configuration.read_text()+'display_orientation = landscape\n')
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
        # The loaded workflow becomes a temporary printer named workflow.ini.
        # Reused seeds can contain a previously exported format for that name;
        # clear only this synthetic preset's preference in our private copy.
        # The test then checks the requested printer configuration as default.
        app_ini = datadir / 'PrusaSlicer.ini'
        app_ini.write_text(re.sub(r'(?m)^printer_' + 'workflow.ini'.encode().hex() + r' = .*\n',
                                  '', app_ini.read_text()))
        configuration.write_text(re.sub(r'(?m)^sla_archive_format = .*$',
            'sla_archive_format = '+(args.direct_export.upper() if args.direct_export in ('sl1','sl1s') else args.direct_export),configuration.read_text()))
    if args.direct_export in ('nanodlp','nanodlp_v1','nanodlp_rgb','nanodlp_rgb_v1'):
        if not args.nanodlp_profile:
            raise ValueError('NanoDLP GUI test requires an explicit controller profile')
        profile=json.loads(args.nanodlp_profile.read_text())
        # This fixture uses original FSS scripts only for decoder/GUI tests.
        # It is never installed into user presets or certified for hardware.
        text=configuration.read_text()
        values={'display_mirror_x':1,'display_mirror_y':0,'sla_bottom_layers':1,'sla_transition_layers':0,
                'sla_light_pwm':180,'sla_bottom_light_pwm':180,
                'sla_wait_before_cure':1.25,'sla_bottom_wait_before_cure':2.5,
                'sla_wait_after_cure':.5,'sla_bottom_wait_after_cure':.5,
                'sla_wait_after_lift':.75,'sla_bottom_wait_after_lift':.75}
        for key,value in values.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        serialized=json.dumps(profile,ensure_ascii=False,separators=(',',':')).replace(chr(92),chr(92)*2)
        text+='nanodlp_device_profile = '+serialized+'\n'
        configuration.write_text(text)
    if args.direct_export == 'svgx':
        text=configuration.read_text()
        values={'display_mirror_x':0,'display_mirror_y':1,'sla_transition_layers':0}
        for prefix in ('sla_', 'sla_bottom_'):
            values.update({prefix+'lift_height':6,prefix+'lift_speed':100,prefix+'retract_speed':100,prefix+'light_pwm':255})
            values.update({prefix+key:0 for key in ('wait_before_cure','wait_after_cure','wait_after_lift','lift_height2','retract_height2')})
        for key,value in values.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
    if args.direct_export in ('ctb_encrypted','ctb_encrypted_v4','ctb_v3','ctb_v4','ctb_v5','ctb_gktwo'):
        text=configuration.read_text()
        for key,value in {'display_mirror_x':1,'display_mirror_y':0}.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
    if args.direct_export in ('cbddlp','photon','cbddlp_v1','photon_v1'):
        configuration.write_text(configuration.read_text()+'sla_cbddlp_aa = '+str(args.cbddlp_aa)+'\n')
    if args.direct_export in ('ctb_v1','cbddlp_v1','photon_v1'):
        text=configuration.read_text()
        values={'sla_lift_height':.05,'sla_bottom_lift_height':6,
                'sla_lift_speed':100,'sla_bottom_lift_speed':100,
                'sla_retract_speed':100,'sla_bottom_retract_speed':100}
        for key,value in values.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
    if args.direct_export == 'photons':
        configuration.write_text(configuration.read_text()
            .replace('display_width = 100\n', 'display_width = 68.04\n')
            .replace('display_height = 60\n', 'display_height = 120.96\n')
            .replace('display_pixels_x = 200\n', 'display_pixels_x = 1440\n')
            .replace('display_pixels_y = 120\n', 'display_pixels_y = 2560\n')
            + 'max_print_height = 165\ndisplay_orientation = landscape\ndisplay_mirror_x = 1\ndisplay_mirror_y = 0\nsla_bottom_layers = 1\nsla_transition_layers = 0\nsla_lift_speed = 120\nsla_bottom_lift_speed = 120\nsla_retract_speed = 120\nsla_bottom_retract_speed = 120\nsla_lift_height = 6\nsla_bottom_lift_height = 6\nsla_wait_before_cure = 1\nsla_bottom_wait_before_cure = 1\nsla_lift_height2 = 0\nsla_bottom_lift_height2 = 0\nsla_retract_height2 = 0\nsla_bottom_retract_height2 = 0\nsla_wait_after_cure = 0\nsla_bottom_wait_after_cure = 0\nsla_wait_after_lift = 0\nsla_bottom_wait_after_lift = 0\nsla_light_pwm = 255\nsla_bottom_light_pwm = 255\nexposure_time = 3\ninitial_exposure_time = 20\n')
    if args.direct_export in ('lgs','lgs30','lgs120','lgs4k'):
        text=configuration.read_text()
        values={'display_pixels_x':128,'display_pixels_y':256,'display_width':64,'display_height':128,
                'max_print_height':{'lgs':140,'lgs30':170,'lgs120':150,'lgs4k':190}[args.direct_export],
                'display_orientation':'landscape','display_mirror_x':1,'display_mirror_y':0,
                'sla_bottom_layers':1,'sla_transition_layers':0,'sla_lift_speed':80,'sla_bottom_lift_speed':60,
                'sla_retract_speed':80,'sla_bottom_retract_speed':80,
                'sla_wait_before_cure':1.25,'sla_bottom_wait_before_cure':2.5,
                'sla_lift_height':6,'sla_bottom_lift_height':4}
        for key,value in values.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
    if args.direct_export in ('gr1','mdlp'):
        text=configuration.read_text()
        values={'display_pixels_x':128,'display_pixels_y':256,'display_width':64,'display_height':128,
                'display_orientation':'landscape','display_mirror_x':0,'display_mirror_y':0,
                'sla_bottom_layers':1,'sla_transition_layers':0,'exposure_time':3,'initial_exposure_time':20,
                'sla_lift_speed':100 if args.direct_export=='mdlp' else 120,
                'sla_bottom_lift_speed':100 if args.direct_export=='mdlp' else 60,
                'sla_retract_speed':100 if args.direct_export=='mdlp' else 120,
                'sla_bottom_retract_speed':100 if args.direct_export=='mdlp' else 120,
                'sla_wait_before_cure':.8 if args.direct_export=='mdlp' else 2,
                'sla_bottom_wait_before_cure':.8 if args.direct_export=='mdlp' else 2,
                'sla_lift_height':6,'sla_bottom_lift_height':6 if args.direct_export=='mdlp' else 4}
        for key,value in values.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
    if args.direct_export == 'zcode':
        text=configuration.read_text()
        values={'display_pixels_x':128,'display_pixels_y':256,'display_width':64,'display_height':128,
                'max_print_height':220,'display_orientation':'landscape','display_mirror_x':0,'display_mirror_y':1,
                'sla_bottom_layers':1,'sla_transition_layers':1,'exposure_time':3,'initial_exposure_time':20}
        for key,value in values.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
    if args.direct_export == 'zcodex':
        text=configuration.read_text()
        values={'display_pixels_x':1440,'display_pixels_y':2560,'display_width':74.67,'display_height':132.88,
                'max_print_height':175,'display_orientation':'landscape','display_mirror_x':1,'display_mirror_y':0,
                'sla_bottom_layers':1,'sla_transition_layers':0,'exposure_time':3,'initial_exposure_time':20,
                'sla_lift_height':6,'sla_bottom_lift_height':6,'sla_lift_speed':120,'sla_bottom_lift_speed':120,
                'sla_retract_speed':100,'sla_bottom_retract_speed':100,
                'sla_wait_before_cure':1.25,'sla_bottom_wait_before_cure':1.25}
        for key,value in values.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
    if args.direct_export == 'jxs':
        text=configuration.read_text()
        values={'display_pixels_x':4920,'display_pixels_y':2880,'display_width':221.4,'display_height':129.6,
                'max_print_height':245,'display_orientation':'landscape','display_mirror_x':0,'display_mirror_y':1,
                'sla_bottom_layers':1,'sla_transition_layers':1,'exposure_time':3,'initial_exposure_time':20}
        for key,value in values.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
    if args.direct_export == 'vdt':
        text=re.sub(r'(?m)^display_orientation = .*\n','',configuration.read_text())
        configuration.write_text(text+'display_orientation = landscape\n')
    if args.direct_export == 'qdt':
        text=configuration.read_text()
        values={'display_pixels_x':800,'display_pixels_y':400,'display_width':400,'display_height':200,
                'max_print_height':400,'display_orientation':'landscape','display_mirror_x':1,'display_mirror_y':0,
                'sla_bottom_layers':4,'sla_transition_layers':0,'exposure_time':3,'initial_exposure_time':30,
                'sla_lift_speed':100,'sla_bottom_lift_speed':100,
                'sla_retract_speed':100,'sla_bottom_retract_speed':100,
                'sla_wait_before_cure':0,'sla_bottom_wait_before_cure':0,
                'sla_lift_height':6,'sla_bottom_lift_height':6}
        for key,value in values.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
    if args.direct_export == 'phz':
        configuration.write_text(configuration.read_text()+'sla_phz_encryption_key = '+args.phz_encryption_key+'\n')
    if args.cwsrgb_export:
        configuration.write_text(configuration.read_text().replace('display_pixels_x = 200\n','display_pixels_x = 201\n')+'display_orientation = landscape\n')
    if args.anycubic_tsmc:
        assert args.anycubic_export in ('pwc', 'dl2p', 'pm5', 'pm3n', 'px6s', 'pm5s', 'm5sp'), 'TSMC fixture requires a modern Anycubic format'
        configuration.write_text(configuration.read_text() +
            'sla_lift_height = 3\nsla_bottom_lift_height = 5\n'
            'sla_lift_height2 = 2.5\nsla_bottom_lift_height2 = 3\n'
            'sla_lift_speed = 90\nsla_bottom_lift_speed = 60\n'
            'sla_lift_speed2 = 180\nsla_bottom_lift_speed2 = 120\n'
            'sla_retract_speed = 150\nsla_bottom_retract_speed = 210\n'
            'sla_retract_speed2 = 240\nsla_bottom_retract_speed2 = 300\n'
            'sla_wait_before_cure = 1.25\nsla_bottom_wait_before_cure = 1.25\n'
            'sla_bottom_layers = 1\nsla_transition_layers = 1\n')
    if args.anycubic_export == 'pws':
        configuration.write_text(configuration.read_text()
            .replace('display_width = 100\n', 'display_width = 68.04\n')
            .replace('display_height = 60\n', 'display_height = 120.96\n')
            + 'max_print_height = 165\ndisplay_orientation = landscape\n')
    if args.anycubic_export == 'dl2p' and args.anycubic_version == '515':
        text=configuration.read_text()
        for key,value in {'display_width':130.56,'display_height':73.44,'max_print_height':165,'display_orientation':'landscape'}.items():
            text=re.sub(r'(?m)^'+key+r' = .*\n','',text)+key+' = '+str(value)+'\n'
        configuration.write_text(text)
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
            # Binary formats at this fixture's 0.5 mm pixel pitch need an
            # object wider than one pixel. Keep the 0.3 mm Z / six layers.
            line = 'v ' + ' '.join(str(float(value) * (.05 if args.direct_export in ('gr1','mdlp','qdt','vdt','zcode') and index<2 else .015))
                                  for index,value in enumerate(line.split()[1:]))
        lines.append(line)
    model.write_text('\n'.join(lines) + '\n')
    metadata_fixture=None
    if args.preset_metadata_roundtrip:
        metadata_fixture={
            'version':1,
            'printer':{'inherits':'Missing printer base','printer_vendor':'Offline vendor',
                'printer_variant':'LCD revision B','default_sla_print_profile':'Printer default process',
                'default_sla_material_profile':'Printer default resin'},
            'material':{'inherits':'Missing material base','compatible_printers':'"Drukarka Łódź";"Printer; B"',
                'compatible_prints':'"Proces dokładny";"Process; B"','compatible_printers_condition':'printer_model == "CL-60"',
                'compatible_prints_condition':'layer_height <= 0.05','default_sla_material_profile':'Material role default'},
            'process':{'inherits':'Missing process base','compatible_printers':'"Drukarka Łódź";"Printer; B"',
                'compatible_printers_condition':'display_pixels_x >= 200','default_sla_print_profile':'Process role default'}}
        encoded=json.dumps(metadata_fixture,ensure_ascii=False,separators=(',',':')).replace('\\','\\\\')
        configuration.write_text(configuration.read_text()+'printer_model = CL-60\nprinter_settings_id = Drukarka Łódź\n'
            +'sla_material_settings_id = Żywica testowa\nsla_print_settings_id = Proces dokładny\n'
            +'sla_preset_metadata = '+encoded+'\n')
    installed_files={}
    if args.installed_preset_settings:
        from hashlib import sha256
        fixture=json.loads(args.installed_preset_settings.read_text())
        for role,folder,identity,name in [('printer','printer','printer_settings_id','Drukarka Łódź'),
                ('material','sla_material','sla_material_settings_id','Żywica testowa'),
                ('process','sla_print','sla_print_settings_id','Proces dokładny')]:
            values=dict(fixture[role]['values'])
            values.update(metadata_fixture[role]);values['inherits']='Installed base'
            if role=='printer':
                values['printer_model']='CL-92';values['default_sla_print_profile']='Installed default process'
            else: values['compatible_printers']='"Other printer"'
            values[identity]=name
            profile=datadir/folder/(name+'.ini');profile.parent.mkdir(exist_ok=True)
            profile.write_text(''.join(key+' = '+value+'\n' for key,value in sorted(values.items())))
            installed_files[profile]=sha256(profile.read_bytes()).hexdigest()
    report = {'datadir': str(datadir), 'checks': []}
    log = (directory / 'application.log').open('w')
    command = [
        str(args.binary.resolve()), '--datadir', str(datadir),
        '--load', str(configuration), str(model)]
    if args.debug:
        command = ['gdb', '-batch', '-ex', 'set debuginfod enabled off',
                   '-ex', 'run', '-ex', 'bt 24', '--args'] + command
    application_env = dict(os.environ, GDK_BACKEND='x11', NO_AT_BRIDGE='0')
    # Audit the app separately from the AT-SPI test driver and its IPC.
    audit_library = os.environ.get('SLA_APP_NETWORK_AUDIT_LIBRARY')
    if audit_library:
        application_env['LD_PRELOAD'] = str(Path(audit_library).resolve())
    process = subprocess.Popen(command,
        stdout=log, stderr=subprocess.STDOUT,
        env=application_env)
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

        def verify_application_network():
            audit = os.environ.get('SLA_NETWORK_AUDIT_FILE')
            if audit:
                maps = Path(f'/proc/{application_pid}/maps').read_text()
                assert 'offline_network_audit.so' in maps, 'Network audit was not loaded'
                attempts = Path(audit).read_text() if Path(audit).exists() else ''
                assert not attempts.strip(), 'Unexpected network attempts: ' + attempts
                report['checks'].append('network audit loaded; no network attempts in this workflow')
                report.setdefault('audited_pids', [])
                if application_pid not in report['audited_pids']: report['audited_pids'].append(application_pid)

        def button(label):
            app.clear_cache()
            def eligible(node):
                if node.get_role_name() != 'button' or node.get_name() != label:
                    return False
                before = node.get_state_set().contains(Atspi.StateType.SENSITIVE)
                node.clear_cache()
                state = node.get_state_set()
                after = state.contains(Atspi.StateType.SENSITIVE)
                if before != after:
                    report.setdefault('refreshed_button_states', []).append(
                        dict(name=label, cached_sensitive=before, current_sensitive=after))
                return state.contains(Atspi.StateType.SHOWING) and after
            return find(app, eligible)
        def opened_editor():
            visible=lambda n:n.get_state_set().contains(Atspi.StateType.SHOWING)
            modal=find(app,lambda n:n.get_role_name() in ('dialog','frame') and
                n.get_name().startswith('Layer editor') and visible(n))
            assert modal is None, 'Edit layers opened a modal dialog instead of Sliced'
            return find(app,lambda n:n.get_role_name()=='scroll pane' and visible(n) and
                named(n,'Apply to sliced result') is not None and named(n,'Back to 3D') is not None)

        def editor_changed(editor):
            return find(editor,lambda n:n.get_state_set().contains(Atspi.StateType.SHOWING) and
                n.get_name().startswith('Sliced layers: modified.')) is not None

        def close_editor(editor):
            click(named(editor,'Back to 3D'))
            wait_for(lambda:not editor.get_state_set().contains(Atspi.StateType.SHOWING),'back to 3D')

        first_action = wait_for(lambda: button('No') or button('Slice now'), 'model import')
        if first_action.get_name() == 'No':
            # This synthetic OBJ is already in millimeters; reject auto inch conversion.
            click(first_action)
        if args.deferred_direct_export:
            assert not button('Edit layers'), 'Deferred fixture already has a ready result'
            click(named(app, 'File'))
            submenu=wait_for(lambda:find(app,lambda n:n.get_role_name() in ('menu','menu item') and
                n.get_name()=='Export' and n.get_state_set().contains(Atspi.StateType.SHOWING)),'File export submenu')
            click(submenu)
            item=wait_for(lambda:find(app,lambda n:n.get_role_name()=='menu item' and
                n.get_name().startswith('Export sliced layers') and n.get_state_set().contains(Atspi.StateType.SHOWING)),'deferred SLA export menu')
            click(item)
            report['checks'].append('export requested before initial slicing through the SLA menu')
        else:
            click(wait_for(lambda: button('Slice now'), 'enabled slicing button'))
            wait_for(lambda: button('Edit layers'), 'completed slicing', timeout=40)
        if args.direct_export:
            if not args.deferred_direct_export:
                export = wait_for(lambda: find(app, lambda n: n.get_role_name() == 'button' and
                    n.get_name().startswith('Export') and n.get_state_set().contains(Atspi.StateType.SHOWING) and
                    n.get_state_set().contains(Atspi.StateType.SENSITIVE)), 'sidebar export action')
                click(export)
            chooser = wait_for(lambda: find(app, lambda n: n.get_name().rstrip(':') == 'Save SLA file as' and
                n.get_state_set().contains(Atspi.StateType.SHOWING)), 'sidebar output dialog')
            initial_field=wait_for(lambda: find(chooser, lambda n: n.get_role_name() in ('text','entry') and
                n.get_state_set().contains(Atspi.StateType.EDITABLE) and n.get_state_set().contains(Atspi.StateType.SHOWING)), 'initial output filename')
            direct_extension={'klipper':'zip','klipper_rgb':'rgb.zip','cbddlp_v1':'v1.cbddlp','photon_v1':'v1.photon','ctb_v1':'v1.ctb','ctb_v3':'v3.ctb','ctb_v4':'v4.ctb','ctb_v5':'v5.ctb','ctb_gktwo':'gktwo.ctb','ctb_encrypted':'encrypted.ctb','ctb_encrypted_v4':'v4.encrypted.ctb','nanodlp_v1':'v1.nanodlp','nanodlp_rgb':'rgb.nanodlp','nanodlp_rgb_v1':'v1.rgb.nanodlp'}.get(args.direct_export,args.direct_export)
            assert Atspi.Text.get_text(initial_field,0,-1).endswith('.'+direct_extension), 'Default filter disagrees with printer format'
            combos=[]
            def collect(node):
                if node.get_role_name() == 'combo box' and node.get_state_set().contains(Atspi.StateType.SHOWING): combos.append(node)
                for index in range(node.get_child_count()): collect(node.get_child_at_index(index))
            collect(chooser)
            suffix='.'+direct_extension
            for combo in combos:
                option=find(combo, lambda n: n.get_role_name() in ('menu item','list item') and ('*'+suffix) in n.get_name() and (args.direct_export not in ('klipper','klipper_rgb') or ('Klipper MSLA' in n.get_name() and ('RGB' if args.direct_export=='klipper_rgb' else 'Mono') in n.get_name())))
                if option:
                    if args.exercise_format_switch:
                        alternate_suffix='.sl1' if args.direct_export!='sl1' else '.ctb'
                        alternate=find(combo, lambda n: n.get_role_name() in ('menu item','list item') and
                                       ('*'+alternate_suffix) in n.get_name())
                        assert alternate, 'Missing alternate native export filter'
                        assert combo.get_selection_iface().select_child(alternate.get_index_in_parent())
                        wait_for(lambda: Atspi.Text.get_text(initial_field,0,-1).endswith(alternate_suffix),
                                 'automatic alternate filename suffix')
                        assert combo.get_selection_iface().select_child(option.get_index_in_parent())
                        wait_for(lambda: Atspi.Text.get_text(initial_field,0,-1).endswith(suffix),
                                 'automatic restored filename suffix')
                        report['checks'].append('native format switch changes filename suffix in both directions')
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
            if args.direct_export in ('sl1','sl1s'):
                with zipfile.ZipFile(output) as archive:
                    assert 'config.ini' in archive.namelist()
                    assert any(n.endswith('.png') and not n.startswith('thumbnail') for n in archive.namelist())
            elif args.direct_export in ('klipper','klipper_rgb'):
                with zipfile.ZipFile(output) as archive:
                    assert '.klipper' in archive.namelist() and 'job.gcode' in archive.namelist()
                    assert 'MSLA_DISPLAY_VALIDATE RESOLUTION=' in archive.read('job.gcode').decode()
                    assert '1.png' in archive.namelist() and 'config.ini' not in archive.namelist()
            elif args.direct_export in ('ctb','ctb_v1','cbddlp','cbddlp_v1','photon','photon_v1'):
                import struct
                data=output.read_bytes()
                expected_magic=0x12fd0086 if args.direct_export in ('ctb','ctb_v1') else 0x12fd0019
                assert struct.unpack_from('<II',data)==(expected_magic,1 if args.direct_export in ('ctb_v1','cbddlp_v1','photon_v1') else 2)
                table,count=struct.unpack_from('<II',data,64)
                assert count>0 and table+36*count<=len(data)
                planes=struct.unpack_from('<I',data,92)[0]
                assert planes==(1 if args.direct_export in ('ctb','ctb_v1','cbddlp_v1','photon_v1') else args.cbddlp_aa)
                for index in range(count*planes):
                    address,size=struct.unpack_from('<II',data,table+36*index+12)
                    assert size>0 and address+size<=len(data)
            elif args.direct_export in ('ctb_v3','ctb_v4','ctb_v5','ctb_gktwo'):
                import struct
                data=output.read_bytes()
                version=3 if args.direct_export=='ctb_v3' else 5 if args.direct_export=='ctb_v5' else 4
                magic=0xFF220810 if args.direct_export=='ctb_gktwo' else 0x12FD0086 if version==3 else 0x12FD0106
                assert struct.unpack_from('<II',data)==(magic,version)
                table,count=struct.unpack_from('<II',data,64)
                assert count>0 and table+36*count<=len(data)
                slicer=struct.unpack_from('<I',data,104)[0]
                assert data[slicer+39]==version*0x10
                for index in range(count):
                    address,size,page,record_size=struct.unpack_from('<IIII',data,table+36*index+12)
                    full_address=page*2**32+address
                    assert size>0 and record_size==84 and 84<=full_address and full_address+size<=len(data)
                    assert struct.unpack_from('<I',data,full_address-84+36)[0]==size+84
            elif args.direct_export in ('ctb_encrypted','ctb_encrypted_v4'):
                import struct
                data=output.read_bytes()
                assert struct.unpack_from('<III',data)==(0x12FD0107,288,48)
                assert struct.unpack_from('<I',data,16)[0]==(4 if args.direct_export=='ctb_encrypted_v4' else 5)
                size,address=struct.unpack_from('<II',data,20)
                assert size==32 and address+36==len(data)
                assert struct.unpack_from('<I',data,len(data)-4)[0]==1833054899
            elif args.direct_export in ('nanodlp','nanodlp_v1','nanodlp_rgb','nanodlp_rgb_v1'):
                with zipfile.ZipFile(output) as archive:
                    meta=json.loads(archive.read('meta.json'))
                    profile=json.loads(archive.read('profile.json'))
                    slicer=json.loads(archive.read('slicer.json'))
                    assert meta['FormatVersion']==(1 if args.direct_export.endswith('_v1') else 2)
                    assert slicer['DisplayController']==(0 if '_rgb' in args.direct_export else 1)
                    assert profile['ShieldBeforeLayer']==json.loads(args.nanodlp_profile.read_text())['ShieldBeforeLayer']
                    assert profile['ShieldAfterLayer']==json.loads(args.nanodlp_profile.read_text())['ShieldAfterLayer']
                    names=[name for name in archive.namelist() if name.endswith('.png') and name[:-4].isdigit()]
                    assert len(names)==slicer['LayerCount']>0
            elif args.direct_export == 'svgx':
                import struct
                import xml.etree.ElementTree as ET
                data=output.read_bytes()
                assert data[:16]==b'DLP-II 1.1\n'+bytes(5)
                preview1,preview2,document=struct.unpack_from('<III',data,16)
                assert preview1==28 and preview1<preview2<document<len(data)
                assert data[preview1:preview1+2]==data[preview2:preview2+2]==b'BM'
                root=ET.fromstring(data[document:])
                ns={'s':'http://www.w3.org/2000/svg'}
                params=root.find('s:printparams',ns)
                count=int(params.attrib['layercount'])
                assert count>0 and len(root.findall('s:g',ns))==count+1
            elif args.direct_export == 'fdg':
                import struct
                data=output.read_bytes()
                assert struct.unpack_from('<II',data)==(0xBD3C7AC8,2)
                count=struct.unpack_from('<I',data,8)[0]
                table=struct.unpack_from('<I',data,52)[0]
                assert count>0 and table>=216 and table+36*count<=len(data)
                for index in range(count):
                    address,size,page=struct.unpack_from('<III',data,table+36*index+12)
                    assert size>0 and page*2**32+address+size<=len(data)
            elif args.direct_export == 'phz':
                import struct
                data=output.read_bytes()
                assert struct.unpack_from('<II',data)==(0x9FDA83AE,2)
                table,count=struct.unpack_from('<II',data,36)
                assert count>0 and table+36*count<=len(data)
                assert struct.unpack_from('<I',data,88)[0] == int(args.phz_encryption_key,0)
                for index in range(count):
                    address,size,page=struct.unpack_from('<III',data,table+36*index+12)
                    assert size>0 and page*2**32+address+size<=len(data)
            elif args.direct_export == 'zcode':
                import xml.etree.ElementTree as ET
                with zipfile.ZipFile(output) as archive:
                    root=ET.fromstring(archive.read('task.xml'))
                    count=int(root.findtext('Job/layers'));assert count>0
                    lines=archive.read('lcd.gcode').decode().splitlines()
                    assert lines and all(len(line)==88 and line.endswith('==') for line in lines)
                    assert all(f'{i+1}.png' in archive.namelist() for i in range(count))
            elif args.direct_export == 'zcodex':
                with zipfile.ZipFile(output) as archive:
                    resin=json.loads(archive.read('ResinMetadata'))
                    assert resin['TotalLayersCount']>0
                    assert '<Slice> 0' in archive.read('ResinGCodeData').decode()
                    for index in range(resin['TotalLayersCount']):
                        data=archive.read(f'ResinSlicesData/Slice{index:05}.png')
                        assert data[:8]==b'\x89PNG\r\n\x1a\n' and data[24:26]==bytes((8,0))
                        assert int.from_bytes(data[16:20],'big')==1440 and int.from_bytes(data[20:24],'big')==2560
            elif args.direct_export == 'jxs':
                with zipfile.ZipFile(output) as archive:
                    control=json.loads(archive.read('control.json'))
                    assert control['total_slices']>0 and len(control['action_list'])>control['total_slices']
                    assert any(a[0]=='slice' and a[1].startswith('{PWD}/') for a in control['action_list'])
                    for index in range(control['total_slices']):
                        data=archive.read(f'{index:05}.png')
                        assert data[:8]==b'\x89PNG\r\n\x1a\n' and data[24:26]==bytes((8,2))
                        assert int.from_bytes(data[16:20],'big')==1640 and int.from_bytes(data[20:24],'big')==2880
            elif args.direct_export == 'vdt':
                with zipfile.ZipFile(output) as archive:
                    manifest=json.loads(archive.read('manifest.json'))
                    assert manifest['file_version']==1 and len(manifest['layers'])>0
                    for index in range(len(manifest['layers'])):
                        assert archive.read(str(index)+'.png').startswith(b'\x89PNG\r\n\x1a\n')
                    assert manifest['machine']['resolution_x']==200
                    assert manifest['machine']['resolution_y']==120
            elif args.direct_export == 'qdt':
                lines=output.read_text().splitlines()
                assert lines[0]=='JieHe,50,400,800,2,019,0,FA'
                assert lines[-2]=='FD' and int(lines[-1])>0
                count=int(lines[-1])
                assert lines.count('FB')==count and lines.count('FC')==count
            elif args.direct_export in ('gr1','mdlp'):
                data=output.read_bytes()
                magic=b'\0\0\0\x07MKSDLP\0'
                assert data[:11]==magic and data[-11:]==magic
                at=11
                for size in (116,290):
                    at+=size*size*2;assert data[at:at+2]==b'\r\n';at+=2
                count=int.from_bytes(data[at:at+2],'big');assert count>0;at+=6
                for _ in range(3):
                    size=int.from_bytes(data[at:at+4],'big');at+=4
                    assert size>0 and size%2==0;data[at:at+size].decode('utf-16-be');at+=size
                at+=8 if args.direct_export=='mdlp' else 22
                for _ in range(count):
                    count_lines=int.from_bytes(data[at:at+4],'big');at+=4+6*count_lines
                    assert data[at:at+2]==b'\r\n';at+=2
                assert at+11==len(data)
            elif args.direct_export in ('lgs','lgs30','lgs120','lgs4k'):
                data=output.read_bytes()
                assert data[:8]==b'Longer3D'
                u32=lambda at: int.from_bytes(data[at:at+4],'little')
                assert u32(16)=={'lgs':10,'lgs30':30,'lgs120':120,'lgs4k':4500}[args.direct_export]
                count=u32(164);assert count>0
                at=180+u32(172)*u32(176)*2
                if args.direct_export=='lgs120':
                    size=u32(at);assert size>8 and data[at+4:at+12]==b'\x89PNG\r\n\x1a\n'
                    at+=size+6
                for _ in range(count):
                    size=u32(at);assert size>0 and at+4+size<=len(data)
                    at+=4+size
                assert at==len(data)
            elif args.direct_export == 'photons':
                data=output.read_bytes()
                assert data[:6] == b'\x00\x00\x00\x02\x00\x31'
                u32=lambda at: int.from_bytes(data[at:at+4],'big')
                assert (u32(82),u32(90)) == (224,168)
                at=98+224*168*2
                count=u32(at);at+=4
                assert count > 0
                for _ in range(count):
                    assert (u32(at),u32(at+12),u32(at+16)) == (44944,1440,2560)
                    size=(u32(at+20)>>3)-4
                    assert size > 0 and at+28+size <= len(data)
                    at+=28+size
                assert at == len(data)
            else:
                data=output.read_bytes()
                assert data[:15] == b'\x00\x00\x00\x09CXSW3DV2\x00\x00\x04'
                assert int.from_bytes(data[-4:],'big') == (zlib.crc32(data[:-4],0xffffffff)^0xffffffff)
            if args.shared_zip_switch:
                assert args.direct_export == 'klipper', 'Shared ZIP regression requires a Klipper Mono source'
                from PIL import Image
                from io import BytesIO
                def masks(path):
                    with zipfile.ZipFile(path) as archive:
                        names = sorted((name for name in archive.namelist()
                                        if name.endswith('.png') and name[:-4].isdigit()),
                                       key=lambda name: int(name[:-4]))
                        return [(Image.open(BytesIO(archive.read(name))).convert('L').size,
                                 Image.open(BytesIO(archive.read(name))).convert('L').tobytes()) for name in names]
                expected_masks = masks(output)
                assert len(expected_masks) == 6
                switched_outputs = []
                for label, stem in [('Generic ZIP', 'shared-generic'), ('Klipper MSLA (Mono)', 'shared-klipper')]:
                    click(wait_for(lambda: button('Export'), 'export after completed previous job'))
                    next_chooser = wait_for(lambda: find(app, lambda n: n.get_name().rstrip(':') == 'Save SLA file as'
                        and n.get_state_set().contains(Atspi.StateType.SHOWING)), 'shared suffix output dialog')
                    next_combos = []
                    def collect_next(node):
                        if node.get_role_name() == 'combo box' and node.get_state_set().contains(Atspi.StateType.SHOWING):
                            next_combos.append(node)
                        for i in range(node.get_child_count()): collect_next(node.get_child_at_index(i))
                    collect_next(next_chooser)
                    selected = False
                    for next_combo in next_combos:
                        next_option = find(next_combo, lambda n: n.get_role_name() in ('menu item', 'list item')
                            and label in n.get_name() and '*.zip' in n.get_name())
                        if next_option:
                            assert next_combo.get_selection_iface().select_child(next_option.get_index_in_parent())
                            selected = True
                            break
                    assert selected, 'Missing independent ZIP schema filter: ' + label
                    next_field = wait_for(lambda: find(next_chooser, lambda n: n.get_role_name() in ('text', 'entry')
                        and n.get_state_set().contains(Atspi.StateType.EDITABLE)
                        and n.get_state_set().contains(Atspi.StateType.SHOWING)), 'shared ZIP filename')
                    next_output = directory / (stem + '.zip')
                    assert next_field.get_editable_text_iface().set_text_contents(str(next_output))
                    click(named(next_chooser, 'Save'))
                    wait_for(lambda: next_output.is_file() and next_output.stat().st_size > 0, 'shared ZIP output', timeout=40)
                    with zipfile.ZipFile(next_output) as archive:
                        assert ('manifest.uvtools' in archive.namelist()) == (stem == 'shared-generic')
                        assert ('.klipper' in archive.namelist()) == (stem == 'shared-klipper')
                    assert masks(next_output) == expected_masks, 'Changing ZIP schemas changed sliced masks'
                    switched_outputs.append(str(next_output))
                report['shared_zip_outputs'] = switched_outputs
                report['checks'].append('same .zip suffix selects Generic ZIP and Klipper independently and preserves all masks')
            verify_application_network()
            report.update(status='passed',direct_output=str(output))
            report['checks'].append('sidebar export, explicit native filter and file structure')
            return
        click(wait_for(lambda: button('Edit layers'), 'completed slicing', timeout=40))
        dialog = wait_for(opened_editor, 'layer editor')
        report['checks'].append('model load, SLA slicing and opening cached layers from sidebar')
        if args.range_brush:
            from brush_range_ui import assert_paint_controls_above_preview
            report['painting_discoverability'] = assert_paint_controls_above_preview(dialog)
            tool=wait_for(lambda:find(dialog,lambda n:n.get_name()=='Brush' and n.get_role_name()=='combo box'),'explicit brush selector')
            assert tool.get_selection_iface().select_child(0)
            overlay=wait_for(lambda:named(dialog,'Overlay and paint selected range'),'range overlay control')
            spins=[]
            def scan_spins(node):
                if node.get_role_name()=='spin button' and node.get_state_set().contains(Atspi.StateType.SHOWING): spins.append(node)
                for i in range(node.get_child_count()):scan_spins(node.get_child_at_index(i))
            scan_spins(dialog);assert len(spins)==5
            assert spins[3].get_value_iface().set_current_value(2)
            assert spins[4].get_value_iface().set_current_value(4)

            click(overlay)
            preview=wait_for(lambda:find(dialog,lambda n:n.get_role_name()=='scroll pane' and n.get_state_set().contains(Atspi.StateType.SHOWING) and not find(n,lambda child:child.get_role_name()=='combo box')),'mask viewport')
            bounds=preview.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
            import ctypes
            x=ctypes.CDLL('libX11.so.6');xt=ctypes.CDLL('libXtst.so.6')
            x.XOpenDisplay.argtypes=[ctypes.c_char_p];x.XOpenDisplay.restype=ctypes.c_void_p
            x.XFlush.argtypes=[ctypes.c_void_p];x.XCloseDisplay.argtypes=[ctypes.c_void_p]
            xt.XTestFakeMotionEvent.argtypes=[ctypes.c_void_p,ctypes.c_int,ctypes.c_int,ctypes.c_int,ctypes.c_ulong]
            xt.XTestFakeButtonEvent.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.c_int,ctypes.c_ulong]
            def drag_mask():
                bounds=preview.get_component_iface().get_extents(Atspi.CoordType.SCREEN)
                display=x.XOpenDisplay(None);assert display
                try:
                    assert bounds.width>30 and bounds.height>30
                    xt.XTestFakeMotionEvent(display,-1,bounds.x+14,bounds.y+14,0)
                    xt.XTestFakeButtonEvent(display,1,1,0);x.XFlush(display);time.sleep(.2)
                    xt.XTestFakeMotionEvent(display,-1,bounds.x+29,bounds.y+14,0)
                    x.XFlush(display);time.sleep(.2)
                    xt.XTestFakeButtonEvent(display,1,0,0);x.XFlush(display)
                finally:x.XCloseDisplay(display)
            drag_mask()
            wait_for(lambda:named(dialog,'Undo').get_state_set().contains(Atspi.StateType.SENSITIVE) and not find(app,lambda n:n.get_name()=='Paint layers' and n.get_role_name() in ('dialog','frame') and n.get_state_set().contains(Atspi.StateType.SHOWING)),'mouse stroke committed')
            click(named(dialog,'Undo'));wait_for(lambda:not editor_changed(dialog),'single Undo range stroke')
            click(named(dialog,'Redo'));wait_for(lambda:editor_changed(dialog),'Redo range stroke')
            click(named(dialog,'Undo'));wait_for(lambda:not editor_changed(dialog),'restored original masks')
            assert tool.get_selection_iface().select_child(1),'Eraser choice missing'
            assert tool.get_selection_iface().select_child(0)
            click(overlay)
            report['checks'].append('explicit Brush/Eraser, range overlay, real mouse stroke and single Undo/Redo')
        def model_exposure(editor, value=None):
            click(named(editor,'Layer parameters'))
            panel=wait_for(lambda:find(app,lambda n:n.get_name().startswith('Layer parameters') and
                n.get_role_name() in ('dialog','frame') and n.get_state_set().contains(Atspi.StateType.SHOWING)),'model parameter panel')
            editable=[]
            def scan(node):
                if node.get_role_name() in ('text','entry') and node.get_state_set().contains(Atspi.StateType.EDITABLE) and node.get_state_set().contains(Atspi.StateType.SHOWING): editable.append(node)
                for i in range(node.get_child_count()):scan(node.get_child_at_index(i))
            scan(panel);assert len(editable)==13
            if value is None:
                actual=Atspi.Text.get_text(editable[0],0,-1)
                click(named(panel,'Cancel'));return float(actual)
            assert editable[0].get_editable_text_iface().set_text_contents(str(value))
            click(named(panel,'Apply parameters'))
            wait_for(lambda:editor_changed(editor),'model parameter dirty')
        if args.owned_parameters:
            model_exposure(dialog,7.12)
            click(named(dialog,'Undo'));wait_for(lambda:not editor_changed(dialog),'model parameter undo')
        if args.image_mask_export:
            from PIL import Image
            def choose_image(button_name, title, basename, extension, range_export=False):
                click(named(dialog,button_name))
                chooser=wait_for(lambda: find(app,lambda n:n.get_name()==title and n.get_role_name()=='file chooser'),title)
                combo=wait_for(lambda: find(chooser,lambda n:n.get_role_name()=='combo box' and find(n,lambda c:('*.'+extension) in c.get_name())),'image format list')
                selection=combo.get_selection_iface()
                option=find(combo,lambda n:n.get_role_name() in ('menu item','list item') and ('*.'+extension) in n.get_name())
                assert selection.select_child(option.get_index_in_parent())
                field=wait_for(lambda: find(chooser,lambda n:n.get_role_name() in ('text','entry') and
                    n.get_state_set().contains(Atspi.StateType.EDITABLE) and n.get_state_set().contains(Atspi.StateType.SHOWING)),'mask filename')
                wait_for(lambda: Atspi.Text.get_text(field,0,-1).endswith('.'+extension),'automatic image suffix')
                output=directory/(basename+'.'+extension)
                field.get_editable_text_iface().set_text_contents(str(output))
                click(named(chooser,'Save'))
                if range_export:
                    alert=wait_for(lambda: find(app,lambda n:n.get_role_name()=='alert' and
                        find(n,lambda c:'as separate images into a new folder' in c.get_name())),'mask range destination confirmation')
                    click(named(alert,'Yes'))
                    output=directory/(basename+'_layers')
                    wait_for(lambda: output.is_dir() and len(list(output.iterdir()))>1,'complete mask range')
                else: wait_for(lambda: output.is_file() and output.stat().st_size>0,'current image mask')
                wait_for(lambda: dialog.get_state_set().contains(Atspi.StateType.SENSITIVE),'image export completed')
                return output
            single=choose_image('Export current mask','Export selected mask','current','bmp')
            with Image.open(single) as mask:
                fields=dict(line.split(' = ',1) for line in configuration.read_text().splitlines() if ' = ' in line)
                expected=(200,120)
                if fields.get('display_orientation')=='portrait': expected=expected[::-1]
                assert mask.size==expected,mask.size
                reference=mask.convert('L').tobytes()
            masks=choose_image('Export mask range','Export range to a new mask folder','range','pgm',True)
            files=sorted(masks.glob('layer-*.pgm'))
            assert len(files)>1
            with Image.open(files[0]) as mask: assert mask.convert('L').tobytes()==reference
            assert not editor_changed(dialog),'Mask export changed document revision'
            report['checks'].append('image filters, automatic suffix, current mask and complete numbered range')
            report['image_exports']={'single':str(single),'range':str(masks),'count':len(files)}
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
        assert choices[1].get_selection_iface().select_child(zoom_index), 'Zoom choice rejected ' + zoom_label
        time.sleep(.3)
        assert not editor_changed(dialog), 'Zoom modified mask data'
        assert choices[2].get_selection_iface().select_child(1), 'Range choice rejected Invert'
        click(named(dialog, 'Apply to range'))
        wait_for(lambda: editor_changed(dialog), 'completed range operation')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not editor_changed(dialog), 'single Undo of entire range')
        report['checks'].append(zoom_label + ' zoom, range invert and one Undo of all layers')
        assert choices[2].get_selection_iface().select_child(7), 'Range choice rejected Dilate'
        click(named(dialog, 'Apply to range'))
        wait_for(lambda: editor_changed(dialog), 'completed grayscale dilation')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not editor_changed(dialog), 'single Undo of dilation')
        report['checks'].append('grayscale dilation is accessible and undone as one range edit')
        assert choices[2].get_selection_iface().select_child(13), 'Range choice rejected Box blur'
        click(named(dialog, 'Apply to range'))
        wait_for(lambda: editor_changed(dialog), 'completed box blur')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not editor_changed(dialog), 'single Undo of box blur')
        click(named(dialog, 'Redo'))
        wait_for(lambda: editor_changed(dialog), 'Redo of box blur')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not editor_changed(dialog), 'restored original after box blur')
        report['checks'].append('box blur is accessible on the selected range with atomic Undo/Redo')
        assert choices[2].get_selection_iface().select_child(14), 'Range choice rejected Median blur'
        click(named(dialog, 'Apply to range'))
        wait_for(lambda: editor_changed(dialog), 'completed median blur')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not editor_changed(dialog), 'single Undo of median blur')
        click(named(dialog, 'Redo'))
        wait_for(lambda: editor_changed(dialog), 'Redo of median blur')
        click(named(dialog, 'Undo'))
        wait_for(lambda: not editor_changed(dialog), 'restored original after median blur')
        report['checks'].append('median blur is accessible on the selected range with atomic Undo/Redo')
        click(named(dialog, 'Invert layer'))
        assert editor_changed(dialog), 'Edit did not mark the document dirty'
        click(named(dialog, 'Undo'))
        assert not editor_changed(dialog), 'Undo did not restore the original revision'
        click(named(dialog, 'Redo'))
        assert editor_changed(dialog), 'Redo did not restore the edit'
        report['checks'].append('invert, Undo and Redo update the live dialog')
        if args.range_brush:
            assert choices[1].get_selection_iface().select_child(0), 'Fit zoom rejected'
            value=wait_for(lambda:find(dialog,lambda n:n.get_role_name()=='spin button' and
                n.get_value_iface().get_current_value()==255),'brush grayscale field')
            assert value.get_value_iface().set_current_value(127)
            click(overlay)
            time.sleep(.2)
            drag_mask()
            wait_for(lambda:named(dialog,'Undo').get_state_set().contains(Atspi.StateType.SENSITIVE) and
                value.get_state_set().contains(Atspi.StateType.SENSITIVE) and
                not find(app,lambda n:n.get_name()=='Paint layers' and n.get_role_name() in ('dialog','frame') and
                n.get_state_set().contains(Atspi.StateType.SHOWING)),'persisted range stroke commit')
            click(overlay)

        click(named(dialog, 'Apply to sliced result'))
        assert not editor_changed(dialog), 'Apply did not commit the revision'
        close_editor(dialog)
        time.sleep(.5)
        if args.settings_reopen:
            click(named(app,'Window'))
            click(wait_for(lambda:find(app,lambda n:n.get_role_name()=='menu item' and
                n.get_name().startswith('Printer Settings Tab') and n.get_state_set().contains(Atspi.StateType.SHOWING)), 'printer settings tab'))
            click(named(app,'Window'))
            action=wait_for(lambda:find(app,lambda n:n.get_role_name()=='menu item' and
                n.get_name().startswith('Edit sliced layers') and n.get_state_set().contains(Atspi.StateType.SHOWING)), 'edit layers from settings')
            assert action.get_state_set().contains(Atspi.StateType.SENSITIVE)
            click(action)
        else:
            click(wait_for(lambda: button('Edit layers'), 'reopen action'))
        reopened = wait_for(opened_editor, 'reopened editor')
        assert named(reopened, 'Undo').get_state_set().contains(Atspi.StateType.SENSITIVE), 'Applied edit history disappeared'
        if args.owned_parameters:
            model_exposure(reopened,7.12)
            click(named(reopened,'Apply to sliced result'))
            wait_for(lambda:not editor_changed(reopened),'applied model parameter')
            assert abs(model_exposure(reopened)-7.12)<1e-8
            report['checks'].append('actual model parameter edit/Undo/Apply and retained exposure7.12')
        close_editor(reopened)
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
                            layer = archive.read(f'Metadata/SLA/document/{manifest["document"]["main"][0]:08d}.bin') if manifest['version']>=2 else archive.read('Metadata/SLA/layers/00000000.png')
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
            if args.range_brush:
                import io
                from PIL import Image
                painted=[]
                with zipfile.ZipFile(project) as saved:
                    for layer,index in enumerate(manifest['document']['main']):
                        data=saved.read(f'Metadata/SLA/document/{index:08d}.bin')
                        crop=Image.open(io.BytesIO(data)).convert('L').crop((0,0,40,30)).tobytes()
                        pixels=[i for i,v in enumerate(crop) if v==127]
                        if 1<=layer<=3:
                            assert pixels, 'Range brush pixels missing from a selected saved layer'
                            painted.append(pixels)
                        else:
                            assert not pixels, 'Range brush changed an unselected layer'
                assert all(pixels==painted[0] for pixels in painted), 'Stroke differs across the selected range'
                report['range_brush_persisted']={'layers':len(painted),'pixels_per_layer':len(painted[0]),'gray':127,'selected_layers':[2,3,4]}
                report['checks'].append('all selected masks retain the same range stroke in the applied 3MF')

            assert len(manifest['positions_z']) == manifest['layers']
            assert manifest['settings']['material']['values']['exposure_time'] == '2'
            assert manifest['source_kind'] == 'model_slice'
            if args.preset_metadata_roundtrip:
                assert manifest['settings']['version']==2
                for role in ('printer','material','process'):
                    assert manifest['settings'][role]['metadata']==metadata_fixture[role], role+' metadata changed on initial save'
                report['checks'].append('all role metadata and separate defaults stored without installed parent presets')
            report['checks'].append('3MF contains edited PNGs, layer heights and source fingerprint')
            verify_application_network()
            process.terminate(); process.wait(timeout=10)
            if args.printer_preference_roundtrip:
                # Change only this test's private app preference between sessions.
                # The embedded printer retains SL1, so a contextual-label lookup
                # would fall back to SL1 instead of the source printer's CXDLPv4.
                source_key='printer_'+manifest['settings']['printer']['name'].encode('utf-8').hex()
                ini=datadir/'PrusaSlicer.ini'
                lines=ini.read_text().splitlines()
                updated=[]; in_preferences=False; inserted=False
                for line in lines:
                    if line.strip().startswith('['):
                        in_preferences=line.strip()=='[sla_export_formats]'
                        updated.append(line)
                        if in_preferences:
                            updated.append(source_key+' = cxdlpv4'); inserted=True
                    elif not (in_preferences and line.split('=',1)[0].strip()==source_key):
                        updated.append(line)
                if not inserted: updated.extend(['','[sla_export_formats]',source_key+' = cxdlpv4'])
                ini.write_text('\n'.join(updated)+'\n')
            with (directory / 'application-reopened.log').open('w') as reopened_log:
                process = subprocess.Popen([str(args.binary.resolve()), '--datadir', str(datadir), str(project)],
                    stdout=reopened_log, stderr=subprocess.STDOUT,
                    env=application_env)
            app = wait_for(lambda: find(desktop, lambda n: n.get_role_name() == 'application' and
                n.get_process_id() == process.pid), 'reopened application')
            application_pid = process.pid
            desktop.clear_cache();app.clear_cache()
            startup = wait_for(lambda: find(app, lambda n: n.get_name().endswith('Load project file') and
                n.get_state_set().contains(Atspi.StateType.SHOWING)) or button('Slice now'), 'project startup')
            if startup.get_name().endswith('Load project file'):
                choice = named(startup, 'Open as project')
                if not choice.get_state_set().contains(Atspi.StateType.CHECKED): click(choice)
                click(named(startup, 'OK'))
            click(wait_for(lambda: button('Slice now'), 'reopened project slicing action'))
            try:
                restored_button=wait_for(lambda: button('Edit layers'), 'restored sliced result', timeout=40)
            except Exception:
                observed=[]
                def snapshot(node,depth=0):
                    if depth>16:return
                    try:
                        if node.get_state_set().contains(Atspi.StateType.SHOWING):
                            observed.append({'role':node.get_role_name(),'name':node.get_name()})
                        for i in range(node.get_child_count()):snapshot(node.get_child_at_index(i),depth+1)
                    except Exception:pass
                desktop.clear_cache()
                current=find(desktop,lambda n:n.get_role_name()=='application' and n.get_process_id()==process.pid)
                if current:current.clear_cache();snapshot(current)
                else: snapshot(desktop)
                (directory/'restart-observed-ui.json').write_text(json.dumps(observed,indent=2)+'\n')
                diagnostic={'process_pid':process.pid,'poll':process.poll(),'desktop_children':desktop.get_child_count(),
                    'app_name':app.get_name(),'app_role':app.get_role_name(),'app_children':app.get_child_count()}
                (directory/'restart-process-state.json').write_text(json.dumps(diagnostic,indent=2)+'\n')
                subprocess.run(['xwd','-root','-silent','-out',str(directory/'restart-screen.xwd')],capture_output=True,env=application_env)
                raise
            click(restored_button)
            restored_dialog = wait_for(opened_editor, 'restored editor')
            if args.owned_parameters:
                assert abs(model_exposure(restored_dialog)-7.12)<1e-8
                report['checks'].append('manual actualModel exposure survives 3MF application restart')
            if args.printer_preference_roundtrip:
                restored_choices=[]
                def scan_restored(node):
                    if node.get_role_name()=='combo box' and node.get_state_set().contains(Atspi.StateType.SHOWING):
                        restored_choices.append(node)
                    for index in range(node.get_child_count()): scan_restored(node.get_child_at_index(index))
                scan_restored(restored_dialog)
                selected=restored_choices[-1].get_selection_iface().get_selected_child(0)
                assert selected and selected.get_name().endswith('(.cxdlpv4)'), '3MF label hid the source printer export preference'
                report['checks'].append('source printer CXDLPv4 preference survives contextual 3MF label and application restart')
            assert not named(restored_dialog, 'Undo').get_state_set().contains(Atspi.StateType.SENSITIVE), \
                'Restored result incorrectly retained an old session history'
            close_editor(restored_dialog)
            if args.printer_preference_roundtrip:
                export=wait_for(lambda:find(app,lambda n:n.get_role_name()=='button' and
                    n.get_name().startswith('Export') and n.get_state_set().contains(Atspi.StateType.SHOWING) and
                    n.get_state_set().contains(Atspi.StateType.SENSITIVE)),'reopened sidebar export')
                click(export)
                chooser=wait_for(lambda:find(app,lambda n:n.get_name().rstrip(':')=='Save SLA file as' and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)),'preferred sidebar export dialog')
                field=wait_for(lambda:find(chooser,lambda n:n.get_role_name() in ('text','entry') and
                    n.get_state_set().contains(Atspi.StateType.EDITABLE) and n.get_state_set().contains(Atspi.StateType.SHOWING)),'preferred export filename')
                wait_for(lambda:Atspi.Text.get_text(field,0,-1).endswith('.cxdlpv4'),'source printer preferred sidebar suffix')
                click(named(chooser,'Cancel'))
                report['checks'].append('sidebar export initializes the CXDLPv4 filter and filename suffix after 3MF restart')
            manifest2, mask2 = save_project(directory / 'resaved-project.3mf')
            assert first_pixel(mask2) == 255, 'Re-slicing or re-saving discarded the restored edit'
            assert manifest2['positions_z'] == manifest['positions_z']
            assert manifest2['settings'] == manifest['settings'], 'Reopening replaced preset source identities with project labels'
            if args.preset_metadata_roundtrip:
                for role in ('printer','material','process'):
                    assert manifest2['settings'][role]['metadata']==metadata_fixture[role]
                report['checks'].append('3MF restart and resave preserve source models, inheritance references, lists, conditions and role defaults')
            assert mask2 == mask, 'Restored mask bytes changed'
            if installed_files:
                from hashlib import sha256
                for profile,digest in installed_files.items():
                    assert sha256(profile.read_bytes()).hexdigest()==digest, 'Project import overwrote an installed preset'
                report['checks'].append('project metadata overrides equal print settings while three installed preset files remain unchanged')
            report['checks'].append('restart, slice, empty session Undo history and re-save preserve edited PNGs')
        if args.sidebar_owned_export:
            output=directory/'sidebar-edited-result.cxdlpv4'
            def choose_sidebar_output(destination=output):
                export=wait_for(lambda:find(app,lambda n:n.get_role_name()=='button' and
                    n.get_name().startswith('Export') and n.get_state_set().contains(Atspi.StateType.SHOWING) and
                    n.get_state_set().contains(Atspi.StateType.SENSITIVE)),'owned sidebar export action')
                click(export)
                chooser=wait_for(lambda:find(app,lambda n:n.get_name().rstrip(':')=='Save SLA file as' and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)),'owned sidebar chooser')
                combo=wait_for(lambda:find(chooser,lambda n:n.get_role_name()=='combo box' and
                    find(n,lambda c:'*.cxdlpv4' in c.get_name())),'owned sidebar format list')
                option=find(combo,lambda n:n.get_role_name() in ('menu item','list item') and '*.cxdlpv4' in n.get_name())
                assert combo.get_selection_iface().select_child(option.get_index_in_parent())
                field=wait_for(lambda:find(chooser,lambda n:n.get_role_name() in ('text','entry') and
                    n.get_state_set().contains(Atspi.StateType.EDITABLE) and n.get_state_set().contains(Atspi.StateType.SHOWING)),'owned sidebar filename')
                wait_for(lambda:Atspi.Text.get_text(field,0,-1).endswith('.cxdlpv4'),'owned sidebar automatic suffix')
                assert field.get_editable_text_iface().set_text_contents(str(destination))
                click(named(chooser,'Save'))
                return wait_for(lambda:find(app,lambda n:n.get_role_name()=='alert' and
                    n.get_name()=='Question' and n.get_state_set().contains(Atspi.StateType.SHOWING)),'owned sidebar palette consent')
            confirmation=choose_sidebar_output()
            wait_for(lambda:find(confirmation,lambda n:'128 grayscale levels' in n.get_name()),'owned sidebar precision explanation')
            click(named(confirmation,'No'))
            wait_for(lambda:button('Edit layers'),'sliced result still available after declining export')
            assert not output.exists(), 'Sidebar No published a file'
            report['checks'].append('sidebar palette No preserves sliced workspace and publishes no output')
            if args.sidebar_export_failure:
                blocked = directory / 'blocked.cxdlpv4'
                confirmation = choose_sidebar_output(blocked)
                # Create a nonempty directory only after the file chooser has
                # accepted this filename. The real encoder runs, but atomic
                # publication must fail even when the test runs as root.
                blocked.mkdir()
                marker = blocked / 'keep.txt'
                marker.write_text('previous destination remains intact')
                click(named(confirmation, 'Yes'))
                alert = wait_for(lambda: find(app, lambda n: n.get_role_name() in ('alert', 'frame') and
                    (n.get_name() == 'Error' or n.get_name().endswith(' error')) and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)),
                    'failed publication error', timeout=40)
                # The custom wxHtmlWindow body is not exposed through AT-SPI.
                # Retain a screenshot for review of the exact I/O diagnostic.
                subprocess.run(['import', '-window', 'root', str(directory / 'publication-error.png')],
                    check=True, capture_output=True, timeout=5)
                assert marker.read_text() == 'previous destination remains intact'
                assert not list(directory.glob('.sla-export-*')), 'Failed export left staging data'
                click(named(alert, 'OK'))
                wait_for(lambda: button('Edit layers'), 'editable result after export error')
                app.clear_cache()
                assert not find(app, lambda n: n.get_name() == 'Invalid data' and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)), 'Export error invalidated valid slices'
                wait_for(lambda: find(app, lambda n:
                    any(label in n.get_name() for label in ('Time without printer setup', 'Estimated printing time')) and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)), 'sliced statistics preserved after export error')
                report['checks'].append('failed publication keeps previous destination, removes staging and preserves editable slices and statistics')
            confirmation=choose_sidebar_output();click(named(confirmation,'Yes'))
            wait_for(lambda:output.exists() and output.stat().st_size>0,'owned sidebar output',timeout=40)
            data=output.read_bytes()
            assert data[:15]==b'\x00\x00\x00\x09CXSW3DV2\x00\x00\x04'
            assert int.from_bytes(data[-4:],'big')==(zlib.crc32(data[:-4],0xffffffff)^0xffffffff)
            report['requested_native_output']=str(output);report['cxdlpv4_output']=str(output)
            report['checks'].append('sidebar Yes exports a frozen copy after brush/range/3MF restart with automatic CXDLPv4 suffix')
            if args.sidebar_export_failure:
                report['checks'].append('successful retry after failed publication without reslicing or losing edited source')
        if not args.sidebar_owned_export and (args.klipper_export or args.sl1_export or args.cwsrgb_export or args.novamaker_export or args.cwsxml_export or args.osf_export or args.genericzip_export or args.vda_export or args.chitubox_export or args.anycubic_export or args.cxdlpv1_export or args.cxdlpv3_export or args.cxdlpv2_export or args.cxdlpv4_export):
            extension = '.rgb.zip' if args.klipper_export=='klipper_rgb' else '.zip' if args.klipper_export else '.sl1s' if args.sl1s_export else '.sl1' if args.sl1_export else '.rgb.cws' if args.cwsrgb_export else '.cws' if args.novamaker_export else '.xml.cws' if args.cwsxml_export else '.cxdlpv4' if args.cxdlpv4_export else '.v2.cxdlp' if args.cxdlpv2_export else '.cxdlp' if args.cxdlpv3_export else '.v1.cxdlp' if args.cxdlpv1_export else '.' + args.anycubic_export if args.anycubic_export else ".zip" if args.genericzip_export or args.vda_export or args.chitubox_export else ".osf"
            click(wait_for(lambda: button('Edit layers'), 'OSF export editor action'))
            editor = wait_for(opened_editor, 'OSF export editor')
            export_choices = []
            def scan(node):
                if node.get_role_name() == 'combo box' and node.get_state_set().contains(Atspi.StateType.SHOWING):
                    export_choices.append(node)
                for index in range(node.get_child_count()): scan(node.get_child_at_index(index))
            scan(editor)
            formats = export_choices[-1]
            option = find(formats, lambda n: n.get_role_name() in ('menu item', 'list item') and n.get_name().endswith('(' + extension + ')') and (not args.novamaker_export or 'NovaMaker CWS' in n.get_name()) and (not args.vda_export or 'Voxeldance Additive' in n.get_name()) and (not args.genericzip_export or 'Generic ZIP' in n.get_name()) and (not args.chitubox_export or 'Chitubox ZIP' in n.get_name()) and (not args.klipper_export or ('Klipper MSLA' in n.get_name() and ('RGB' if args.klipper_export=='klipper_rgb' else 'Mono') in n.get_name())))
            assert option is not None, 'OSF is unavailable in the export chooser'
            assert formats.get_selection_iface().select_child(option.get_index_in_parent())
            def matching_guidance(node,prefix):
                # The label changes after the choice event; don't reuse its
                # cached empty name from the previous selection.
                node.clear_cache()
                return node.get_name().startswith(prefix)
            if args.klipper_export:
                wait_for(lambda:find(editor,lambda n:matching_guidance(n,'Klipper ZIP requires matching MSLA')), 'Klipper MSLA firmware guidance')
                report['checks'].append('Klipper MSLA capability guidance is visible before export')
            if args.anycubic_export == 'pwc' and args.anycubic_version in ('1','515'):
                wait_for(lambda:find(editor,lambda n:matching_guidance(n,'PWC 1/515 saves masks')), 'PWC legacy missing-geometry guidance')
                report['checks'].append('PWC 1/515 missing machine geometry is visible before export')
            if args.anycubic_export == 'dl2p' and args.anycubic_version == '515':
                wait_for(lambda:find(editor,lambda n:matching_guidance(n,'This legacy format requires')), 'legacy fixed-geometry guidance')
                report['checks'].append('DL2P 515 fixed printer geometry is visible before export')
            if args.anycubic_tsmc:
                prefix = 'Anycubic 518' if args.anycubic_version == '518' or (args.anycubic_version == 'auto' and args.anycubic_export in ('pm5s', 'm5sp')) else 'Anycubic 516/517'
                note = wait_for(lambda: find(editor, lambda n: n.get_name().startswith(prefix + ' supports')), 'Anycubic motion guidance')
                text = ' '.join(note.get_name().split())
                assert 'two-stage lift' in text and 'LCD exposure and motion' in text
                report['checks'].append('Anycubic UI explains supported motion and where to configure it')
            click(named(editor, 'Export edited layers'))
            if args.missing_printer_identifier:
                alert=wait_for(lambda:find(app,lambda n:n.get_role_name()=='alert' and n.get_name()=='Error' and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)), 'early printer identity refusal')
                wait_for(lambda:find(alert,lambda n:'printer model identifier' in n.get_name() and
                    'Printer > General > Output > Printer model (firmware)' in n.get_name()), 'actionable profile correction guidance')
                assert not find(app,lambda n:n.get_name()=='Save edited layers' and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)), 'Identity refusal opened file chooser'
                assert not list(directory.glob('*.cxdlpv4')), 'Identity refusal published output'
                click(named(alert,'OK'))
                assert abs(model_exposure(editor)-7.12)<1e-8
                close_editor(editor)
                report['checks'].append('early printer identity refusal before chooser/palette with actionable profile guidance and preserved edits')
            else:
                chooser = wait_for(lambda: find(app, lambda n: n.get_name() == 'Save edited layers' and
                    n.get_state_set().contains(Atspi.StateType.SHOWING)), 'OSF output chooser')
                filename = find(chooser, lambda n: n.get_role_name() in ('text', 'entry') and
                    n.get_state_set().contains(Atspi.StateType.EDITABLE))
                assert Atspi.Text.get_text(filename, 0, -1).lower().endswith(extension), 'Format selection did not update the default filename'
                output = directory / ('edited-result' + extension)
                assert filename.get_editable_text_iface().set_text_contents(str(output))
                report['requested_native_output'] = str(output)
                wait_for(lambda: Atspi.Text.get_text(filename,0,-1) == str(output), 'native filename edit applied')
                click(named(chooser, 'Save'))
                if args.owned_parameters and args.cxdlpv4_export:
                    confirmation = wait_for(lambda: find(app, lambda n: n.get_role_name() == 'alert' and n.get_name() == 'Question' and
                        n.get_state_set().contains(Atspi.StateType.SHOWING)), 'CXDLPv4 palette conversion choice')
                    wait_for(lambda: find(confirmation, lambda n: '128 grayscale levels' in n.get_name()),
                        'CXDLPv4 precision explanation')
                    click(named(confirmation, 'No'))
                    assert not output.exists(), 'Declined palette conversion published a file'
                    assert abs(model_exposure(editor)-7.12)<1e-8
                    report['checks'].append('declined CXDLPv4 palette conversion preserves parameters and publishes no file')
                    click(named(editor, 'Export edited layers'))
                    chooser = wait_for(lambda: find(app, lambda n: n.get_name() == 'Save edited layers' and
                        n.get_state_set().contains(Atspi.StateType.SHOWING)), 'retry CXDLPv4 chooser')
                    filename = find(chooser, lambda n: n.get_role_name() in ('text', 'entry') and
                        n.get_state_set().contains(Atspi.StateType.EDITABLE))
                    assert filename.get_editable_text_iface().set_text_contents(str(output))
                    wait_for(lambda: Atspi.Text.get_text(filename,0,-1)==str(output), 'retry filename applied')
                    click(named(chooser, 'Save'))
                    confirmation = wait_for(lambda: find(app, lambda n: n.get_role_name() == 'alert' and n.get_name() == 'Question' and
                        n.get_state_set().contains(Atspi.StateType.SHOWING)), 'retry palette conversion choice')
                    click(named(confirmation, 'Yes'))
                    report['checks'].append('CXDLPv4 explicit export-copy palette conversion confirmed')
                if args.owned_parameters and args.sl1_export:
                    # SL1 encodes a global/transition schedule, not arbitrary rows.
                    # The manually changed first exposure must be refused intact.
                    alert = wait_for(lambda: find(app, lambda n: n.get_role_name() == 'alert' and
                        n.get_name() == 'Error' and n.get_state_set().contains(Atspi.StateType.SHOWING)),
                        'SL1 incompatible owned schedule refusal')
                    wait_for(lambda: find(alert, lambda n: 'SL1 cannot preserve layer[0].Exposure' in n.get_name()),
                        'specific SL1 exposure refusal')
                    assert not output.exists(), 'Rejected SL1 export published a file'
                    click(named(alert, 'OK'))
                    assert abs(model_exposure(editor)-7.12)<1e-8, 'Refusal changed authored exposure'
                    close_editor(editor)
                    report['checks'].append('SL1 arbitrary-row refusal identifies exposure, publishes no file and preserves edits')
                else:
                    wait_for(lambda: output.is_file() and output.stat().st_size > (0 if args.klipper_export or args.sl1_export or args.cwsrgb_export or args.novamaker_export or args.cwsxml_export or args.genericzip_export or args.vda_export or args.chitubox_export or args.anycubic_export or args.cxdlpv4_export else 350001), 'native export output', timeout=40)
                    if args.klipper_export:
                        from PIL import Image
                        from io import BytesIO
                        with zipfile.ZipFile(output) as archive:
                            assert '.klipper' in archive.namelist() and 'config.ini' not in archive.namelist()
                            assert 'job.gcode' in archive.namelist()
                            image=Image.open(BytesIO(archive.read('1.png')))
                            assert image.mode==('RGB' if args.klipper_export=='klipper_rgb' else 'L')
                            assert image.convert('L').getpixel((0,0))==255, 'Edited mask missing from Klipper output'
                    elif args.sl1_export:
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
                        assert version == (int(args.anycubic_version) if args.anycubic_version != 'auto' else {'pws': 1, 'pwc': 516, 'dl2p': 517, 'pm5': 517, 'pm3n': 517, 'px6s': 517, 'pm5s': 518, 'm5sp': 518}[args.anycubic_export])
                        if version >= 516:
                            offset = int.from_bytes(data[44:48], 'little')
                            assert data[offset:offset+7] == b'MACHINE'
                        if args.anycubic_tsmc:
                            import struct
                            u32=lambda at:int.from_bytes(data[at:at+4],'little')
                            f32=lambda at:struct.unpack_from('<f',data,at)[0]
                            extra=u32(40);table=u32(36)
                            assert f32(extra+20)==5 and f32(extra+32)==3
                            assert f32(extra+48)==3 and f32(extra+60)==2.5
                            assert f32(extra+40)==3.5 and f32(extra+68)==2.5
                            assert f32(table+28)==8 and f32(table+60)==5.5
                            report['checks'].append('distinct bottom/normal TSMC tables and total layer lift')
                        if version == 518:
                            u32=lambda at:int.from_bytes(data[at:at+4],'little')
                            assert u32(20)==64 and u32(u32(20)+12)==96
                            sub=u32(56);second=u32(60)
                            assert data[sub:sub+7]==b'SUBIMGS' and u32(sub+16)==u32(u32(36)+16)
                            assert data[second:second+8]==b'PREVIEW2'
                            assert (u32(second+16),u32(second+24))==(330,190)
                            assert second+28+330*190*2==u32(48)
                    else:
                        header = output.read_bytes()[:7]
                        assert int.from_bytes(header[:4], 'big') == 350001 and header[4:6] == b'\x00\x04'
                    wait_for(lambda: not find(app, lambda n: n.get_name() == 'Export layers' and
                        n.get_state_set().contains(Atspi.StateType.SHOWING)), 'OSF export completion')
                    close_editor(editor)
                    report['klipper_output' if args.klipper_export else 'sl1_output' if args.sl1_export else 'cwsrgb_output' if args.cwsrgb_export else 'novamaker_output' if args.novamaker_export else 'cwsxml_output' if args.cwsxml_export else 'cxdlpv4_output' if args.cxdlpv4_export else 'cxdlpv2_output' if args.cxdlpv2_export else 'cxdlpv3_output' if args.cxdlpv3_export else 'cxdlpv1_output' if args.cxdlpv1_export else 'chitubox_output' if args.chitubox_export else 'anycubic_output' if args.anycubic_export else 'vda_output' if args.vda_export else 'genericzip_output' if args.genericzip_export else 'osf_output'] = str(output)
                    report['checks'].append(('Klipper MSLA' if args.klipper_export else 'SL1S' if args.sl1s_export else 'SL1' if args.sl1_export else 'RGB.CWS' if args.cwsrgb_export else 'NovaMaker CWS' if args.novamaker_export else 'XML.CWS' if args.cwsxml_export else 'CXDLPv4' if args.cxdlpv4_export else 'CXDLPv2' if args.cxdlpv2_export else 'CXDLPv3' if args.cxdlpv3_export else 'CXDLPv1' if args.cxdlpv1_export else 'Chitubox ZIP' if args.chitubox_export else args.anycubic_export.upper() if args.anycubic_export else 'Voxeldance Additive ZIP' if args.vda_export else 'Generic ZIP' if args.genericzip_export else 'OSF') + ' GUI choice, automatic filename suffix and native export')
        if args.owned_parameters and args.cxdlpv4_export and args.project_roundtrip:
            unchanged, unchanged_mask = save_project(directory / 'post-export-project.3mf')
            assert unchanged_mask == mask2, 'Palette export changed original source mask'
            assert unchanged['document']['fresh_job']['rows'] == manifest2['document']['fresh_job']['rows']
            report['checks'].append(('CXDLPv4 identity refusal' if args.missing_printer_identifier else 'CXDLPv4 converted-copy export')+' preserves original project pixels and parameters')
        verify_application_network()
        report['status'] = 'passed'
    except Exception as error:
        report.update(status='failed', error=str(error), application_exit_code=process.poll())
        # Preserve the isolated screen even if the application's accessibility
        # endpoint is blocked; diagnostics must not depend on that endpoint.
        try:
            subprocess.run(['import','-window','root',str(directory/'failure.png')],capture_output=True,timeout=5)
        except (OSError, subprocess.TimeoutExpired):
            pass
        visible = []
        def inspect(node, depth=0):
            if depth > 18 or len(visible) >= 1200: return
            if node.get_name() and node.get_state_set().contains(Atspi.StateType.SHOWING):
                visible.append([node.get_role_name(), node.get_name()])
            for index in range(node.get_child_count()): inspect(node.get_child_at_index(index), depth+1)
        try:
            desktop.clear_cache()
            app = find(desktop, lambda n: n.get_role_name() == 'application' and n.get_process_id() == application_pid)
            app.clear_cache()
            inspect(app)
            report['visible_controls'] = visible
            subprocess.run(['import','-window','root',str(directory/'failure.png')],capture_output=True)
        except Exception as inspection_error:
            report['inspection_error'] = str(inspection_error)
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
