#pragma once

/* Forwards the device's log output over UDP to a host on the LAN, as well as
 * (not instead of) the normal serial/USB output. `host_port` is "host:port" or
 * "ip:port"; a blank string leaves everything off. Call once, early - lines
 * logged before Wi-Fi is up are held in a PSRAM ring buffer and sent once it
 * is. Receive them with tools/gcal_remote.py logs (or any UDP listener). */
void remote_log_start(const char *host_port);
