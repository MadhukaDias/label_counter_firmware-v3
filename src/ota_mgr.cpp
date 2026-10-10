#include "ota_mgr.h"
#include "ota_pubkey.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <esp_ota_ops.h>
#include <mbedtls/sha256.h>
#include <mbedtls/pk.h>

// ── Settings ──────────────────────────────────────────────────────────────────
#define OTA_BASE_URL       "https://github.com/MadhukaDias/label_counter_firmware-v3/releases/latest/download/"
#define OTA_MANIFEST_URL   OTA_BASE_URL "version.json"
#define OTA_BIN_URL        OTA_BASE_URL "firmware.bin"
#define OTA_FIRST_CHECK_MS 60000UL            // let the device settle after boot
#define OTA_INTERVAL_MS    (6UL * 3600UL * 1000UL)
#define OTA_RETRY_MS       (5UL * 60UL * 1000UL)  // after a failed attempt
#define OTA_STALL_MS       20000UL            // no data this long = connection dead
#define OTA_VALIDATE_MS    30000UL            // healthy uptime before keeping new fw

static volatile int  _progress = -1;
static TaskHandle_t  _task = nullptr;
static bool          _validated = false;

// Allow Arduino-ESP32 to leave a freshly-OTA'd image "pending verify" so the
// bootloader can roll back if we crash before otaMgrLoop() confirms it.
extern "C" bool verifyRollbackLater() { return true; }

// ── Helpers ───────────────────────────────────────────────────────────────────
static uint32_t verNum(const char* v) {
    if (*v == 'v' || *v == 'V') v++;
    unsigned a = 0, b = 0, c = 0;
    sscanf(v, "%u.%u.%u", &a, &b, &c);
    return a * 1000000UL + b * 1000UL + c;
}

static bool isDevBuild() { return strstr(FW_VERSION, "dev") != nullptr; }

static int hexToBytes(const char* hex, uint8_t* out, size_t max) {
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) return -1;
        out[i] = (uint8_t)v;
    }
    return (int)(n / 2);
}

static bool verifySignature(const uint8_t hash[32], const char* sigHex) {
    uint8_t sig[80];  // DER ECDSA P-256 is <= 72 bytes
    int sigLen = hexToBytes(sigHex, sig, sizeof(sig));
    if (sigLen <= 0) return false;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = mbedtls_pk_parse_public_key(&pk, (const uint8_t*)OTA_PUBKEY_PEM,
                                          sizeof(OTA_PUBKEY_PEM)) == 0 &&
              mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, 32, sig, sigLen) == 0;
    mbedtls_pk_free(&pk);
    return ok;
}

// ── One update attempt ────────────────────────────────────────────────────────
// Returns 0 = nothing to do, 1 = installed (reboots), -1 = failed (retry later).
// Safety: the new image goes to the idle OTA slot. Any failure before the final
// Update.end() leaves the running firmware completely untouched.
static int runCheck() {
    WiFiClientSecure client;
    client.setInsecure();   // integrity/authenticity come from the ECDSA signature below
    HTTPClient http;
    http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
    http.setTimeout(15000);

    // 1. Manifest
    if (!http.begin(client, OTA_MANIFEST_URL)) return -1;
    int code = http.GET();
    if (code != 200) { Serial.printf("[OTA] Manifest HTTP %d\n", code); http.end(); return -1; }
    String body = http.getString();
    http.end();

    JsonDocument doc;
    if (deserializeJson(doc, body)) { Serial.println("[OTA] Bad manifest"); return -1; }
    String ver = doc["version"] | "";
    String sha = doc["sha256"] | "";
    String sig = doc["sig"] | "";
    uint32_t size = doc["size"] | 0;
    if (!ver.length() || sha.length() != 64 || !sig.length() || !size) return -1;

    Preferences prefs;
    prefs.begin("ota", true);
    String bad = prefs.getString("bad", "");
    prefs.end();

    if (verNum(ver.c_str()) <= verNum(FW_VERSION)) { Serial.printf("[OTA] Up to date (%s)\n", FW_VERSION); return 0; }
    if (ver == bad) { Serial.printf("[OTA] %s previously failed validation, skipping\n", ver.c_str()); return 0; }

    const esp_partition_t* next = esp_ota_get_next_update_partition(nullptr);
    if (!next || size > next->size || size < 100000) { Serial.println("[OTA] Bad size"); return -1; }

    Serial.printf("[OTA] Updating %s -> %s (%u bytes)\n", FW_VERSION, ver.c_str(), (unsigned)size);

    // 2. Binary
    if (!http.begin(client, OTA_BIN_URL)) return -1;
    code = http.GET();
    if (code != 200 || (uint32_t)http.getSize() != size) {
        Serial.printf("[OTA] Bin HTTP %d len %d\n", code, http.getSize());
        http.end();
        return -1;
    }
    if (!Update.begin(size, U_FLASH)) { http.end(); return -1; }

    mbedtls_sha256_context sctx;
    mbedtls_sha256_init(&sctx);
    mbedtls_sha256_starts(&sctx, 0);

    WiFiClient* stream = http.getStreamPtr();
    static uint8_t buf[1460];
    uint32_t written = 0, lastData = millis();
    bool ok = true;
    _progress = 0;

    while (written < size) {
        if (WiFi.status() != WL_CONNECTED) { Serial.println("[OTA] WiFi lost"); ok = false; break; }
        int avail = stream->available();
        if (avail > 0) {
            size_t want = min((size_t)avail, min(sizeof(buf), (size_t)(size - written)));
            int n = stream->readBytes(buf, want);
            if (n <= 0) continue;
            mbedtls_sha256_update(&sctx, buf, n);
            if (Update.write(buf, n) != (size_t)n) { Serial.println("[OTA] Flash write failed"); ok = false; break; }
            written += n;
            lastData = millis();
            _progress = written * 100ULL / size;
        } else {
            if (!stream->connected()) { Serial.println("[OTA] Connection closed early"); ok = false; break; }
            if (millis() - lastData > OTA_STALL_MS) { Serial.println("[OTA] Download stalled"); ok = false; break; }
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    http.end();

    uint8_t hash[32];
    mbedtls_sha256_finish(&sctx, hash);
    mbedtls_sha256_free(&sctx);

    // 3. Verify everything BEFORE committing
    if (ok) {
        char hex[65];
        for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", hash[i]);
        if (!sha.equalsIgnoreCase(hex)) { Serial.println("[OTA] SHA-256 mismatch"); ok = false; }
        else if (!verifySignature(hash, sig.c_str())) { Serial.println("[OTA] Signature INVALID"); ok = false; }
    }
    if (ok && !(Update.end(true) && Update.isFinished())) { Serial.println("[OTA] Finalise failed"); ok = false; }

    _progress = -1;
    if (!ok) { Update.abort(); return -1; }

    prefs.begin("ota", false);
    prefs.putString("try", ver);   // cleared once the new image proves healthy
    prefs.end();
    Serial.println("[OTA] Installed, rebooting");
    delay(500);
    ESP.restart();
    return 1;
}

static void otaTask(void*) {
    vTaskDelay(pdMS_TO_TICKS(OTA_FIRST_CHECK_MS));
    for (;;) {
        uint32_t wait = OTA_INTERVAL_MS;
        if (WiFi.status() == WL_CONNECTED) {
            if (runCheck() < 0) wait = OTA_RETRY_MS;
        } else {
            wait = OTA_RETRY_MS;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait));   // woken early by otaRequestCheck()
    }
}

// ── Public API ────────────────────────────────────────────────────────────────
void otaMgrInit() {
    // If a previous update was tried but we are not running it, the bootloader
    // rolled us back: remember that version so we do not retry it forever.
    Preferences prefs;
    prefs.begin("ota", false);
    String tried = prefs.getString("try", "");
    if (tried.length() && tried != FW_VERSION) {
        prefs.putString("bad", tried);
        prefs.remove("try");
        Serial.printf("[OTA] Rolled back from %s\n", tried.c_str());
    }
    prefs.end();

    if (isDevBuild()) { Serial.println("[OTA] Dev build: auto-update disabled"); _validated = true; return; }
    xTaskCreate(otaTask, "OTA_Task", 16384, nullptr, 1, &_task);
}

void otaMgrLoop() {
    if (_validated || millis() < OTA_VALIDATE_MS) return;
    _validated = true;
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
        Preferences prefs;
        prefs.begin("ota", false);
        if (prefs.getString("try", "") == FW_VERSION) Serial.printf("[OTA] %s confirmed good\n", FW_VERSION);
        prefs.remove("try");
        prefs.end();
    }
}

void otaRequestCheck() {
    // Rate-limit: the broker is public, so ignore triggers within 30 s of the last.
    static uint32_t last = 0;
    if (!_task || (last && millis() - last < 30000UL)) return;
    last = millis();
    xTaskNotifyGive(_task);
}
int  otaProgress()     { return _progress; }
