#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <LittleFS.h>

#define AP_SSID "Harvat_AP"
#define DNS_PORT 53
#define HTTP_PORT 80

DNSServer dnsServer;
WebServer httpServer(80);
Preferences prefs;

void handlePortal() {
  File file = LittleFS.open("/portal.html", "r");
  if (file) {
    httpServer.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
    httpServer.streamFile(file, "text/html");
    file.close();
  }
}

void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);
  
  prefs.begin("wifi", true);
  String ssid = prefs.getString("ssid", "");
  String pass = prefs.getString("password", "");
  prefs.end();

  WiFi.begin(ssid.c_str(), pass.c_str());
  
  // Якщо не підключився за 10с — запускаємо портал
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) { delay(500); }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("Connected!");
    httpServer.on("/", []() { httpServer.send(200, "text/plain", "Work mode"); });
  } else {
    WiFi.softAP(AP_SSID);
    dnsServer.start(DNS_PORT, "*", WiFi.softAPIP());
    
    httpServer.on("/", handlePortal);
    httpServer.onNotFound(handlePortal); // Для Captive Portal
    
    httpServer.on("/save", HTTP_POST, []() {
      prefs.begin("wifi", false);
      prefs.putString("ssid", httpServer.arg("ssid"));
      prefs.putString("password", httpServer.arg("password"));
      prefs.end();
      httpServer.send(200, "text/plain", "Rebooting...");
      delay(2000); ESP.restart();
    });
  }
  httpServer.begin();
}

void loop() {
  dnsServer.processNextRequest();
  httpServer.handleClient();
}