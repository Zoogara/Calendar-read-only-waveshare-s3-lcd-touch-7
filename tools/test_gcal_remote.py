#!/usr/bin/env python3
"""Tests for the restart-first logic in gcal_remote.py, against a fake display on localhost.
   python3 tools/test_gcal_remote.py"""
import http.server
import os
import sys
import threading
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gcal_remote as g


class Fake:
    """A display: /version reports its state; POST /restart (if supported) resets uptime and the heap."""
    def __init__(self, largest=6 * 1024, restart_status=202, uptime=500):
        self.largest, self.restart_status, self.uptime = largest, restart_status, uptime
        self.restarts = 0
        self.after_restart_uptime = 40   # the fake "clock" jumps: it answers as already settled


def make_server(f):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def do_GET(self):
            body = (f"version t\nslot ota_0\nuptime_s {f.uptime}\nreset power-on\n"
                    f"internal_free 40000\ninternal_largest {f.largest}\n").encode()
            self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers()
            self.wfile.write(body)
        def do_POST(self):
            n = int(self.headers.get("Content-Length", 0)); self.rfile.read(n)
            self.send_response(f.restart_status); self.send_header("Content-Length", "0"); self.end_headers()
            if f.restart_status == 202:
                f.restarts += 1; f.largest = 20 * 1024; f.uptime = f.after_restart_uptime
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"127.0.0.1:{srv.server_address[1]}"


class T(unittest.TestCase):
    def run_case(self, f, min_kb=16, before=None):
        srv, dev = make_server(f)
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        logs = []
        b = before if before is not None else g.device_version(dev, "Basic x")
        now = [0.0]                         # a fake clock that only moves when the code sleeps
        def sleep(sec):
            now[0] += sec
            if f.restarts:
                f.uptime = int(now[0])      # the restarted display's uptime follows it
        r = g.ensure_fresh_heap(dev, "Basic x", b, min_kb, settle_s=30, timeout=100, log=logs.append,
                                sleep=sleep, clock=lambda: now[0])
        return r, logs, b

    def test_fragmented_display_is_restarted(self):
        f = Fake(largest=6 * 1024)
        r, logs, _ = self.run_case(f)
        self.assertEqual(f.restarts, 1)
        self.assertEqual(r["largest_kb"], 20)
        self.assertGreaterEqual(r["uptime_s"], 30)   # waited for its start-up fetches

    def test_waits_for_the_heap_to_recover_after_a_restart(self):
        # after a restart the largest block starts at 12 KB and reaches 20 KB only a minute in: it must not send at 30 s
        f = Fake(largest=6 * 1024)
        srv, dev = make_server(f)
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        now = [0.0]
        def sleep(sec):
            now[0] += sec
            if f.restarts:
                f.uptime = int(now[0])
                f.largest = (12 if f.uptime < 70 else 20) * 1024
        b = g.device_version(dev, "Basic x")
        logs = []
        r = g.ensure_fresh_heap(dev, "Basic x", b, 16, settle_s=30, timeout=200, log=logs.append,
                                sleep=sleep, clock=lambda: now[0])
        self.assertEqual(r["largest_kb"], 20)
        self.assertGreaterEqual(r["uptime_s"], 70)

    def test_gives_up_waiting_and_sends_anyway(self):
        f = Fake(largest=6 * 1024)
        srv, dev = make_server(f)
        self.addCleanup(srv.server_close)
        self.addCleanup(srv.shutdown)
        now = [0.0]
        def sleep(sec):
            now[0] += sec
            if f.restarts:
                f.uptime = int(now[0])
                f.largest = 12 * 1024              # never recovers
        b = g.device_version(dev, "Basic x")
        logs = []
        r = g.ensure_fresh_heap(dev, "Basic x", b, 16, settle_s=30, timeout=90, log=logs.append,
                                sleep=sleep, clock=lambda: now[0])
        self.assertEqual(r["largest_kb"], 12)
        self.assertTrue(any("sending anyway" in l for l in logs))

    def test_healthy_display_is_left_alone(self):
        f = Fake(largest=20 * 1024)
        r, logs, b = self.run_case(f)
        self.assertEqual(f.restarts, 0)
        self.assertIs(r, b)

    def test_unknown_memory_does_not_restart(self):
        f = Fake(largest=0)
        r, _, _ = self.run_case(f)
        self.assertEqual(f.restarts, 0)

    def test_no_version_does_not_restart(self):
        f = Fake()
        srv, dev = make_server(f); self.addCleanup(srv.shutdown)
        self.assertIsNone(g.ensure_fresh_heap(dev, "Basic x", None, 16))
        self.assertEqual(f.restarts, 0)

    def test_404_means_unsupported_and_carries_on(self):
        f = Fake(restart_status=404)
        r, logs, b = self.run_case(f)
        self.assertIs(r, b)
        self.assertTrue(any("no /restart" in l for l in logs))

    def test_wrong_password_carries_on(self):
        f = Fake(restart_status=401)
        r, logs, b = self.run_case(f)
        self.assertIs(r, b)
        self.assertTrue(any("HTTP 401" in l for l in logs))

    def test_lockout_carries_on(self):
        f = Fake(restart_status=429)
        r, logs, b = self.run_case(f)
        self.assertIs(r, b)
        self.assertTrue(any("HTTP 429" in l for l in logs))

    def test_unreachable_carries_on(self):
        b = {"largest_kb": 5, "free_kb": 30, "uptime_s": 1, "version": "t", "slot": "x", "reset": "x"}
        r = g.ensure_fresh_heap("127.0.0.1:9", "Basic x", b, 16, log=lambda s: None, sleep=lambda s: None)
        self.assertIs(r, b)


if __name__ == "__main__":
    unittest.main()
