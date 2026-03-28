#include <Arduino.h>
#include <WiFi.h>
#include <LittleFS.h>

#include <mbedtls/ssl.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/x509_crt.h>
#include <mbedtls/pk.h>
#include <mbedtls/error.h>
#include <mbedtls/base64.h>
#include <mbedtls/ssl_cache.h>

#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <fcntl.h>

const char* WIFI_SSID     = " ";
const char* WIFI_PASSWORD = " ";

#define HTTPS_PORT     443
#define RECV_TIMEOUT_S 10
#define AUTH_USER      "admin"
#define AUTH_PASS      "password"

#define PATH_CERT      "/server.crt"
#define PATH_KEY       "/server.key"
#define PATH_LOGIN     "/login.html"
#define PATH_DASHBOARD "/dashboard.html"

static mbedtls_entropy_context   g_entropy;
static mbedtls_ctr_drbg_context  g_ctr_drbg;
static mbedtls_x509_crt          g_srvcert;
static mbedtls_pk_context        g_pkey;
static mbedtls_ssl_config        g_conf;
static mbedtls_ssl_cache_context g_cache;

static int g_serverFd = -1;

static void tlsDebugCb(void*, int level,
                       const char* file, int line, const char* str) {
    if (level <= 1) {
        // Виводимо тільки критичні повідомлення
        Serial.printf("[mbedTLS] %s:%d %s", file, line, str);
    }
}

bool initFS() {
    if (!LittleFS.begin(true)) {
        Serial.println("[FS] ПОМИЛКА монтування!");
        return false;
    }
    Serial.println("[FS] LittleFS OK");
    File root = LittleFS.open("/");
    File f = root.openNextFile();
    while (f) {
        Serial.printf("[FS]   %-20s %5d bytes\n", f.name(), f.size());
        f = root.openNextFile();
    }
    return true;
}

size_t readFileBuf(const char* path, uint8_t* buf, size_t maxLen) {
    File f = LittleFS.open(path, "r");
    if (!f) { Serial.printf("[FS] Not found: %s\n", path); return 0; }
    size_t len = f.read(buf, maxLen - 1);
    buf[len] = '\0';
    f.close();
    Serial.printf("[FS] Read '%s': %d bytes\n", path, len);
    return len;
}

bool initTLS() {
    char err[128];
    int  ret;

    mbedtls_entropy_init(&g_entropy);
    mbedtls_ctr_drbg_init(&g_ctr_drbg);
    mbedtls_x509_crt_init(&g_srvcert);
    mbedtls_pk_init(&g_pkey);
    mbedtls_ssl_config_init(&g_conf);
    mbedtls_ssl_cache_init(&g_cache);

    // Seed RNG
    ret = mbedtls_ctr_drbg_seed(&g_ctr_drbg, mbedtls_entropy_func,
                                 &g_entropy,
                                 (const unsigned char*)"lab5", 4);
    if (ret != 0) {
        mbedtls_strerror(ret, err, sizeof(err));
        Serial.printf("[TLS] ctr_drbg_seed: %s\n", err);
        return false;
    }

    uint8_t* certBuf = (uint8_t*)malloc(4096);
    uint8_t* keyBuf  = (uint8_t*)malloc(4096);
    if (!certBuf || !keyBuf) {
        Serial.println("[TLS] malloc failed — не вистачає heap!");
        free(certBuf); free(keyBuf); return false;
    }

    size_t certLen = readFileBuf(PATH_CERT, certBuf, 4096);
    if (certLen == 0) { free(certBuf); free(keyBuf); return false; }

    certBuf[certLen] = '\0';
    ret = mbedtls_x509_crt_parse(&g_srvcert, certBuf, certLen + 1);
    if (ret != 0) {
        mbedtls_strerror(ret, err, sizeof(err));
        Serial.printf("[TLS] x509_crt_parse FAILED: -0x%04X %s\n", -ret, err);
        Serial.println("[TLS] Перегенеруй сертифікат командою з README!");
        free(certBuf); free(keyBuf); return false;
    }
    Serial.printf("[TLS] Certificate loaded OK (%d bytes)\n", certLen);

    size_t keyLen = readFileBuf(PATH_KEY, keyBuf, 4096);
    if (keyLen == 0) { free(certBuf); free(keyBuf); return false; }

    keyBuf[keyLen] = '\0';
#if MBEDTLS_VERSION_MAJOR >= 3
    ret = mbedtls_pk_parse_key(&g_pkey, keyBuf, keyLen + 1, nullptr, 0,
                                mbedtls_ctr_drbg_random, &g_ctr_drbg);
#else
    ret = mbedtls_pk_parse_key(&g_pkey, keyBuf, keyLen + 1, nullptr, 0);
#endif
    if (ret != 0) {
        mbedtls_strerror(ret, err, sizeof(err));
        Serial.printf("[TLS] pk_parse_key FAILED: -0x%04X %s\n", -ret, err);
        free(certBuf); free(keyBuf); return false;
    }
    Serial.printf("[TLS] Private key loaded OK (%d bytes)\n", keyLen);

    free(certBuf);
    free(keyBuf);

    ret = mbedtls_ssl_config_defaults(&g_conf,
                                       MBEDTLS_SSL_IS_SERVER,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) { Serial.println("[TLS] config_defaults failed"); return false; }

    mbedtls_ssl_conf_min_version(&g_conf,
                                  MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_3);
    mbedtls_ssl_conf_max_version(&g_conf,
                                  MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_3);

    mbedtls_ssl_conf_session_cache(&g_conf, &g_cache,
                                    mbedtls_ssl_cache_get,
                                    mbedtls_ssl_cache_set);

    mbedtls_ssl_conf_rng(&g_conf, mbedtls_ctr_drbg_random, &g_ctr_drbg);
    mbedtls_ssl_conf_authmode(&g_conf, MBEDTLS_SSL_VERIFY_NONE);

    static const int ciphersuites[] = {
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384,
        MBEDTLS_TLS_RSA_WITH_AES_128_GCM_SHA256,
        MBEDTLS_TLS_RSA_WITH_AES_256_GCM_SHA384,
        MBEDTLS_TLS_RSA_WITH_AES_128_CBC_SHA256,
        0
    };
    mbedtls_ssl_conf_ciphersuites(&g_conf, ciphersuites);

    ret = mbedtls_ssl_conf_own_cert(&g_conf, &g_srvcert, &g_pkey);
    if (ret != 0) {
        mbedtls_strerror(ret, err, sizeof(err));
        Serial.printf("[TLS] own_cert failed: %s\n", err);
        return false;
    }

    Serial.println("[TLS] Config OK — TLS 1.2, RSA cipher suites");
    return true;
}

String b64decode(const String& in) {
    unsigned char out[512] = {};
    size_t outLen = 0;
    mbedtls_base64_decode(out, sizeof(out) - 1, &outLen,
                          (const unsigned char*)in.c_str(), in.length());
    out[outLen] = '\0';
    return String((char*)out);
}

bool checkAuth(const String& req) {
    const String marker = "Authorization: Basic ";
    int i = req.indexOf(marker);
    if (i < 0) return false;
    i += marker.length();
    int j = req.indexOf("\r\n", i);
    String decoded  = b64decode(req.substring(i, j < 0 ? req.length() : j));
    String expected = String(AUTH_USER) + ":" + String(AUTH_PASS);
    Serial.printf("[Auth] '%s' -> %s\n",
                  decoded.c_str(),
                  decoded.equals(expected) ? "OK" : "DENIED");
    return decoded.equals(expected);
}

String getPath(const String& req) {
    int a = req.indexOf(' ') + 1;
    int b = req.indexOf(' ', a);
    return (a > 0 && b > a) ? req.substring(a, b) : String("/");
}

void tlsWrite(mbedtls_ssl_context* ssl, const char* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int r = mbedtls_ssl_write(ssl,
                                  (const unsigned char*)data + sent,
                                  len - sent);
        if (r == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        if (r < 0) break;
        sent += r;
    }
}

void sendHeaders(mbedtls_ssl_context* ssl,
                 int code, const char* status,
                 const char* ctype, size_t bodyLen,
                 const char* extra = "") {
    char hdr[512];
    snprintf(hdr, sizeof(hdr),
             "HTTP/1.1 %d %s\r\n"
             "Content-Type: %s\r\n"
             "Content-Length: %u\r\n"
             "Connection: close\r\n"
             "Cache-Control: no-store\r\n"
             "%s\r\n",
             code, status, ctype, (unsigned)bodyLen, extra);
    tlsWrite(ssl, hdr, strlen(hdr));
}

void sendStr(mbedtls_ssl_context* ssl,
             int code, const char* status,
             const char* ctype, const char* body,
             const char* extra = "") {
    sendHeaders(ssl, code, status, ctype, strlen(body), extra);
    tlsWrite(ssl, body, strlen(body));
}

void sendFile(mbedtls_ssl_context* ssl,
              int code, const char* status,
              const char* ctype, const char* path) {
    File f = LittleFS.open(path, "r");
    if (!f) {
        sendStr(ssl, 404, "Not Found", "text/plain", "404 File not found");
        return;
    }
    sendHeaders(ssl, code, status, ctype, f.size());
    uint8_t chunk[512];
    while (f.available()) {
        size_t n = f.read(chunk, sizeof(chunk));
        if (n > 0) tlsWrite(ssl, (const char*)chunk, n);
    }
    f.close();
}

void handleConnection(int clientFd) {
    mbedtls_ssl_context ssl;
    mbedtls_net_context net;
    mbedtls_ssl_init(&ssl);
    mbedtls_net_init(&net);
    net.fd = clientFd;

    int ret = mbedtls_ssl_setup(&ssl, &g_conf);
    if (ret != 0) { Serial.println("[TLS] ssl_setup failed"); goto done; }

    mbedtls_ssl_set_bio(&ssl, &net,
                        mbedtls_net_send,
                        mbedtls_net_recv,
                        nullptr);

    // Handshake
    do { ret = mbedtls_ssl_handshake(&ssl); }
    while (ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE);

    if (ret != 0) {
        char e[128];
        mbedtls_strerror(ret, e, sizeof(e));
        Serial.printf("[TLS] Handshake FAILED: -0x%04X: %s\n", (unsigned)(-ret), e);
        goto done;
    }

    {
        // Виводимо яку версію TLS та cipher suite погодили
        Serial.printf("[TLS] Handshake OK | %s | %s\n",
                      mbedtls_ssl_get_version(&ssl),
                      mbedtls_ssl_get_ciphersuite(&ssl));

        // Читаємо HTTP запит
        char buf[4096] = {};
        int  total = 0;

        while (total < (int)sizeof(buf) - 1) {
            ret = mbedtls_ssl_read(&ssl,
                                   (unsigned char*)buf + total,
                                   sizeof(buf) - total - 1);
            if (ret == MBEDTLS_ERR_SSL_WANT_READ) { delay(1); continue; }
            if (ret <= 0) break;
            total += ret;
            buf[total] = '\0';
            if (strstr(buf, "\r\n\r\n")) break;
        }

        if (total == 0) { goto done; }

        String request(buf);
        String path = getPath(request);
        Serial.printf("[HTTPS] %s\n", request.c_str());
        Serial.printf("[HTTPS] %s\n", path.c_str());

        if (path == "/" || path == "/index.html") {
            sendFile(&ssl, 200, "OK", "text/html; charset=utf-8", PATH_LOGIN);
        }
        else if (path == "/dashboard") {
            if (!checkAuth(request))
                sendStr(&ssl, 401, "Unauthorized", "text/plain",
                        "401 Unauthorized",
                        "WWW-Authenticate: Basic realm=\"ESP32-Lab5\"\r\n");
            else
                sendFile(&ssl, 200, "OK",
                         "text/html; charset=utf-8", PATH_DASHBOARD);
        }
        else if (path == "/api/status") {
            if (!checkAuth(request))
                sendStr(&ssl, 401, "Unauthorized", "application/json",
                        "{\"error\":\"unauthorized\"}",
                        "WWW-Authenticate: Basic realm=\"ESP32-Lab5\"\r\n");
            else
                sendStr(&ssl, 200, "OK", "application/json",
                        "{\"ok\":true,\"chip\":\"ESP32-C3\","
                        "\"tls\":\"mbedTLS\",\"fs\":\"LittleFS\"}");
        }
        else {
            sendStr(&ssl, 404, "Not Found", "text/plain", "404 Not Found");
        }
    }

    mbedtls_ssl_close_notify(&ssl);

done:
    mbedtls_ssl_free(&ssl);
    close(clientFd);
}

bool startServer() {
    g_serverFd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (g_serverFd < 0) { Serial.println("[TCP] socket() failed"); return false; }

    int opt = 1;
    setsockopt(g_serverFd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(HTTPS_PORT);

    if (bind(g_serverFd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        Serial.println("[TCP] bind() failed"); close(g_serverFd); return false;
    }
    if (listen(g_serverFd, 3) < 0) {
        Serial.println("[TCP] listen() failed"); close(g_serverFd); return false;
    }
    fcntl(g_serverFd, F_SETFL, O_NONBLOCK);
    Serial.printf("[TCP] Listening on :%d\n", HTTPS_PORT);
    return true;
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("\n=== Lab5: HTTPS + LittleFS (Fixed Handshake) ===");

    if (!initFS())    { while(1) delay(1000); }

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.print("[WiFi] Connecting");
    while (WiFi.status() != WL_CONNECTED) { delay(500); Serial.print("."); }
    Serial.printf("\n[WiFi] IP: %s\n", WiFi.localIP().toString().c_str());

    if (!initTLS())    { while(1) delay(1000); }
    if (!startServer()){ while(1) delay(1000); }

    Serial.printf("\n>>> https://%s\n", WiFi.localIP().toString().c_str());
    Serial.printf(">>> Login: %s / %s\n", AUTH_USER, AUTH_PASS);
    Serial.println(">>> Browser: Advanced -> Proceed (self-signed)\n");
}

void loop() {
    struct sockaddr_in clientAddr;
    socklen_t addrLen = sizeof(clientAddr);

    int clientFd = accept(g_serverFd,
                          (struct sockaddr*)&clientAddr, &addrLen);
    if (clientFd < 0) { delay(5); return; }

    struct timeval tv = { .tv_sec = RECV_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(clientFd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    Serial.printf("[TCP] Client: %s\n", inet_ntoa(clientAddr.sin_addr));
    handleConnection(clientFd);
}
