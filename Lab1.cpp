#include <WiFi.h>
#include <WebServer.h>
#include <SPIFFS.h>

const char* WIFI_SSID     = "Shiz";     
const char* WIFI_PASSWORD = "01102005"; 

WebServer server(80);


void handleRoot() {
  File file = SPIFFS.open("/index.html", "r");
  if (!file) {
    server.send(404, "text/plain", "404: index.html not found in SPIFFS");
    Serial.println("[ERROR] index.html не знайдено у SPIFFS");
    return;
  }

  server.streamFile(file, "text/html");
  file.close();
  Serial.println("[HTTP] GET / -> 200 OK");
}

void handleNotFound() {
  server.send(404, "text/plain", "404: Resource not found");
  Serial.printf("[HTTP] 404 -> %s\n", server.uri().c_str());
}

void connectWiFi() {
  Serial.printf("\n[WiFi] Підключення до: %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\n[WiFi] Підключено! IP: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\n[WiFi] ПОМИЛКА: не вдалося підключитися!");
  }
}

void initSPIFFS() {
  if (!SPIFFS.begin(true)) {
    Serial.println("[SPIFFS] ПОМИЛКА ініціалізації!");
    return;
  }
  Serial.println("[SPIFFS] Змонтовано успішно");

  Serial.println("[SPIFFS] Файли у Flash:");
  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    Serial.printf("   %s (%d bytes)\n", file.name(), file.size());
    file = root.openNextFile();
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=== Lab1: Embedded HTTP Server ===");

  initSPIFFS();

  connectWiFi();

  server.on("/", HTTP_GET, handleRoot);
  server.onNotFound(handleNotFound);

  server.begin();
  Serial.println("[HTTP] Сервер запущено на порті 80");
  Serial.printf("[HTTP] Відкрийте у браузері: http://%s\n",
                WiFi.localIP().toString().c_str());
}

void loop() {
  server.handleClient();
}
