#!/usr/bin/env python3
"""Opt-in Linux capture probe. Run in the project's dev shell after building
share-audio-capture-probe. All daemons, sockets, state and logs are confined to
BUILD/gh26-private. Only PIDs started here are terminated. No display or bus.
Usage: python3 tests/run-share-audio-private-graph.py BUILD [PROBE_BINARY]
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import time

build = Path(sys.argv[1]).resolve()
binary = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else build / 'share-audio-capture-probe'
root = build / ('gh26-private' if len(sys.argv) == 2 else 'gh26-private-' + binary.name)
root.mkdir(exist_ok=False)
rt = root / 'rt'
rt.mkdir(mode=0o700)
(rt / 'pulse').mkdir()
config = root / 'config'
(config / 'pipewire/pipewire.conf.d').mkdir(parents=True)
(config / 'wireplumber/wireplumber.conf.d').mkdir(parents=True)
(config / 'pipewire/pipewire.conf.d/10-private.conf').write_text('''
context.objects = [ { factory = adapter args = {
 factory.name = support.null-audio-sink node.name = gh26-out
 media.class = Audio/Sink audio.position = [ FL FR ]
} } ]
''')
(config / 'wireplumber/wireplumber.conf.d/10-private.conf').write_text('''
wireplumber.profiles = { gh26 = {
 inherits = [ main, mixin.systemwide-session, mixin.stateless ]
 monitor.alsa = disabled
 monitor.alsa-midi = disabled
 monitor.bluez = disabled
 monitor.bluez-midi = disabled
 monitor.v4l2 = disabled
 monitor.libcamera = disabled
} }
''')
env = dict(os.environ, XDG_RUNTIME_DIR=str(rt), PIPEWIRE_RUNTIME_DIR=str(rt),
           XDG_CONFIG_HOME=str(config), XDG_STATE_HOME=str(root / 'state'),
           PULSE_RUNTIME_PATH=str(rt / 'pulse'), PULSE_SERVER=f'unix:{rt}/pulse/native',
           PULSE_SINK='gh26-out', DBUS_SESSION_BUS_ADDRESS='unix:path=/nonexistent-gh26-bus',
           DBUS_SYSTEM_BUS_ADDRESS='unix:path=/nonexistent-gh26-system-bus',
           LIGHTNING_PRIVATE_AUDIO_PROBE='1', GST_REGISTRY=str(root / 'registry.bin'))
for key in ('DISPLAY', 'WAYLAND_DISPLAY', 'PIPEWIRE_REMOTE', 'PULSE_SOURCE'):
    env.pop(key, None)
children = []
logs = []
def start(name, command):
    log = open(root / f'{name}.log', 'w')
    logs.append(log)
    child = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    children.append(child)
    return child

def probe(name, mode, overrides=None, expected=0):
    with open(root / f'{name}.log', 'w') as log:
        result = subprocess.run([str(binary), mode],
                                env=env | (overrides or {}), stdout=log,
                                stderr=subprocess.STDOUT, timeout=30)
    text = (root / f'{name}.log').read_text()
    print(name, 'exit', result.returncode, flush=True)
    for line in text.splitlines():
        if line.startswith(('SYSTEM ', 'MONITOR CONTROL ', 'capture kind=', 'share audio unavailable:')):
            print(line, flush=True)
    if result.returncode != expected:
        raise RuntimeError(f'{name}: expected {expected}, got {result.returncode}; see {root}')

try:
    start('pipewire', ['pipewire'])
    time.sleep(1)
    start('wireplumber', ['wireplumber', '-p', 'gh26'])
    start('pipewire-pulse', ['pipewire-pulse'])
    time.sleep(2)
    start('application', ['gst-launch-1.0', '-q', 'audiotestsrc', 'is-live=true',
                         'freq=690', 'volume=0.3', '!', 'audioconvert', '!',
                         'pulsesink', 'client-name=ToneApp'])
    time.sleep(2)
    # Record the graph to demonstrate this run had only our null output.
    dump = subprocess.check_output(['pw-dump'], env=env)
    (root / 'graph.json').write_bytes(dump)
    objects = json.loads(dump)
    sinks = [o['info']['props'].get('node.name') for o in objects
             if o.get('info', {}).get('props', {}).get('media.class') == 'Audio/Sink']
    assert sinks == ['gh26-out'], sinks
    print('private sinks:', sinks, flush=True)
    with open(root / 'daemon-live.log', 'w') as log:
        result = subprocess.run([str(build / 'share-audio-daemon-test'),
                                 'aPrivateConnectionEnumeratesAndCapturesOnARealGraph'],
                                env=env | {'LIGHTNING_TEST_PIPEWIRE_REMOTE': str(rt / 'pipewire-0')},
                                stdout=log, stderr=subprocess.STDOUT, timeout=15)
    assert result.returncode == 0, (root / 'daemon-live.log').read_text()
    print('private connection capture: pass', flush=True)
    probe('monitor-control', 'monitor-control')
    probe('system', 'system')
    # The shipped AppImage's missing mixer, without host plugins rescuing it.
    plugins = root / 'no-mixer'
    plugins.mkdir()
    for directory in env.get('GST_PLUGIN_SYSTEM_PATH_1_0', '').split(':'):
        if directory:
            for plugin in Path(directory).glob('libgst*.so'):
                target = plugins / plugin.name
                if plugin.name != 'libgstaudiomixer.so' and not target.exists():
                    target.symlink_to(plugin)
    assert list(plugins.iterdir()), 'dev-shell plugin path is required'
    no_mixer = {'GST_PLUGIN_SYSTEM_PATH_1_0': str(plugins), 'GST_PLUGIN_PATH_1_0': '',
                'GST_PLUGIN_SYSTEM_PATH': str(plugins), 'GST_PLUGIN_PATH': '',
                'GST_REGISTRY': str(root / 'no-mixer-registry.bin')}
    probe('no-mixer', 'system', no_mixer, expected=3)
    probe('no-pipewire', 'system', {'PIPEWIRE_REMOTE': str(rt / 'absent')}, expected=3)
finally:
    for child in reversed(children):
        if child.poll() is None:
            child.terminate()
    for child in reversed(children):
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
    for log in logs:
        log.close()
    print('terminated own children:', [child.pid for child in children], flush=True)
