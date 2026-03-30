#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Wire.h>
#include <BH1750.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

const char* WIFI_SSID     = "Shiz";
const char* WIFI_PASSWORD = "01102005";

WebServer httpServer(80);
WebSocketsServer wsServer(81);
BH1750 lightMeter;
unsigned long lastSensorRead = 0;

void readAndBroadcast() {
  float lux = lightMeter.readLightLevel();
  if (lux >= 0) {
    StaticJsonDocument<64> doc;
    doc["lux"] = lux;
    String payload;
    serializeJson(doc, payload);
    wsServer.broadcastTXT(payload);
  }
}

void setup() {
  Serial.begin(115200);
  LittleFS.begin(true);
  Wire.begin(8, 9);
  lightMeter.begin(BH1750::CONTINUOUS_HIGH_RES_MODE);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) delay(500);

  httpServer.on("/", HTTP_GET, []() {
    File file = LittleFS.open("/index.html", "r");
    httpServer.streamFile(file, "text/html");
    file.close();
  });

  httpServer.begin();
  wsServer.begin();
}

void loop() {
  httpServer.handleClient();
  wsServer.loop();
  if (millis() - lastSensorRead >= 500) {
    lastSensorRead = millis();
    readAndBroadcast();
  }
}