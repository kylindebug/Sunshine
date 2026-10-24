#!/usr/bin/python3
"""UKUI protection owned by actual stream leases, never by application lifetime."""
import argparse
import fcntl
import hashlib
import json
import os
import pathlib
import re
import selectors
import signal
import socket
import struct
import subprocess
import time


class Guard:
    """Journal mutations, inhibit idle, and confirm locking before restoring access."""

    def __init__(self, backend, journal, privacy=False):
        self.backend = backend
        self.journal = pathlib.Path(journal)
        self.privacy = privacy
        self.state = None
        self.cookie = None

    def save(self):
        self.journal.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        temporary = self.journal.with_suffix('.tmp')
        with temporary.open('w', encoding='utf-8') as stream:
            json.dump(self.state, stream)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, self.journal)

    def recover(self):
        if self.journal.exists():
            self.state = json.loads(self.journal.read_text(encoding='utf-8'))
            self.stop()

    def start(self):
        if self.state is not None:
            raise RuntimeError('Previous protection recovery is still pending')
        self.cookie = self.backend.inhibit()
        try:
            self.state = {'inputs': {}, 'monitors': [], 'active': True}
            self.save()
            if self.privacy:
                self.state['monitors'] = self.backend.monitor_snapshot()
                self.save()
                self.enforce_inputs()
                for monitor in self.state['monitors']:
                    self.backend.power(monitor, 4)
                self.backend.verify_monitors(self.state['monitors'])
        except Exception:
            self.stop()
            raise

    def enforce_inputs(self):
        for key, device in self.backend.inputs().items():
            if device['remote'] or not device['enabled']:
                continue
            if key not in self.state['inputs']:
                self.state['inputs'][key] = device
                self.save()
            self.backend.enable_input(key, device, False)

    def tick(self):
        if self.state is None:
            return
        self.backend.keep_awake()
        if self.privacy:
            self.enforce_inputs()
            self.backend.verify_monitors(self.state['monitors'])

    def stop(self):
        if self.state is None:
            return
        # Never reveal the desktop just because the asynchronous Lock call succeeded.
        self.backend.lock()
        for key, device in self.state['inputs'].items():
            self.backend.enable_input(key, device, device['enabled'])
        for monitor in self.state['monitors']:
            self.backend.power(monitor, monitor['power'])
        if self.cookie is not None:
            self.backend.uninhibit(self.cookie)
            self.cookie = None
        self.journal.unlink(missing_ok=True)
        self.state = None


class Ukui:
    """Use the installed UKUI interfaces and explicit, verified DDC monitor identities."""
    input_path = '/org/ukui/KWin/InputDevice'
    input_iface = 'org.ukui.KWin.InputDevice'

    def __init__(self, monitors):
        import dbus
        self.dbus = dbus
        self.bus = dbus.SessionBus()
        self.session = dbus.Interface(self.bus.get_object('org.gnome.SessionManager',
                                     '/org/gnome/SessionManager'), 'org.gnome.SessionManager')
        self.saver = dbus.Interface(self.bus.get_object('org.ukui.ScreenSaver', '/'),
                                   'org.ukui.ScreenSaver')
        self.monitors = monitors
        self.last_activity = 0
        self.last_monitor_check = 0

    def inhibit(self):
        cookie = self.session.Inhibit('Sunshine', self.dbus.UInt32(0),
                                      'Active remote desktop stream', self.dbus.UInt32(12), timeout=3)
        if not self.session.IsInhibited(self.dbus.UInt32(8), timeout=3):
            self.uninhibit(cookie)
            raise RuntimeError('UKUI did not acknowledge idle inhibition')
        return cookie

    def uninhibit(self, cookie):
        self.session.Uninhibit(cookie, timeout=3)

    def keep_awake(self):
        if time.monotonic() - self.last_activity >= 15:
            if not self.session.IsInhibited(self.dbus.UInt32(8), timeout=3):
                raise RuntimeError('UKUI idle inhibitor was lost')
            self.session.simulateUserActivity(timeout=3)
            self.last_activity = time.monotonic()

    def lock(self):
        self.saver.Lock(timeout=3)
        for _ in range(30):
            if self.saver.GetLockState(timeout=1):
                return
            time.sleep(.1)
        raise RuntimeError('Lock not confirmed; retaining physical privacy protection')

    def props(self, path):
        return self.dbus.Interface(self.bus.get_object('org.ukui.KWin', path),
                                   'org.freedesktop.DBus.Properties')

    def inputs(self):
        names = self.props(self.input_path).Get('org.ukui.KWin.InputDeviceManager',
                                                'devicesSysNames', timeout=2)
        result = {}
        for name in names:
            name = str(name)
            if not re.fullmatch(r'event\d+', name):
                raise RuntimeError('Unexpected input device identifier')
            values = self.props(self.input_path + '/' + name).GetAll(self.input_iface, timeout=2)
            identity = str(pathlib.Path('/sys/class/input', name).resolve())
            remote = ('/devices/virtual/input/' in identity and
                      str(values['name']).startswith('libvirtualhid '))
            result[name] = {'name': str(values['name']), 'identity': identity,
                            'enabled': bool(values['enabled']), 'remote': remote}
        return result

    def enable_input(self, key, device, enabled):
        identity = pathlib.Path('/sys/class/input', key)
        if not identity.exists() or str(identity.resolve()) != device['identity']:
            return  # Removed/reused event numbers must never enable an unrelated device.
        props = self.props(self.input_path + '/' + key)
        if str(props.Get(self.input_iface, 'name', timeout=2)) != device['name']:
            raise RuntimeError('Input device identity changed')
        props.Set(self.input_iface, 'enabled', self.dbus.Boolean(enabled), timeout=2)
        if bool(props.Get(self.input_iface, 'enabled', timeout=2)) != enabled:
            raise RuntimeError('Physical input isolation was not acknowledged')

    @staticmethod
    def connected_monitors():
        return {p.parent.name: hashlib.sha256((p.parent / 'edid').read_bytes()).hexdigest()
                for p in pathlib.Path('/sys/class/drm').glob('card*-*/status')
                if p.read_text().strip() == 'connected'}

    @staticmethod
    def ddc(monitor, *args):
        bus = int(monitor['bus'])
        if bus < 0 or bus > 4095:
            raise RuntimeError('Invalid DDC bus')
        return subprocess.run(['ddcutil', '--bus', str(bus), '--terse', *args],
                              check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              text=True, timeout=3).stdout

    def read_power(self, monitor):
        value = self.ddc(monitor, 'getvcp', 'D6')
        match = re.search(r'VCP D6 SNC x([0-9a-fA-F]+)', value)
        if not match:
            raise RuntimeError('Monitor power state cannot be positively verified')
        return int(match.group(1), 16)

    def monitor_snapshot(self):
        self.verify_identity(self.monitors)
        return [dict(item, power=self.read_power(item)) for item in self.monitors]

    def verify_identity(self, monitors):
        expected = {item['connector']: item['edid_sha256'] for item in monitors}
        if not expected or self.connected_monitors() != expected:
            raise RuntimeError('Configure DDC buses and EDID hashes for every connected monitor')

    def power(self, monitor, value):
        # A changed monitor is not allowed to inherit a saved power command.
        if self.connected_monitors().get(monitor['connector']) != monitor['edid_sha256']:
            raise RuntimeError('Monitor identity changed; manual recovery required')
        self.ddc(monitor, 'setvcp', 'D6', str(value))

    def verify_monitors(self, monitors):
        self.verify_identity(monitors)
        if time.monotonic() - self.last_monitor_check >= 2:
            for monitor in monitors:
                if self.read_power(monitor) != 4:
                    raise RuntimeError('Physical display power-off could not be confirmed')
            self.last_monitor_check = time.monotonic()


def serve(guard, endpoint):
    """Count live authenticated socket leases and recover immediately on last EOF."""
    endpoint = pathlib.Path(endpoint)
    endpoint.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    # Ownership of this lock proves any leftover socket belongs to a dead instance.
    instance_lock = endpoint.with_suffix('.lock').open('w', encoding='utf-8')
    fcntl.flock(instance_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    guard.recover()
    endpoint.unlink(missing_ok=True)
    listener = socket.socket(socket.AF_UNIX)
    listener.bind(str(endpoint))
    listener.listen(8)
    listener.setblocking(False)
    poller = selectors.DefaultSelector()
    poller.register(listener, selectors.EVENT_READ)
    clients = set()
    running = True

    def stop(*_):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        while running:
            for key, _ in poller.select(.1):
                if key.fileobj is listener:
                    connection, _ = listener.accept()
                    _, uid, _ = struct.unpack('3i', connection.getsockopt(socket.SOL_SOCKET,
                                                                         socket.SO_PEERCRED, 12))
                    if uid != os.getuid():
                        connection.close()
                        continue
                    try:
                        if not clients:
                            guard.start()
                        connection.sendall(b'OK\n')
                        connection.setblocking(False)
                        clients.add(connection)
                        poller.register(connection, selectors.EVENT_READ)
                    except Exception as exc:
                        print('Session protection refused:', str(exc), flush=True)
                        connection.close()
                        if not clients:
                            guard.stop()
                else:
                    connection = key.fileobj
                    # Clients send no payload; EOF or unsolicited bytes end the lease.
                    poller.unregister(connection)
                    clients.remove(connection)
                    connection.close()
                    if not clients:
                        guard.stop()
            if clients:
                guard.tick()
    finally:
        # Closing leases first causes Sunshine to stop video if the protector fails.
        for connection in clients:
            connection.close()
        listener.close()
        poller.close()
        endpoint.unlink(missing_ok=True)
        guard.stop()
        instance_lock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', required=True)
    parser.add_argument('--journal', required=True)
    parser.add_argument('--privacy-config', help='JSON monitor identities; requires DDC access')
    args = parser.parse_args()
    os.umask(0o077)
    monitors = json.loads(pathlib.Path(args.privacy_config).read_text(encoding='utf-8')) if args.privacy_config else []
    serve(Guard(Ukui(monitors), args.journal, bool(args.privacy_config)), args.socket)


if __name__ == '__main__':
    main()
