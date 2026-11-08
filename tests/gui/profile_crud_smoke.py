#!/usr/bin/env python3
"""Exercise printer/material/process preset CRUD in GTK using an isolated seed."""
import argparse
import configparser
import json
import os
from pathlib import Path
import shutil
import re
import subprocess
import tempfile
import time
from printer_wizard_smoke import Atspi, find, named, wait_for, click


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--seed', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--inspect', action='store_true')
    parser.add_argument('--archive-format', choices=('klipper','klipper_rgb','SL1','SL1S','cxdlpv4','phz','cbddlp','photon','pwc','pm5s'), help='Select a display label and verify stable format ID is persisted')
    parser.add_argument('--anycubic-version', choices=('auto','1','515','516','517','518'), help='Edit and verify printer-persistent Anycubic version')
    parser.add_argument('--cbddlp-aa', type=int, help='Edit and verify CBDDLP/Photon v2 plane count in the saved printer preset')
    parser.add_argument('--printer-model', help='Edit the firmware model through the native settings field before saving')
    parser.add_argument('--phz-encryption-key', help='Edit and verify the PHZ seed in the saved printer preset')
    args = parser.parse_args()
    if args.phz_encryption_key and args.archive_format != 'phz':
        parser.error('--phz-encryption-key requires --archive-format phz')
    args.output.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix='profile-crud-', dir=args.output.resolve()))
    datadir = directory / 'presets'
    shutil.copytree(args.seed, datadir)
    configuration = datadir / 'PrusaSlicer.ini'
    def config():
        parsed = configparser.RawConfigParser()
        parsed.read_string('[DEFAULT]\n' + configuration.read_text())
        return parsed
    initial_printer = config().get('presets', 'printer')
    if not (datadir / 'printer' / (initial_printer + '.ini')).is_file():
        # CLI fixture seeds may select a temporary workflow.ini that was never
        # saved as a preset. Pin an existing SLA printer so preference checks
        # exercise that printer instead of an implicit startup fallback.
        candidates = sorted(p for p in (datadir / 'printer').glob('*.ini')
                            if 'printer_technology = SLA' in p.read_text())
        if not candidates:
            raise ValueError('Seed requires a saved SLA printer preset')
        initial_printer = candidates[0].stem
        configuration.write_text(re.sub(r'(?m)^printer = .*$', 'printer = ' + initial_printer,
                                         configuration.read_text()))
    def preference_key(name): return 'printer_' + name.encode('utf-8').hex()
    configuration.write_text(re.sub(r'(?ms)^\[sla_export_formats\]\n.*?(?=^\[|\Z)', '', configuration.read_text()))
    with configuration.open('a') as stream:
        stream.write('\n[sla_export_formats]\n' + preference_key(initial_printer) + ' = pm7\n')
    def preference(name): return config().get('sla_export_formats', preference_key(name), fallback='')
    report = {'datadir': str(datadir), 'checks': []}
    application_env = dict(os.environ, GDK_BACKEND='x11', NO_AT_BRIDGE='0')
    if os.environ.get('SLA_APP_NETWORK_AUDIT_LIBRARY'):
        application_env['LD_PRELOAD'] = str(Path(os.environ['SLA_APP_NETWORK_AUDIT_LIBRARY']).resolve())
    with (directory / 'application.log').open('w') as log:
        process = subprocess.Popen([str(args.binary.resolve()), '--datadir', str(datadir)],
            stdout=log, stderr=subprocess.STDOUT,
            env=application_env)
    app = None
    def visible(node):
        # Settings pages are shown lazily. Query their current GTK state rather
        # than the cached flags from the previously hidden page.
        node.clear_cache()
        return node.get_state_set().contains(Atspi.StateType.SHOWING)
    def dump(node, depth=0):
        if depth > 14: return []
        output = []
        output.append([node.get_role_name() if visible(node) else 'hidden '+node.get_role_name(), node.get_name(), node.get_description()])
        for index in range(node.get_child_count()):
            output.extend(dump(node.get_child_at_index(index), depth+1))
        return output
    try:
        desktop = Atspi.get_desktop(0)
        app = wait_for(lambda: find(desktop, lambda n:
            n.get_role_name() == 'application' and n.get_process_id() == process.pid), 'application')
        frame=wait_for(lambda:find(app,lambda n:n.get_role_name()=='frame'),'application frame')
        frame.get_component_iface().grab_focus()
        def settings_tab(label):
            item = wait_for(lambda: find(app, lambda n: n.get_role_name() == 'menu item' and
                n.get_name().startswith(label + ' Tab')), 'settings menu: ' + label)
            click(item)
        wait_for(lambda: named(app, 'SLA print settings:'), 'visible process preset in sidebar')
        wait_for(lambda: named(app, 'SLA material:'), 'visible material preset in sidebar')
        report['checks'].append('SLA process and material selectors are visible in the sidebar')
        if args.inspect:
            settings_tab('Printer Settings')
            time.sleep(1)
            report['tree'] = dump(app)
            report['status'] = 'inspected'
            return
        def dialog(title):
            return wait_for(lambda: find(app, lambda n: n.get_name() == title and visible(n)), title)
        def tool(label):
            return wait_for(lambda: find(app, lambda n: visible(n) and n.get_role_name() == 'button' and
                (n.get_name() == label or n.get_description() == label)), label)
        def enter_name(window, name):
            entry = find(window, lambda n: n.get_role_name() in ('text', 'entry') and
                n.get_state_set().contains(Atspi.StateType.EDITABLE))
            if entry is None: raise RuntimeError('No editable preset name')
            entry.get_editable_text_iface().set_text_contents(name)
        for tab, subdir, label in (
            ('Printer Settings', 'printer', 'GUI printer'),
            ('Material Settings', 'sla_material', 'GUI material'),
            ('Print Settings', 'sla_print', 'GUI process')):
            settings_tab(tab)
            if tab == 'Printer Settings':
                assert find(app, lambda n: visible(n) and n.get_name() == 'Print Host upload') is None
                assert find(app, lambda n: visible(n) and n.get_description() == 'Add physical printer') is None
                if args.archive_format:
                    field = wait_for(lambda: find(app, lambda n: visible(n) and n.get_role_name() in ('text','entry') and
                        'parameter name\t: sla_archive_format' in n.get_description()), 'archive format settings field')
                    format_label={'klipper':'Klipper MSLA (Mono) (.zip)','klipper_rgb':'Klipper MSLA (RGB) (.rgb.zip)','pwc':'Anycubic Custom (machine table) (.pwc)','pm5s':'Photon Mono M5s (.pm5s)','SL1':'SL1 archive (.sl1)', 'SL1S':'SL1S Speed archive (.sl1s)', 'cxdlpv4':'Creality CXDLPv4 (.cxdlpv4)', 'phz':'Phrozen PHZ v2 (grayscale) (.phz)','cbddlp':'Chitubox CBDDLP v2 (selectable AA) (.cbddlp)','photon':'Chitubox Photon v2 (selectable AA) (.photon)'}[args.archive_format]
                    assert field.get_component_iface().grab_focus()
                    assert field.get_editable_text_iface().set_text_contents(format_label)
                if args.anycubic_version is not None:
                    target=wait_for(lambda:find(app,lambda n:visible(n) and n.get_role_name() in ('text','entry') and 'parameter name\t: printer_model' in n.get_description()),'commit archive format')
                    assert target.get_component_iface().grab_focus()
                    version=wait_for(lambda:find(app,lambda n:visible(n) and n.get_role_name() in ('text','entry') and 'parameter name\t: sla_anycubic_version' in n.get_description() and n.get_state_set().contains(Atspi.StateType.SENSITIVE)),'editable Anycubic version')
                    assert version.get_component_iface().grab_focus()
                    version_label='Automatic (format default)' if args.anycubic_version=='auto' else args.anycubic_version
                    assert version.get_editable_text_iface().set_text_contents(version_label)
                    assert target.get_component_iface().grab_focus()
                if args.cbddlp_aa is not None:
                    target=wait_for(lambda:find(app,lambda n:visible(n) and n.get_role_name() in ('text','entry') and 'parameter name\t: printer_model' in n.get_description()),'commit archive format')
                    assert target.get_component_iface().grab_focus()
                    aa=wait_for(lambda:find(app,lambda n:visible(n) and n.get_role_name() in ('text','entry') and 'parameter name\t: sla_cbddlp_aa' in n.get_description() and n.get_state_set().contains(Atspi.StateType.SENSITIVE)),'editable AA planes')
                    assert aa.get_component_iface().grab_focus()
                    assert aa.get_editable_text_iface().set_text_contents(str(args.cbddlp_aa))
                    assert target.get_component_iface().grab_focus()
                if args.phz_encryption_key:
                    # Commit the editable combo before waiting for the seed field
                    # to be enabled by the selected format.
                    target = wait_for(lambda: find(app, lambda n: visible(n) and n.get_role_name() in ('text','entry')
                        and 'parameter name\t: printer_model' in n.get_description()), 'firmware model focus target')
                    assert target.get_component_iface().grab_focus()
                    key = wait_for(lambda: find(app, lambda n: visible(n) and n.get_role_name() in ('text','entry')
                        and 'parameter name\t: sla_phz_encryption_key' in n.get_description()
                        and n.get_state_set().contains(Atspi.StateType.SENSITIVE)
                        and n.get_state_set().contains(Atspi.StateType.EDITABLE)), 'editable PHZ seed')
                    assert key.get_component_iface().grab_focus()
                    assert key.get_editable_text_iface().set_text_contents(args.phz_encryption_key)
                if args.printer_model:
                    model = wait_for(lambda: find(app, lambda n: visible(n) and n.get_role_name() in ('text','entry')
                        and 'parameter name\t: printer_model' in n.get_description()
                        and n.get_state_set().contains(Atspi.StateType.EDITABLE)), 'editable firmware model')
                    assert model.get_component_iface().grab_focus()
                    assert model.get_editable_text_iface().set_text_contents(args.printer_model)
            click(tool('Save preset'))
            save = dialog('Save preset')
            enter_name(save, label)
            click(named(save, 'OK'))
            path = datadir / subdir / (label + '.ini')
            wait_for(path.is_file, 'saved preset')
            if tab == 'Printer Settings':
                assert 'printer_technology = SLA' in path.read_text()
                if args.archive_format:
                    assert ('sla_archive_format = '+args.archive_format+'\n') in path.read_text(), 'Format label was persisted instead of registry ID'
                if args.anycubic_version is not None:
                    assert ('sla_anycubic_version = '+args.anycubic_version+'\n') in path.read_text()
                    report['checks'].append('Anycubic stable version is editable and saved with the printer')
                if args.cbddlp_aa is not None:
                    assert ('sla_cbddlp_aa = '+str(args.cbddlp_aa)+'\n') in path.read_text()
                    report['checks'].append('native CBDDLP/Photon AA is editable and saved with the printer')
                if args.printer_model:
                    assert ('printer_model = ' + args.printer_model + '\n') in path.read_text()
                    report['checks'].append('native firmware model field is editable and saved with the printer')
                if args.phz_encryption_key:
                    assert ('sla_phz_encryption_key = ' + args.phz_encryption_key + '\n') in path.read_text()
                    report['checks'].append('native PHZ seed field is editable and saved with the printer')
                wait_for(lambda: preference(label) == (args.archive_format or 'pm7'), 'copied printer export preference')
                if args.anycubic_version is not None:
                    process.terminate(); process.wait(timeout=5)
                    with (directory / 'application.log').open('a') as log:
                        process = subprocess.Popen([str(args.binary.resolve()), '--datadir', str(datadir)],
                            stdout=log, stderr=subprocess.STDOUT,
                            env=application_env)
                    app=wait_for(lambda: find(Atspi.get_desktop(0), lambda n: n.get_role_name() == 'application' and n.get_process_id() == process.pid), 'restarted application')
                    settings_tab('Printer Settings')
                    version=wait_for(lambda:find(app,lambda n:visible(n) and n.get_role_name() in ('text','entry') and 'parameter name\t: sla_anycubic_version' in n.get_description()),'restored Anycubic version')
                    expected_label='Automatic (format default)' if args.anycubic_version=='auto' else args.anycubic_version
                    assert Atspi.Text.get_text(version,0,-1) == expected_label
                    report['checks'].append('restart restores Anycubic version in the native printer settings')

            click(tool('Rename preset'))
            rename = dialog('Rename preset')
            enter_name(rename, label + ' renamed')
            click(named(rename, 'OK'))
            renamed = path.with_name(label + ' renamed.ini')
            wait_for(lambda: renamed.is_file() and not path.exists(), 'renamed preset')
            if tab == 'Printer Settings':
                wait_for(lambda: preference(label + ' renamed') == (args.archive_format or 'pm7') and not preference(label), 'renamed export preference')
                if args.anycubic_version is not None:
                    assert ('sla_anycubic_version = '+args.anycubic_version+'\n') in renamed.read_text()
                    report['checks'].append('printer rename preserves the selected Anycubic version')
                # Simulate an on-disk collision created after startup, so it
                # is absent from the in-memory list. The original must survive.
                collision = datadir / subdir / 'occupied name.ini'
                collision.write_text('; unrelated existing file\n')
                click(tool('Rename preset'))
                rename = dialog('Rename preset')
                enter_name(rename, 'occupied name')
                click(named(rename, 'OK'))
                click(wait_for(lambda: find(app, lambda n: visible(n) and n.get_role_name() == 'button' and n.get_name() == 'OK'), 'rename error acknowledgement'))
                assert renamed.is_file() and collision.read_text() == '; unrelated existing file\n'
                collision.unlink()
                report['checks'].append('failed rename preserves the preset and existing destination')
            click(tool('Delete preset'))
            wait_for(lambda: named(app, 'Are you sure you want to delete the selected preset?'), 'delete confirmation')
            click(wait_for(lambda: find(app, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Yes' and visible(n)), 'confirm delete'))
            wait_for(lambda: not renamed.exists(), 'deleted preset')
            if tab == 'Printer Settings':
                wait_for(lambda: not preference(label + ' renamed'), 'deleted export preference')
                # Editing a draft and saving a copy must retain the source
                # printer's original durable preference.
                assert preference(initial_printer) == 'pm7'
                report['checks'].append('printer export preference follows copy/rename/delete independently')
            report['checks'].append(f'{tab}: save, rename and delete from GUI')
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
        if app is not None:
            try: report['tree'] = dump(app)
            except Exception: pass
        raise
    finally:
        if process.poll() is None and (args.inspect or report.get('status') == 'failed') and shutil.which('xprop'):
            try:
                import gi
                gi.require_version('Gdk','3.0'); gi.require_version('GdkX11','3.0')
                from gi.repository import Gdk, GdkX11
                clients=subprocess.check_output(['xprop','-root','_NET_CLIENT_LIST'],text=True)
                for window in re.findall(r'0x[0-9a-fA-F]+',clients):
                    pid=subprocess.check_output(['xprop','-id',window,'_NET_WM_PID'],text=True)
                    if re.search(r'=\s*'+str(process.pid)+r'\s*$',pid):
                        native=GdkX11.X11Window.foreign_new_for_display(Gdk.Display.get_default(),int(window,16))
                        image=Gdk.pixbuf_get_from_window(native,0,0,native.get_width(),native.get_height())
                        if image is not None:
                            screenshot=args.output.resolve()/'settings.png'
                            image.savev(str(screenshot),'png',[],[])
                            report['screenshot']=str(screenshot)
                        break
            except Exception as error:
                report['screenshot_error']=str(error)
        if process.poll() is None:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
