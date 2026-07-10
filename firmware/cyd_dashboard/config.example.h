#pragma once

// Copy this file to config.h and fill in your details. config.h is gitignored.

#define WIFI_SSID     "your-wifi-name"
#define WIFI_PASSWORD "your-wifi-password"

// Where to find the Mac running server/usage_server.py.
//
// RECOMMENDED: use your Mac's ".local" name (Bonjour/mDNS). The board will
// then find the Mac by name even after its IP address changes (which happens
// often on a laptop that sleeps/wakes or rejoins WiFi). Find the name on the
// Mac with:  scutil --get LocalHostName   then add ".local", e.g.:
#define SERVER_HOST "Dusits-MacBook-Air.local"
//
// ALTERNATIVE: a fixed IP like "192.168.1.42" also works, but if your router
// hands the Mac a different IP later you'll have to update this and re-flash.

#define SERVER_PORT 8787
