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
    parser.add_argument('--archive-format', choices=('SL1','cxdlpv4'), help='Select a display label and verify stable format ID is persisted')
    parser.add_argument('--printer-model', help='Edit the firmware model through the native settings field before saving')
    args = parser.parse_args()
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
    def preference_key(name): return 'printer_' + name.encode('utf-8').hex()
    configuration.write_text(re.sub(r'(?ms)^\[sla_export_formats\]\n.*?(?=^\[|\Z)', '', configuration.read_text()))
    with configuration.open('a') as stream:
        stream.write('\n[sla_export_formats]\n' + preference_key(initial_printer) + ' = pm7\n')
    def preference(name): return config().get('sla_export_formats', preference_key(name), fallback='')
    report = {'datadir': str(datadir), 'checks': []}
    with (directory / 'application.log').open('w') as log:
        process = subprocess.Popen([str(args.binary.resolve()), '--datadir', str(datadir)],
            stdout=log, stderr=subprocess.STDOUT,
            env=dict(os.environ, GDK_BACKEND='x11', NO_AT_BRIDGE='0'))
    app = None
    def visible(node):
        return node.get_state_set().contains(Atspi.StateType.SHOWING)
    def dump(node, depth=0):
        if depth > 14: return []
        output = []
        if visible(node):
            output.append([node.get_role_name(), node.get_name(), node.get_description()])
        for index in range(node.get_child_count()):
            output.extend(dump(node.get_child_at_index(index), depth+1))
        return output
    try:
        desktop = Atspi.get_desktop(0)
        app = wait_for(lambda: find(desktop, lambda n:
            n.get_role_name() == 'application' and n.get_process_id() == process.pid), 'application')
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
                    label='SL1 archive (.sl1)' if args.archive_format=='SL1' else 'Creality CXDLPv4 (.cxdlpv4)'
                    assert field.get_component_iface().grab_focus()
                    assert field.get_editable_text_iface().set_text_contents(label)
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
                if args.printer_model:
                    assert ('printer_model = ' + args.printer_model + '\n') in path.read_text()
                    report['checks'].append('native firmware model field is editable and saved with the printer')
                wait_for(lambda: preference(label) == (args.archive_format or 'pm7'), 'copied printer export preference')
            click(tool('Rename preset'))
            rename = dialog('Rename preset')
            enter_name(rename, label + ' renamed')
            click(named(rename, 'OK'))
            renamed = path.with_name(label + ' renamed.ini')
            wait_for(lambda: renamed.is_file() and not path.exists(), 'renamed preset')
            if tab == 'Printer Settings':
                wait_for(lambda: preference(label + ' renamed') == (args.archive_format or 'pm7') and not preference(label), 'renamed export preference')
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
                assert preference(initial_printer) == (args.archive_format or 'pm7')
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
        if process.poll() is None:
            process.terminate()
            try: process.wait(timeout=5)
            except subprocess.TimeoutExpired: process.kill(); process.wait()
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
