#include "web.h"
#include <WiFi.h>
#include <WebServer.h>
#include "config.h"
#include "control.h"
#include "sim.h"
#include "web_ui.h"

static WebServer server(80);

static void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json", body);
}

// /set?cadence=90&power_noise=5   (GET, or POST with form fields)
static void handleSet() {
  int applied = 0;
  String errors;
  for (int i = 0; i < server.args(); i++) {
    String err;
    if (applySetting(server.argName(i), server.arg(i), err)) {
      applied++;
    } else {
      if (errors.length()) errors += ',';
      errors += "\"";
      errors += jsonEscape(err);
      errors += "\"";
    }
  }
  String body = String("{\"ok\":") + (errors.length() || applied == 0 ? "false" : "true") +
                ",\"applied\":" + applied + ",\"errors\":[" + errors + "]}";
  sendJson(errors.length() || applied == 0 ? 400 : 200, body);
}

// /do?cmd=drop | stall&arg=10 | inject&arg=short | reset | defaults
static void handleDo() {
  String msg;
  bool ok = runAction(server.arg("cmd"), server.arg("arg"), msg);
  sendJson(ok ? 200 : 400, String("{\"ok\":") + (ok ? "true" : "false") + ",\"msg\":\"" + jsonEscape(msg) + "\"}");
}

void webBegin() {
  WiFi.mode(WIFI_AP);
  const bool secured = strlen(WIFI_AP_PASSWORD) >= 8;
  WiFi.softAP(WIFI_AP_SSID, secured ? WIFI_AP_PASSWORD : nullptr, WIFI_AP_CHANNEL);
  sim.logf("Wi-Fi AP \"%s\"%s at http://%s/", WIFI_AP_SSID, secured ? "" : " (open)", WiFi.softAPIP().toString().c_str());

  server.on("/", HTTP_GET, [] { server.send_P(200, "text/html; charset=utf-8", INDEX_HTML); });
  server.on("/state", HTTP_GET, [] { sendJson(200, stateJson()); });
  server.on("/meta", HTTP_GET, [] { sendJson(200, metaJson()); });
  server.on("/set", handleSet);
  server.on("/do", handleDo);
  server.onNotFound([] { sendJson(404, "{\"ok\":false,\"msg\":\"not found\"}"); });
  server.begin();
}

void webLoop() { server.handleClient(); }
