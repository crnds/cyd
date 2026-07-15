// First-boot WiFi config portal. Entered only when STATE.sdOk is true AND
// cfgWifiSsid is empty after loadRuntimeConfig() runs — i.e. WIFI_SSID was
// left blank in config.h and no wifi_ssid key has ever been saved to
// /config.json. Requires the SD card because /config.json is the only
// writable place credentials can persist (config.h is baked into flash at
// compile time); with no card there's nowhere to save to, so this is
// skipped and connectWifi() just runs with the (empty) compiled defaults as
// it always has.
//
// Runs entirely inside setup(), before networkTask/loop() start, so it's
// free to block: brings up an open softAP + captive portal, waits for a
// phone to submit new WiFi creds, writes them to /config.json, then
// ESP.restart()s into the normal boot path (which now finds a saved
// wifi_ssid and skips this on the next boot). Never returns normally.
// Split out of cyd_dashboard.ino.
#include "state.h"

static void drawApSetupScreen(const char* apName, IPAddress ip, int stations) {
  g->fillScreen(COL_BG);
  g->setTextColor(COL_ACCENT);
  g->setTextSize(3);
  g->setCursor(10, 16);
  g->print("WIFI SETUP");

  g->setTextColor(COL_TEXT);
  g->setTextSize(2);
  g->setCursor(10, 64);
  g->print("1. Join WiFi:");
  g->setTextColor(COL_ACCENT);
  g->setCursor(10, 88);
  g->print(apName);

  g->setTextColor(COL_TEXT);
  g->setCursor(10, 124);
  g->print("2. Open in browser:");
  g->setTextColor(COL_ACCENT);
  g->setCursor(10, 148);
  g->print(ip.toString());

  g->setTextColor(COL_TEXT2);
  g->setTextSize(1);
  g->setCursor(10, 200);
  g->print(stations > 0 ? "Phone connected -- fill in the form" : "Waiting for a phone to join...");
  presentFrame();
}

void runApSetup() {
  logDiag("ap_setup_entered");

  // AP_STA (not plain AP) so scanNetworks() below still works, letting the
  // setup page offer a pick-list of nearby SSIDs instead of requiring exact,
  // error-prone manual typing on a phone keyboard.
  WiFi.mode(WIFI_AP_STA);
  int found = WiFi.scanNetworks();

  uint8_t mac[6];
  WiFi.macAddress(mac);
  char apName[24];
  snprintf(apName, sizeof(apName), "CYD-Setup-%02X%02X", mac[4], mac[5]);
  WiFi.softAP(apName);  // open network, no password -- see AP setup design notes
  IPAddress apIp = WiFi.softAPIP();

  String options;
  for (int i = 0; i < found; i++) {
    options += "<option value=\"" + WiFi.SSID(i) + "\">";
  }

  DNSServer dnsServer;
  dnsServer.start(53, "*", apIp);  // redirect all DNS lookups to us (captive portal)

  WebServer server(80);

  server.on("/", HTTP_GET, [&server, &options]() {
    String html = String(
      "<!DOCTYPE html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
      "<title>CYD Setup</title><style>"
      "body{font-family:sans-serif;background:#08090d;color:#f1f5f9;padding:24px;max-width:360px;margin:auto}"
      "h2{color:#F4620E}input{width:100%;box-sizing:border-box;padding:10px;margin:6px 0;"
      "border-radius:6px;border:1px solid #333;background:#161b27;color:#fff}"
      "button{width:100%;padding:12px;background:#F4620E;color:#fff;border:none;"
      "border-radius:6px;font-size:16px;margin-top:12px}"
      "</style></head><body>"
      "<h2>CYD Dashboard Setup</h2>"
      "<p>Join your home WiFi, then the board reboots into the dashboard.</p>"
      "<form action='/save' method='POST'>"
      "<input list='nets' name='ssid' placeholder='WiFi name' autocomplete='off' required>"
      "<datalist id='nets'>") + options + String("</datalist>"
      "<input name='password' type='password' placeholder='WiFi password'>"
      "<button type='submit'>Save &amp; Reboot</button>"
      "</form></body></html>");
    server.send(200, "text/html", html);
  });

  server.on("/save", HTTP_POST, [&server]() {
    String ssid = server.arg("ssid");
    String password = server.arg("password");
    if (ssid.length() == 0) {
      server.send(400, "text/plain", "SSID required");
      return;
    }
    saveWifiCredsToSD(ssid, password);
    logDiag("ap_setup_saved_rebooting");
    server.send(200, "text/html",
      "<html><body style='font-family:sans-serif;background:#08090d;color:#f1f5f9;padding:24px'>"
      "<h2 style='color:#F4620E'>Saved</h2><p>Rebooting into the dashboard...</p></body></html>");
    delay(500);
    ESP.restart();
  });

  // Captive-portal auto-popup: redirect any unrecognized path to the setup
  // page. This covers the common cases well enough; if a phone's OS doesn't
  // auto-open the popup, the on-screen IP works from any browser too.
  server.onNotFound([&server, apIp]() {
    server.sendHeader("Location", String("http://") + apIp.toString() + "/", true);
    server.send(302, "text/plain", "");
  });

  server.begin();

  uint32_t lastDrawMs = 0;
  drawApSetupScreen(apName, apIp, WiFi.softAPgetStationNum());
  for (;;) {
    dnsServer.processNextRequest();
    server.handleClient();
    uint32_t now = millis();
    if (now - lastDrawMs >= 1000) {
      lastDrawMs = now;
      drawApSetupScreen(apName, apIp, WiFi.softAPgetStationNum());
    }
    delay(5);
  }
}
