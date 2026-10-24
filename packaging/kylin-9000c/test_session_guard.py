"""Run with python3 -m unittest discover -s packaging/kylin-9000c -p test_session_guard.py."""
import multiprocessing
import pathlib
import socket
import tempfile
import time
import unittest

from session_guard import Guard, serve


class Backend:
    def __init__(self):
        self.events = []
        self.fail_lock = False
        self.fail_power = False
        self.devices = {
            'event0': {'remote': False, 'enabled': True, 'identity': 'physical'},
            'event1': {'remote': True, 'enabled': True, 'identity': 'virtual'},
            'event2': {'remote': False, 'enabled': False, 'identity': 'disabled'},
        }

    def inhibit(self):
        self.events.append('inhibit')
        return 17

    def uninhibit(self, cookie):
        assert cookie == 17
        self.events.append('uninhibit')

    def lock(self):
        self.events.append('lock')
        if self.fail_lock:
            raise RuntimeError('locked state unknown')

    def inputs(self):
        return self.devices

    def enable_input(self, key, device, enabled):
        self.events.append((key, enabled))

    def monitor_snapshot(self):
        return [{'bus': 2, 'power': 1}]

    def power(self, monitor, value):
        self.events.append(('power', value))
        if value == 4 and self.fail_power:
            raise RuntimeError('monitor denied')

    def verify_monitors(self, monitors):
        self.events.append('verified')

    def keep_awake(self):
        self.events.append('awake')


class GuardTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.backend = Backend()
        self.journal = pathlib.Path(self.temp.name, 'journal.json')
        self.guard = Guard(self.backend, self.journal, True)

    def test_only_physical_enabled_devices_are_disabled(self):
        self.guard.start()
        self.assertIn(('event0', False), self.backend.events)
        self.assertNotIn(('event1', False), self.backend.events)
        self.assertNotIn(('event2', False), self.backend.events)
        self.assertTrue(self.journal.exists())

    def test_lock_precedes_every_restore_and_inhibitor_release(self):
        self.guard.start()
        self.backend.events.clear()
        self.guard.stop()
        self.assertEqual(self.backend.events, ['lock', ('event0', True), ('power', 1), 'uninhibit'])
        self.assertFalse(self.journal.exists())

    def test_failed_lock_keeps_screen_input_and_journal_protected(self):
        self.guard.start()
        self.backend.events.clear()
        self.backend.fail_lock = True
        with self.assertRaises(RuntimeError):
            self.guard.stop()
        self.assertEqual(self.backend.events, ['lock'])
        self.assertTrue(self.journal.exists())
        self.backend.fail_lock = False
        self.guard.stop()

    def test_setup_failure_locks_then_rolls_back(self):
        self.backend.fail_power = True
        with self.assertRaises(RuntimeError):
            self.guard.start()
        events = self.backend.events
        self.assertLess(events.index('lock'), events.index(('event0', True)))
        self.assertIsNone(self.guard.state)

    def test_recovery_after_process_death_locks_before_restoring(self):
        self.guard.start()
        replacement = Guard(self.backend, self.journal, True)
        self.backend.events.clear()
        replacement.recover()
        self.assertEqual(self.backend.events[0], 'lock')
        self.assertIn(('power', 1), self.backend.events)
        self.assertFalse(self.journal.exists())

    def test_hotplug_is_added_to_journal_before_disable(self):
        self.guard.start()
        self.backend.devices['event3'] = {'remote': False, 'enabled': True, 'identity': 'new'}
        self.guard.tick()
        self.assertIn('event3', self.journal.read_text())
        self.assertIn(('event3', False), self.backend.events)

    def test_session_only_mode_never_changes_input_or_display(self):
        guard = Guard(self.backend, self.journal)
        guard.start()
        guard.tick()
        guard.stop()
        self.assertEqual(self.backend.events, ['inhibit', 'awake', 'lock', 'uninhibit'])

    def test_pending_recovery_cannot_be_overwritten(self):
        self.guard.start()
        before = self.journal.read_text()
        with self.assertRaises(RuntimeError):
            self.guard.start()
        self.assertEqual(self.journal.read_text(), before)


def run_server(endpoint, events):
    class LeaseGuard:
        def recover(self):
            pass

        def start(self):
            events.put('start')

        def stop(self):
            events.put('stop')

        def tick(self):
            pass
    serve(LeaseGuard(), endpoint)


class LeaseTest(unittest.TestCase):
    def test_two_clients_lock_only_on_last_disconnect_and_reconnect(self):
        with tempfile.TemporaryDirectory() as directory:
            endpoint = str(pathlib.Path(directory, 'guard.sock'))
            events = multiprocessing.Queue()
            process = multiprocessing.Process(target=run_server, args=(endpoint, events))
            process.start()
            clients = []
            try:
                for _ in range(100):
                    if pathlib.Path(endpoint).exists():
                        break
                    time.sleep(.01)
                for _ in range(2):
                    client = socket.socket(socket.AF_UNIX)
                    client.settimeout(2)
                    client.connect(endpoint)
                    self.assertEqual(client.recv(3), b'OK\n')
                    clients.append(client)
                self.assertEqual(events.get(timeout=2), 'start')
                clients.pop().close()
                time.sleep(.15)
                self.assertTrue(events.empty())
                clients.pop().close()
                self.assertEqual(events.get(timeout=2), 'stop')
                client = socket.socket(socket.AF_UNIX)
                client.settimeout(2)
                client.connect(endpoint)
                clients.append(client)
                self.assertEqual(client.recv(3), b'OK\n')
                self.assertEqual(events.get(timeout=2), 'start')
            finally:
                for client in clients:
                    client.close()
                process.terminate()
                process.join(3)
                if process.is_alive():
                    process.kill()


if __name__ == '__main__':
    unittest.main()
