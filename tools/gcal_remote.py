#!/usr/bin/env python3
"""Wireless log viewer and firmware pusher for the gcal display (stdlib only).

  gcal_remote.py logs [--port 9999] [--file device.log]
      Listens for the UDP log the device sends to its "Log destination"
      (config page -> Developer). Prints each line, colour codes stripped.

  gcal_remote.py ota --device 192.168.1.50 [--build build-dev/gcal_display.bin]
      Serves the built .bin from this machine and tells the device to fetch
      it (POST /ota on its config web server, same password as the settings
      page - set GCAL_PASS). The device restarts into the new image; one that
      doesn't survive its first minute rolls back by itself.
      The download is plain http, which only a DEVELOPMENT build accepts:
      build with tools/dev_build.sh (-> build-dev/), and send the display a dev
      build every time - a default build is https-only and would end this.

      Before sending it reads GET /version (which image the display runs), and afterwards waits
      for the display to come back and checks it is running the build it was sent - a new image
      that dies early is rolled back by the bootloader with no other sign. Exit status 2 if the
      display came back on a different image (or didn't answer).

Serial logging and `idf.py flash` over USB are unaffected and still work.
"""
import argparse, base64, functools, http.server, json, os, re, socket, socketserver
import sys, threading, time, urllib.error, urllib.parse, urllib.request

ANSI = re.compile(r"\x1b\[[0-9;]*m")


def cmd_logs(a):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(("0.0.0.0", a.port))
    out = open(a.file, "a") if a.file else None
    print(f"listening for device logs on UDP :{a.port}", file=sys.stderr)
    # Port 9999 is also used by some smart-home gear, so other hosts' packets
    # can land here. Lock onto the first sender whose packet looks like an
    # ESP-IDF log line and ignore everyone else (or force it with --from).
    device = a.sender
    starts = (b"I (", b"W (", b"E (", b"D (", b"V (", b"\x1b[0;3", b"\x1b[1;3")
    while True:
        data, (ip, _) = sock.recvfrom(4096)
        if device is None:
            if not data.startswith(starts):
                continue
            device = ip
            print(f"locked onto {ip}", file=sys.stderr)
        elif ip != device:
            continue
        text = ANSI.sub("", data.decode(errors="replace"))
        sys.stdout.write(text)
        sys.stdout.flush()
        if out:
            out.write(text)
            out.flush()


def local_ip(device):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.connect((device, 80))
    ip = s.getsockname()[0]
    s.close()
    return ip


def device_version(device, auth, timeout=6):
    """GET /version -> {"version", "slot", "uptime_s"}, or None if the display doesn't answer or has
    no /version (a firmware older than the endpoint)."""
    req = urllib.request.Request(f"http://{device}/version")
    req.add_header("Authorization", auth)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as r:
            fields = dict(line.split(" ", 1) for line in r.read().decode().splitlines() if " " in line)
        return {"version": fields.get("version", "?"), "slot": fields.get("slot", "?"),
                "uptime_s": int(fields.get("uptime_s", "0")), "reset": fields.get("reset", "?"),
                "free_kb": int(fields.get("internal_free", "0")) // 1024,
                "largest_kb": int(fields.get("internal_largest", "0")) // 1024}
    except (urllib.error.URLError, OSError, ValueError):
        return None


def build_version(build):
    """The version string baked into this build (what the display will report), from the build
    folder's project_description.json; None if it can't be read."""
    try:
        with open(os.path.join(os.path.dirname(build), "project_description.json")) as f:
            return json.load(f).get("project_version")
    except (OSError, ValueError):
        return None


def cmd_ota(a):
    build = os.path.abspath(a.build)
    if not os.path.isfile(build):
        sys.exit(f"{build} not found - run idf.py build first")
    password = os.environ.get("GCAL_PASS")
    if not password:
        sys.exit("set GCAL_PASS to the config web page password")
    ip = local_ip(a.device)

    done = threading.Event()
    size = os.path.getsize(build)

    class Handler(http.server.SimpleHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def copyfile(self, source, outputfile):
            sent = 0
            try:
                while True:
                    chunk = source.read(16384)
                    if not chunk:
                        break
                    outputfile.write(chunk)
                    sent += len(chunk)
                    print(f"\r  sent {sent * 100 // size}%", end="", flush=True)
            except (BrokenPipeError, ConnectionResetError):
                print("\n  connection dropped - the device gave up or restarted")
                return
            if sent >= size:
                print("\n  download complete")
                done.set()

    handler = functools.partial(Handler, directory=os.path.dirname(build))
    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("0.0.0.0", a.serve_port), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    fw_url = f"http://{ip}:{a.serve_port}/{os.path.basename(build)}"
    auth = "Basic " + base64.b64encode(f"x:{password}".encode()).decode()
    req = urllib.request.Request(
        f"http://{a.device}/ota?url={urllib.parse.quote(fw_url, safe='')}", method="POST", data=b"")
    req.add_header("Authorization", auth)
    expected = build_version(build)
    before = device_version(a.device, auth)
    if before:
        print(f"display is running {before['version']} from {before['slot']} (up {before['uptime_s']} s, "
              f"last restart: {before['reset']}; internal RAM {before['free_kb']} KB free, largest block "
              f"{before['largest_kb']} KB)")
    else:
        print("display gave no /version (an older firmware, or not answering)")
    if expected:
        print(f"sending {expected}")
    # The display needs a free 8KB chunk of internal RAM to start the update
    # task; right after a sync or with a panel open there can briefly be none
    # (it answers 503). Retry a few times. Any other refusal (400) is final and
    # its explanation is printed.
    for attempt in range(1, 7):
        try:
            with urllib.request.urlopen(req, timeout=15) as r:
                print(r.status, r.read().decode().strip())
            break
        except urllib.error.HTTPError as e:
            if e.code != 503 or attempt == 6:
                print(f"  the display refused ({e.code}): {e.read().decode().strip()}")
                sys.exit(1)
            print(f"  display not ready (503) - retrying in 5s ({attempt}/6)")
            threading.Event().wait(5)
        except (urllib.error.URLError, OSError) as e:
            # a dropped connection or a timeout - e.g. the display restarting. Try again.
            if attempt == 6:
                print(f"  no answer from the display: {e}")
                sys.exit(1)
            print(f"  connection problem ({e.__class__.__name__}) - retrying in 5s ({attempt}/6)")
            threading.Event().wait(5)
    print(f"serving {fw_url} - waiting for the device to download it (up to {a.wait}s, Ctrl-C to stop)")
    try:
        if not done.wait(a.wait):
            print("timed out waiting for the download")
            return
        threading.Event().wait(5)        # let it finish writing and restart
    except KeyboardInterrupt:
        return

    # Did it come back on the new image? A new image that dies early is rolled back by the bootloader.
    if not expected:
        print("no project_description.json next to the build - can't check which image came back")
        return
    t_done = time.monotonic()
    last = None
    while time.monotonic() - t_done < a.verify_wait:
        v = device_version(a.device, auth)
        if v:
            last = v
            # same version AND restarted since the download finished (it could be a re-send of the same build)
            if v["version"] == expected and v["uptime_s"] <= (time.monotonic() - t_done) + 15:
                print(f"OK: display is running {v['version']} from {v['slot']} (up {v['uptime_s']} s, "
                      f"last restart: {v['reset']})")
                return
        threading.Event().wait(4)
    if last is None:
        print(f"NO ANSWER: the display did not answer /version within {a.verify_wait} s")
    elif last["version"] != expected:
        print(f"FAILED: display is running {last['version']} from {last['slot']} (up {last['uptime_s']} s), not {expected} - "
              f"the new image did not start, or was rolled back. Its last restart: {last['reset']}")
    else:
        print(f"FAILED: display reports {expected} but has not restarted (up {last['uptime_s']} s)")
    sys.exit(2)


p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
sub = p.add_subparsers(dest="cmd", required=True)
l = sub.add_parser("logs")
l.add_argument("--port", type=int, default=9999)
l.add_argument("--file")
l.add_argument("--from", dest="sender", help="only accept packets from this IP (default: first sender that sends log lines)")
l.set_defaults(fn=cmd_logs)
o = sub.add_parser("ota")
o.add_argument("--device", required=True, help="the display's IP or hostname")
o.add_argument("--build", default="build-dev/gcal_display.bin", help="a development build (tools/dev_build.sh)")
o.add_argument("--serve-port", type=int, default=8070)
o.add_argument("--wait", type=int, default=300, help="max seconds to keep serving")
o.add_argument("--verify-wait", type=int, default=150, help="max seconds to wait for the display to come back on the new image")
o.set_defaults(fn=cmd_ota)
a = p.parse_args()
a.fn(a)
