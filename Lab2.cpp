#include <WiFi.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <LittleFS.h>

const char* WIFI_SSID     = " ";
const char* WIFI_PASSWORD = " ";

#define LED_PIN 8
WebServer server(80);
bool ledState = false;

// Ініціалізація LittleFS
void initFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("[FS] Помилка монтування LittleFS!");
    return;
  }
  Serial.println("[FS] LittleFS монтовано успішно");
}

// Видача HTML файлу з LittleFS
void handleDashboard() {
  File file = LittleFS.open("/index.html", "r");
  if (!file) {
    server.send(404, "text/plain", "File not found");
    return;
  }
  server.streamFile(file, "text/html");
  file.close();
}

void handleStatus() {
  StaticJsonDocument<64> doc;
  doc["led_on"] = ledState;
  String response;
  serializeJson(doc, response);
  server.send(200, "application/json", response);
}

void handleControl() {
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"error\":\"no body\"}");
    return;
  }

  StaticJsonDocument<128> doc;
  deserializeJson(doc, server.arg("plain"));
  const char* command = doc["command"];

  if (strcmp(command, "on") == 0) ledState = true;
  else if (strcmp(command, "off") == 0) ledState = false;

  digitalWrite(LED_PIN, ledState ? LOW : HIGH); // Low Trigger LED
  server.send(200, "application/json", "{\"result\":\"ok\"}");
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  initFS();

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
  Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());

  server.on("/", HTTP_GET, handleDashboard);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/control", HTTP_POST, handleControl);
  
  server.begin();
}

void loop() {
  server.handleClient();
}