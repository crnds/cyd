#pragma once

// Copy this file to config.h and fill in your details. config.h is gitignored.

// Leave WIFI_SSID as "" (empty) to boot straight into the first-boot AP setup
// portal instead: the board opens its own WiFi hotspot ("CYD-Setup-XXXX"),
// serves a page at 192.168.4.1 for entering your real network's SSID/password
// from a phone, then reboots once submitted. Submitted creds are saved to
// internal flash (NVS) -- no SD card needed.
#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Where to find the Mac running server/usage_server.py.
//
// RECOMMENDED: use your Mac's ".local" name (Bonjour/mDNS). The board will
// then find the Mac by name even after its IP address changes (which happens
// often on a laptop that sleeps/wakes or rejoins WiFi). Find the name on the
// Mac with:  scutil --get LocalHostName   then add ".local", e.g.:
#define SERVER_HOST "eUnite-MBA-M3-DN-2.local"
//
// ALTERNATIVE: a fixed IP like "192.168.1.42" also works, but if your router
// hands the Mac a different IP later you'll have to update this and re-flash.

#define SERVER_PORT 8787
