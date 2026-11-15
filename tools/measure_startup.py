"""Measure the real app with five isolated 80-file boxes; preserve ShellNew state.

The first launch registers the fixture objects and is a warm-up. Measured restarts
reuse those same objects. This is an Explorer-already-running test, not a logon test.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
from pathlib import Path
import subprocess
import time
import winreg

parser = argparse.ArgumentParser()
parser.add_argument('--executable', required=True, type=Path)
parser.add_argument('--output', required=True, type=Path)
parser.add_argument('--runs', type=int, default=3)
args = parser.parse_args()
exe, output = args.executable.resolve(), args.output.resolve()
if output.exists(): raise SystemExit('Use a new output directory.')
output.mkdir(parents=True)
root = output / 'data'
for index in range(5):
    box = root if index == 0 else root / 'boxes' / f'benchmark-{index}'
    (box / 'items').mkdir(parents=True)
    (box / 'settings.ini').write_text('[DesktopBox]\nName=DesktopBox 性能验证 ' + str(index) + '\nBackgroundMode=0\nGlassTransparency=55\nGlassBlur=18\n', encoding='utf-16')
    for item in range(80): (box / 'items' / f'sample-{item}.txt').write_text('benchmark')
(root / 'startup-trace.flag').touch()

user32, kernel32, psapi = c.WinDLL('user32', use_last_error=True), c.WinDLL('kernel32', use_last_error=True), c.WinDLL('psapi', use_last_error=True)
user32.FindWindowW.argtypes = [w.LPCWSTR, w.LPCWSTR]; user32.FindWindowW.restype = w.HWND
kernel32.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]; kernel32.OpenProcess.restype = w.HANDLE
kernel32.CloseHandle.argtypes = [w.HANDLE]
class Memory(c.Structure):
    _fields_ = [('cb', w.DWORD), ('faults', w.DWORD)] + [(name, c.c_size_t) for name in ['peak_working_set', 'working_set', 'peak_paged', 'paged', 'peak_nonpaged', 'nonpaged', 'pagefile', 'peak_pagefile', 'private_bytes']]
class Io(c.Structure):
    _fields_ = [(name, c.c_ulonglong) for name in ['reads', 'writes', 'other', 'read_bytes', 'write_bytes', 'other_bytes']]
psapi.GetProcessMemoryInfo.argtypes = [w.HANDLE, c.POINTER(Memory), w.DWORD]
kernel32.GetProcessIoCounters.argtypes = [w.HANDLE, c.POINTER(Io)]
kernel32.GetProcessTimes.argtypes = [w.HANDLE] + [c.POINTER(c.c_ulonglong)] * 4
kernel32.GetProcessHandleCount.argtypes = [w.HANDLE, c.POINTER(w.DWORD)]
user32.GetGuiResources.argtypes = [w.HANDLE, w.DWORD]
if user32.FindWindowW('DesktopBox.Controller.02', '桌面收纳盒'): raise SystemExit('An existing DesktopBox instance must finish before this isolated measurement.')
startup = subprocess.STARTUPINFO(); startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW; startup.wShowWindow = 0
def run(*options):
    return subprocess.run([str(exe), '--data-dir', str(root), *options], startupinfo=startup, timeout=15, check=True)
def sample(handle):
    memory = Memory(); memory.cb = c.sizeof(memory)
    if not psapi.GetProcessMemoryInfo(handle, c.byref(memory), c.sizeof(memory)): raise c.WinError(c.get_last_error())
    io = Io(); kernel32.GetProcessIoCounters(handle, c.byref(io))
    times = [c.c_ulonglong() for _ in range(4)]
    kernel32.GetProcessTimes(handle, *(c.byref(value) for value in times))
    handles = w.DWORD(); kernel32.GetProcessHandleCount(handle, c.byref(handles))
    return {'private_bytes': memory.private_bytes, 'working_set': memory.working_set, 'peak_private_bytes': memory.peak_pagefile,
            'cpu_100ns': times[2].value + times[3].value, 'handles': handles.value, 'gdi_objects': user32.GetGuiResources(handle, 0),
            **{name: getattr(io, name) for name, _ in Io._fields_}}
def read_tree(path):
    try: key = winreg.OpenKey(winreg.HKEY_CURRENT_USER, path)
    except FileNotFoundError: return None
    with key:
        values = [winreg.EnumValue(key, index) for index in range(winreg.QueryInfoKey(key)[1])]
        children = {winreg.EnumKey(key, index): read_tree(path + '\\' + winreg.EnumKey(key, index)) for index in range(winreg.QueryInfoKey(key)[0])}
        return {'values': values, 'children': children}
def delete_tree(path):
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, path, 0, winreg.KEY_READ | winreg.KEY_WRITE) as key:
            children = [winreg.EnumKey(key, index) for index in range(winreg.QueryInfoKey(key)[0])]
        for child in children: delete_tree(path + '\\' + child)
        winreg.DeleteKey(winreg.HKEY_CURRENT_USER, path)
    except FileNotFoundError: pass
def write_tree(path, tree):
    if tree is None: return
    with winreg.CreateKey(winreg.HKEY_CURRENT_USER, path) as key:
        for name, value, kind in tree['values']: winreg.SetValueEx(key, name, 0, kind, value)
    for child, subtree in tree['children'].items(): write_tree(path + '\\' + child, subtree)
associations = [r'Software\Classes\.desktopbox', r'Software\Classes\DesktopBox.StorageBox.1']
original = {path: read_tree(path) for path in associations}
(output / 'registry-original.json').write_text(json.dumps(original, ensure_ascii=False, indent=2), encoding='utf-8')
results = []
process = None
try:
    for index in range(args.runs + 1):
        process = subprocess.Popen([str(exe), '--data-dir', str(root)], startupinfo=startup)
        time.sleep(2)
        if process.poll() is not None: raise RuntimeError('The app exited before readiness measurement.')
        run('--diagnostics')
        diagnostic = (root / 'hover-diagnostics.txt').read_text(encoding='utf-8')
        boxes = [line for line in diagnostic.splitlines() if line.startswith('box located=')]
        if len(boxes) != 5 or any(not line.startswith('box located=1') for line in boxes): raise RuntimeError('Not all five desktop objects are located: ' + diagnostic)
        if index:
            handle = kernel32.OpenProcess(0x0400 | 0x0010, False, process.pid)
            if not handle: raise c.WinError(c.get_last_error())
            try:
                before, started = sample(handle), time.perf_counter()
                time.sleep(10)
                after, ended = sample(handle), time.perf_counter()
            finally: kernel32.CloseHandle(handle)
            record = {'run': index, 'scenario': 'real native desktop, 5 boxes x 80 local files, existing Explorer',
                      'startup': {key: int(value) for line in diagnostic.splitlines() if '=' in line for key, value in [line.split('=', 1)] if key.endswith('_ms') and value.isdecimal()},
                      'idle_before': before, 'idle_after': after, 'idle_wall_ms': (ended-started)*1000,
                      'idle_cpu_ms': (after['cpu_100ns']-before['cpu_100ns'])/10000,
                      'idle_single_core_percent': (after['cpu_100ns']-before['cpu_100ns'])/100000/(ended-started),
                      'idle_read_operations': after['reads']-before['reads'], 'idle_write_operations': after['writes']-before['writes']}
            trace = root / 'startup-trace.json'
            if trace.exists(): record['startup_trace'] = json.loads(trace.read_text())
            (output / f'run-{index}.json').write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding='utf-8')
            results.append(record)
            print(json.dumps({'run':index, 'startup':record['startup'], 'private_mib':after['private_bytes']/1048576,
                              'idle_cpu_percent':record['idle_single_core_percent'], 'idle_reads':record['idle_read_operations'], 'idle_writes':record['idle_write_operations']}, ensure_ascii=False), flush=True)
        run('--shutdown'); process.wait(timeout=10); process = None
    (output / 'summary.json').write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding='utf-8')
finally:
    if process and process.poll() is None:
        run('--shutdown'); process.wait(timeout=10)
    run('--unregister-shell')
    for path, tree in original.items():
        current = read_tree(path)
        # Our unregistration already removed our own keys. Never overwrite a
        # different installation which appeared while the test was running.
        if current is None: write_tree(path, tree)
