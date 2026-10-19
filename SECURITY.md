# Security policy

This is a hobby project for a home network, maintained on a best-effort basis. It is not
hardened against a hostile network or physical access - the README's
[Security notes](README.md#security-notes) list exactly what is and isn't protected. Please read
them first: issues listed there as limitations are known.

## Supported versions

The latest commits on the `main` and `with-HA-weather` branches. There are no other maintained
releases.

## Reporting a vulnerability

Please **don't open a public issue** for something exploitable. Use GitHub's private reporting:

1. Go to this repository's **Security** tab.
2. Choose **Report a vulnerability**.

(If you don't see that option, open an ordinary issue that says only that you have a security
report and would like a private way to send it - no details - and I'll set one up.)

Useful to include: what you found and where (file and function if you know them), what an attacker
needs (same Wi-Fi? physical access? a logged-in browser?), steps to reproduce, and what you would
expect instead.

You can expect an acknowledgement within a couple of weeks and a fix or a documented limitation
afterwards, as time allows. Credit in the fix is yours if you want it.

## Scope

In scope: the firmware in this repository - the config web page and its `/save` and `/ota`
routes, the setup portal, update handling, parsing of calendar/feed/weather data, and the storage
of settings. Out of scope: the issues the README already lists as known limitations, attacks that
need physical access to the board or card, and vulnerabilities in ESP-IDF, LVGL or other
third-party code (report those upstream).
