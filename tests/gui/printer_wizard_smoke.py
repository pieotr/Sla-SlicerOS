#!/usr/bin/env python3
"""Local GTK/AT-SPI integration test. Uses a new datadir, never user presets.
Run with the system Python (PyGObject/AT-SPI required) on a graphical session.
No downloads, network calls or coordinate-dependent input.
"""
from appname import T
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time
import gi

gi.require_version('Atspi', '2.0')
from gi.repository import Atspi, GLib
import xtest_shim  # noqa: F401  routes XTEST through KWin EIS inside run_private_gui.sh


def find(node, predicate, depth=0):
    if depth > 18:
        return None
    try:
        if predicate(node):
            return node
        if depth == 0:   # owner-drawn drop-downs expose no item children: synthesize them for item lookups
            item = xtest_shim.find_owner_drawn_item(node, predicate)
            if item:
                return item
        for i in range(node.get_child_count()):
            child = node.get_child_at_index(i)
            if child:
                found = find(child, predicate, depth + 1)
                if found:
                    return found
    except GLib.Error:
        # GTK may retire accessibility objects while constructing a modal dialog.
        return None
    return None


AUTO_EXPORT_FORMAT = True   # tests that drive the 'Export format' dialog themselves set this False


def wait_for(getter, description, timeout=20):
    end = time.monotonic() + timeout
    export_chooser = xtest_shim.expects_export_chooser(getter)
    while time.monotonic() < end:
        result = getter()
        if result:
            return result
        if export_chooser and AUTO_EXPORT_FORMAT:
            # the app's own 'Export format' dialog precedes these file choosers (the GTK filter combo is not used any more)
            dialog = xtest_shim.pending_export_format_dialog()
            if dialog:
                xtest_shim.accept_export_format(dialog)
        time.sleep(.2)
    raise RuntimeError('Timed out: ' + description)


def fit_frame(frame, width, height, description, accept=None, timeout=45):
    """Move/resize the main frame to (0, 0, width, height) of the private display and wait until it really is there.

    Root cause of the one-off "Timed out: QA frame fitted to private display" (wave 35 analysis of the failed bundle
    run): the request used to be sent ONCE right after the frame appeared, then only polled with a 20 s limit. On a slow
    first start of a bundle the frame is still being mapped/placed by the window manager (and the model-load question
    "The object is too small" opens a modal dialog around the same moment), so the single request could be overridden
    or ignored and nothing ever asked again. Here the request is repeated every ~2 s while polling (an idempotent
    call), the step gets a longer limit (45 s; every other wait keeps its own), and a real failure is NOT masked: the
    timeout message carries the last bounds, whether the request was accepted and the visible modal dialogs.
    `accept(bounds)` overrides the exact-match test (some tests only need the frame to lie within the display)."""
    from gi.repository import Atspi as _A
    comp = frame.get_component_iface()
    if accept is None:
        accept = lambda b: b.x == 0 and b.y == 0 and b.width == width and b.height == height
    state = {'accepted': None, 'last': None, 'asked': 0.0}
    def attempt():
        now = time.monotonic()
        if now - state['asked'] >= 2.0:
            state['asked'] = now
            state['accepted'] = comp.set_extents(0, 0, width, height, _A.CoordType.SCREEN)
        b = comp.get_extents(_A.CoordType.SCREEN)
        state['last'] = (b.x, b.y, b.width, b.height)
        return b if accept(b) else None
    try:
        return wait_for(attempt, description, timeout)
    except RuntimeError as error:
        raise RuntimeError('%s (last bounds %r, last set_extents accepted=%r, requested %dx%d)' %
                           (error, state['last'], state['accepted'], width, height)) from None


def sliced_editor(app):
    """The Sliced editor is a permanent workspace view since wave 5 (no 'Layer editor' dialog any
    more): it is present exactly when its 'Back to 3D' button is showing. Returns the search root
    (the application) like archive_editor_smoke does, or None. Replaces the stale
    find(... name startswith 'Layer editor' ...) lookups of the legacy scripts."""
    back = find(app, lambda n: n.get_role_name() == 'button' and n.get_name() == 'Back to 3D' and
                n.get_state_set().contains(Atspi.StateType.SHOWING))
    return app if back else None


def layer_dirty(app):
    """True when the Sliced editor reports unsaved layer edits or project settings.

    Since the editor became a permanent workspace view its title no longer ends with '*' (the
    legacy scripts' dirty marker); the same state is the 'Layer document: modified' status label
    (or 'Generated layers: not saved ...' for generated jobs)."""
    return find(app, lambda n: n.get_role_name() == 'label' and n.get_state_set().contains(Atspi.StateType.SHOWING) and
                (n.get_name() or '').startswith(('Layer document: modified', 'Generated layers: not saved'))) is not None


def named(node, name):
    return find(node, lambda n: n.get_name() == name and
                not n.get_state_set().contains(Atspi.StateType.DEFUNCT))


def click(node):
    assert node is not None, 'Missing control'
    assert node.get_action_iface().do_action(0), 'Control rejected action'
    time.sleep(.3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('binary', type=Path)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--profile', default='Anet N7')
    parser.add_argument('--dimensions', type=float, nargs=2, default=(192.,120.))
    parser.add_argument('--model')
    parser.add_argument('--format')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    datadir = Path(tempfile.mkdtemp(prefix='printer-wizard-', dir=args.output.resolve()))
    report = {'datadir': str(datadir), 'checks': []}
    desktop = Atspi.get_desktop(0)

    def launch():
        log = (datadir / 'application.log').open('a')
        proc = subprocess.Popen([str(args.binary.resolve()), '--datadir', str(datadir)],
                                stdout=log, stderr=subprocess.STDOUT,
                                env=dict(os.environ, GDK_BACKEND='x11', NO_AT_BRIDGE='0',
                                         # the audit check below needs the library inside the *app*, not in
                                         # this Python process, so the harness hands it over by name
                                         **({'LD_PRELOAD': os.environ['SLA_APP_NETWORK_AUDIT_LIBRARY']}
                                            if os.environ.get('SLA_APP_NETWORK_AUDIT_LIBRARY') else {})))
        log.close()
        def application():
            if proc.poll() is not None:
                raise RuntimeError(f'Application exited during launch: {proc.returncode}; log: {datadir / "application.log"}')
            return find(desktop, lambda n: n.get_process_id() == proc.pid and n.get_role_name() == 'application')
        app = wait_for(application, 'application')
        return proc, app

    def wizard(app, first=False):
        if not first:
            click(wait_for(lambda: find(app, lambda n: n.get_role_name() == 'menu item'
                and n.get_name().startswith('Configuration Wizard')), 'wizard menu'))
        return wait_for(lambda: find(app, lambda n:
            n.get_name() == T('PrusaSlicer - Configuration Wizard') and
            n.get_state_set().contains(Atspi.StateType.SHOWING)), 'visible wizard dialog')

    def select(dialog):
        # Wave 34/38 flow: step 1 asks what to set up ("A printer" is the default), then where the printer comes from;
        # the library path marks a profile (nothing is saved until Finish).
        click(wait_for(lambda: find(dialog, lambda n: n.get_role_name() == 'button' and n.get_name() in ('Next >', '&Next >') and
            n.get_state_set().contains(Atspi.StateType.SHOWING)), 'Next button (what to set up)'))
        click(wait_for(lambda: find(dialog, lambda n: n.get_role_name() == 'radio button' and
            n.get_name() == 'A profile from the library' and n.get_state_set().contains(Atspi.StateType.SHOWING)), 'library path card'))
        click(wait_for(lambda: find(dialog, lambda n: n.get_role_name() == 'button' and n.get_name() in ('Next >', '&Next >') and
            n.get_state_set().contains(Atspi.StateType.SHOWING)), 'Next button'))
        cell = wait_for(lambda: find(dialog, lambda n: n.get_name() == args.profile and
            n.get_role_name() == 'table cell' and n.get_state_set().contains(Atspi.StateType.SHOWING)), 'visible library profile')
        table = cell.get_parent()
        # AT-SPI versions differ in TableCell introspection. Ask the table to
        # map its child index to a row instead of assuming a header offset.
        interface = table.get_table_iface()
        row = interface.get_row_at_index(cell.get_index_in_parent())
        report.setdefault('selection_traces', []).append({
            'cell_role': cell.get_role_name(), 'child_index': cell.get_index_in_parent(),
            'parent_role': table.get_role_name(), 'rows': interface.get_n_rows(), 'row': row,
            'selected': cell.get_state_set().contains(Atspi.StateType.SELECTED)})
        wait_for(lambda: cell.get_state_set().contains(Atspi.StateType.SELECTED) or
                 row >= 0 and interface.add_row_selection(row), 'selected profile row')
        wait_for(lambda: named(dialog, 'Finish').get_state_set().contains(Atspi.StateType.SENSITIVE), 'enabled Finish button')

    proc = None
    try:
        proc, app = launch()
        time.sleep(3)
        dialog = named(app, T('PrusaSlicer - Configuration Wizard')) or wizard(app)
        select(dialog)
        original = datadir / ('printer/' + args.profile + '.ini')
        time.sleep(1)
        assert not original.exists(), 'library selection saved a profile before Finish'
        report['checks'].append('library selection does not install anything before Finish')
        click(named(dialog, 'Finish'))
        wait_for(original.exists, 'saved printer')
        original_bytes = original.read_bytes()
        assert b'printer_technology = SLA' in original_bytes
        fields = dict(line.split(' = ',1) for line in original_bytes.decode().splitlines() if ' = ' in line)
        assert float(fields['display_width']) == args.dimensions[0]
        assert float(fields['display_height']) == args.dimensions[1]
        if args.model: assert fields['printer_model'] == args.model
        if args.format: assert fields['sla_archive_format'] == args.format
        report['checks'].append('fresh datadir library selection and save')
        dialog = wizard(app)
        select(dialog)
        click(named(dialog, 'Finish'))
        wait_for((datadir / ('printer/' + args.profile + ' (2).ini')).exists, 'duplicate printer')
        assert original.read_bytes() == original_bytes
        report['checks'].append('Finish creates unique copy without overwriting')
        dialog = wizard(app)
        select(dialog)
        click(named(dialog, 'Cancel'))
        assert len(list((datadir / 'printer').glob('*.ini'))) == 2
        report['checks'].append('Cancel preserves saved profiles')
        # AppConfig autosaves while idle. Inspect what the next process will load.
        config = datadir / 'PrusaSlicer.ini'
        wait_for(lambda: config.exists() and ('printer = ' + args.profile + ' (2)\n') in config.read_text(), 'persisted selection')
        proc.terminate(); proc.wait(timeout=5); proc = None
        proc, app = launch()
        time.sleep(2)
        assert named(app, T('PrusaSlicer - Configuration Wizard')) is None
        assert ('printer = ' + args.profile + ' (2)\n') in config.read_text()
        report['checks'].append('restart preserves selection and skips first-launch wizard')
        audit = os.environ.get('SLA_NETWORK_AUDIT_FILE')
        if audit:
            assert 'offline_network_audit.so' in Path(f'/proc/{proc.pid}/maps').read_text()
            assert not (Path(audit).read_text() if Path(audit).exists() else '')
            report['checks'].append('network audit loaded; no network attempts')
        report['status'] = 'passed'
    except Exception as error:
        report['status'] = 'failed'
        report['error'] = str(error)
        if proc is not None:
            report['application_exit_code'] = proc.poll()
        raise
    finally:
        if proc is not None and proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill(); proc.wait()
        (args.output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
