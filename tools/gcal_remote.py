#!/usr/bin/env python3
"""Wireless log viewer and firmware pusher for the gcal display (stdlib only).

  gcal_remote.py logs [--port 9999] [--file device.log]
      Listens for the UDP log the device sends to its "Log destination"
      (config page -> Developer). Prints each line, colour codes stripped.

  gcal_remote.py ota --device 192.168.1.50 [--build build/gcal_display.bin]
      Serves the built .bin from this machine and tells the device to fetch
      it (POST /ota on its config web server, same password as the settings
      page - set GCAL_PASS). The device restarts into the new image; one that
      doesn't survive its first minute rolls back by itself.

Serial logging and `idf.py flash` over USB are unaffected and still work.
"""
import argparse, base64, functools, http.server, os, re, socket, socketserver
import sys, threading, urllib.parse, urllib.request

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


def cmd_ota(a):
    build = os.path.abspath(a.build)
    if not os.path.isfile(build):
        sys.exit(f"{build} not found - run idf.py build first")
    password = os.environ.get("GCAL_PASS")
    if not password:
        sys.exit("set GCAL_PASS to the config web page password")
    ip = local_ip(a.device)

    handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=os.path.dirname(build))
    handler.log_message = lambda *args: None
    socketserver.TCPServer.allow_reuse_address = True
    srv = socketserver.ThreadingTCPServer(("0.0.0.0", a.serve_port), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    fw_url = f"http://{ip}:{a.serve_port}/{os.path.basename(build)}"
    req = urllib.request.Request(
        f"http://{a.device}/ota?url={urllib.parse.quote(fw_url, safe='')}", method="POST", data=b"")
    req.add_header("Authorization", "Basic " + base64.b64encode(f"x:{password}".encode()).decode())
    with urllib.request.urlopen(req, timeout=15) as r:
        print(r.status, r.read().decode().strip())
    print(f"serving {fw_url} - waiting for the device to finish downloading (Ctrl-C to stop)")
    try:
        threading.Event().wait(a.wait)
    except KeyboardInterrupt:
        pass


p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
sub = p.add_subparsers(dest="cmd", required=True)
l = sub.add_parser("logs")
l.add_argument("--port", type=int, default=9999)
l.add_argument("--file")
l.add_argument("--from", dest="sender", help="only accept packets from this IP (default: first sender that sends log lines)")
l.set_defaults(fn=cmd_logs)
o = sub.add_parser("ota")
o.add_argument("--device", required=True, help="the display's IP or hostname")
o.add_argument("--build", default="build/gcal_display.bin")
o.add_argument("--serve-port", type=int, default=8070)
o.add_argument("--wait", type=int, default=120, help="seconds to keep serving")
o.set_defaults(fn=cmd_ota)
a = p.parse_args()
a.fn(a)
