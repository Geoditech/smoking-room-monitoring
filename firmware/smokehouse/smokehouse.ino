/*
 ======================================================================
  SISTEM MONITORING LINGKUNGAN RUANG PENGASAPAN IKAN AKTIF
  Firmware ESP32  |  PRD-SMLRPI-001 v2.0  +  CR-001 (toggle switch)
 ======================================================================
  Mengukur: Suhu (PT100 + MAX31865), RH (SHT20), CO (MICS-5524 / ADC).
  Mengirim: MQTT tiap 3-5 detik (dashboard real-time) dan
            webhook Google Apps Script tiap 1 menit (catatan per sesi).
  Kendali : SATU toggle switch.  ON = mulai sesi,  OFF = akhiri sesi.

  DAFTAR ISI (cari judul bagiannya dengan Ctrl+F)
    1. KONFIGURASI & KONSTANTA   pin, interval, kalibrasi (non-rahasia)
    2. TIPE DATA & VARIABEL BERSAMA
    3. KONFIGURASI RUNTIME       NVS + perintah Serial (FR-015)
    4. UTILITAS                  format angka, JSON
    5. ANTRIAN WEBHOOK (RAM)     terbatas, tanpa SD/RTC (FR-013)
    6. SENSOR                    suhu, RH, CO + filter + kalibrasi
    7. TOGGLE SWITCH             debounce level (FR-008)
    8. SESI                      mulai / berhenti / pencatatan per menit
    9. JARINGAN                  Wi-Fi, NTP, MQTT  (task di core 0)
   10. WEBHOOK                   kirim ke Apps Script (task di core 0)
   11. STATE MACHINE & setup()/loop()

  Library (Library Manager Arduino IDE):
    - Adafruit MAX31865 library   (beserta Adafruit BusIO)
    - PubSubClient (Nick O'Leary)
  Board: ESP32 Dev Module, ESP32 Arduino core 2.0.14+ atau 3.x.
  Data rahasia ada di secrets.h (salin dari secrets.example.h).
 ======================================================================
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <Preferences.h>
#include <SPI.h>
#include <Wire.h>
#include <time.h>
#include <esp_task_wdt.h>
#include <esp_system.h>
#include <Adafruit_MAX31865.h>

// Arduino IDE membuat "prototipe fungsi" otomatis dan menyisipkannya SEBELUM
// definisi struct di bawah. Tanpa deklarasi maju ini, fungsi yang memakai tipe
// tersebut (mis. qPush(QItem&)) menghasilkan error "has not been declared".
struct Config;
struct Snapshot;
struct CfgItem;
struct QItem;

#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #error "secrets.h tidak ditemukan. Salin secrets.example.h menjadi secrets.h lalu isi nilainya."
#endif

// Nilai cadangan bila ada makro yang tidak diisi di secrets.h
#ifndef MQTT_PORT
  #define MQTT_PORT 1883
#endif
#ifndef MQTT_USE_TLS
  #define MQTT_USE_TLS 0
#endif
#ifndef DEVICE_ID
  #define DEVICE_ID "smk-01"
#endif

// ======================================================================
// 1. KONFIGURASI & KONSTANTA  (bukan rahasia — aman diubah di sini)
// ======================================================================
#define FW_VERSION "1.0.0"

// ---- Pin (PRD HW-005, HW-006, HW-007, HW-010 + CR-001) ----
static const int PIN_SPI_SCK     = 18;   // MAX31865
static const int PIN_SPI_MISO    = 19;
static const int PIN_SPI_MOSI    = 23;
static const int PIN_MAX31865_CS = 5;
static const int PIN_I2C_SDA     = 21;   // SHT20
static const int PIN_I2C_SCL     = 22;
static const int PIN_CO_ADC      = 34;   // MICS-5524 AOUT via pembagi (ADC1)
static const int PIN_SWITCH      = 32;   // toggle: ON = terhubung ke GND (LOW)
// GPIO33 (bekas tombol STOP) tidak dipakai lagi.

// ---- PT100 / MAX31865 ----
static const float PT100_RNOMINAL = 100.0f;   // PT100
static const float PT100_RREF     = 430.0f;   // resistor referensi pada modul (umumnya 430 ohm)

// ---- Pembagi tegangan CO (PRD FR-003) ----
static const float DIVIDER_R1    = 1800.0f;   // ohm (sisi AOUT)
static const float DIVIDER_R2    = 3300.0f;   // ohm (sisi GND)
static const float DIVIDER_RATIO = DIVIDER_R2 / (DIVIDER_R1 + DIVIDER_R2);  // ~0,647
static const float CO_SUPPLY_MV  = 5000.0f;   // catu MICS-5524 (untuk mode kalibrasi POWER)

// ---- Filter & validasi ADC CO (FR-004) ----
static const int   CO_ADC_SAMPLES   = 32;     // sampel per siklus (median terpangkas)
static const int   CO_ADC_TRIM      = 4;      // buang 4 terendah + 4 tertinggi
static const float CO_FILTER_ALPHA  = 0.20f;  // EMA: 0 < alpha <= 1 (kecil = lebih halus)
static const float ADC_SAT_MV       = 3100.0f;// di atas ini ADC ESP32 mulai jenuh
static const float CO_MIN_VALID_MV  = 20.0f;  // di bawah ini: kabel putus / korsleting
static const uint32_t CO_WARMUP_MS  = 120000UL; // pemanasan sensor sejak boot (verifikasi ke datasheet)

// ---- Waktu ----
static const uint32_t SAMPLE_INTERVAL_MS   = 1000;    // siklus pembacaan sensor
static const uint32_t DEFAULT_PUBLISH_MS   = 4000;    // MQTT (FR-009: 3-5 detik)
static const uint32_t DEFAULT_LOG_MS       = 60000;   // webhook (FR-010: 1 menit)
static const uint32_t SWITCH_DEBOUNCE_MS   = 50;
static const uint32_t MQTT_READY_WAIT_MS   = 15000;   // tunggu broker sebelum siap menerima START
static const uint32_t STATUS_HEARTBEAT_MS  = 30000;
static const uint32_t WEBHOOK_TIMEOUT_MS   = 8000;
static const uint32_t WDT_TIMEOUT_S        = 30;
static const uint32_t I2C_CLOCK_HZ         = 50000;   // pelan agar kabel SHT20 panjang tetap stabil (RSK-010)
static const uint32_t RH_RETRY_MS          = 5000;    // jeda coba ulang bila SHT20 tidak merespons (mengurangi spam log I2C)

// ---- Antrian RAM untuk webhook saat Wi-Fi putus (OI-003) ----
#define QUEUE_CAPACITY 120    // 120 baris = 2 jam data per menit (~10 KB RAM)

// ---- NTP (WIB = UTC+7) ----
static const long  GMT_OFFSET_SEC = 7 * 3600;
static const char* NTP_1 = "pool.ntp.org";
static const char* NTP_2 = "time.google.com";
static const char* NTP_3 = "time.cloudflare.com";
static const time_t MIN_VALID_EPOCH = 1735689600;     // 2025-01-01: di bawah ini = waktu belum valid

// ---- Debug ----
#define PRINT_SAMPLES 1       // 1 = cetak satu baris pembacaan per detik di Serial Monitor

// ======================================================================
// 2. TIPE DATA & VARIABEL BERSAMA
// ======================================================================
// Program berjalan di 3 task:
//   loop()       (core 1) : sensor, switch, state machine, sesi
//   mqttTask     (core 0) : Wi-Fi, NTP, MQTT
//   webhookTask  (core 0) : kirim ke Google Apps Script
// Sehingga pengiriman jaringan yang lambat tidak mengganggu pembacaan sensor
// dan tidak membuat interval MQTT 3-5 detik meleset.

enum State : uint8_t {
  ST_INITIALIZING,
  ST_CONNECTING_WIFI,
  ST_SYNCING_TIME,
  ST_CONNECTING_MQTT,
  ST_WAIT_SWITCH_OFF,     // switch sudah ON saat boot: minta digeser OFF dulu
  ST_IDLE,                // siap, menunggu switch ON
  ST_SESSION_ACTIVE
};

static const char* stateName(State s) {
  switch (s) {
    case ST_INITIALIZING:     return "INITIALIZING";
    case ST_CONNECTING_WIFI:  return "CONNECTING_WIFI";
    case ST_SYNCING_TIME:     return "SYNCING_TIME";
    case ST_CONNECTING_MQTT:  return "CONNECTING_MQTT";
    case ST_WAIT_SWITCH_OFF:  return "WAIT_SWITCH_OFF";
    case ST_IDLE:             return "IDLE";
    case ST_SESSION_ACTIVE:   return "SESSION_ACTIVE";
  }
  return "?";
}

struct Config {
  char     wifiSsid[33];
  char     wifiPass[65];
  char     mqttHost[96];
  uint16_t mqttPort;
  uint8_t  mqttTls;
  char     mqttUser[48];
  char     mqttPass[64];
  char     webhookUrl[220];
  char     webhookToken[48];
  char     deviceId[24];
  uint32_t publishMs;
  uint32_t logMs;
  uint8_t  coMode;     // 0 = belum dikalibrasi (tanpa PPM), 1 = linear, 2 = pangkat
  float    coA, coB, coR0, coRl;
  float    tempOffset; // koreksi hasil uji akurasi (NFR-001)
  float    rhOffset;
};
static Config cfg;

struct Snapshot {          // nilai sensor terbaru (diisi loop, dibaca mqttTask)
  bool     valid;
  uint32_t ts;             // epoch UTC (detik), 0 = waktu belum valid
  float    temp, rh, coMv, coPinMv, coPpm;
  uint8_t  flt;            // bit0 suhu, bit1 RH, bit2 CO, bit3 CO masih pemanasan
  uint8_t  tflt;           // register fault MAX31865 (0 = normal)
};

static SemaphoreHandle_t g_mtx;                    // melindungi g_snap, g_sid, antrian
#define LOCK()   xSemaphoreTake(g_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(g_mtx)

static Snapshot g_snap;
static char     g_sid[20] = "";                    // ID sesi aktif
static volatile uint32_t g_sessionStartTs = 0;
static volatile bool     g_sessionActive  = false;
static volatile bool     g_wifiOk = false, g_mqttOk = false, g_timeOk = false;
static volatile State    g_state = ST_INITIALIZING;
static volatile uint32_t g_statusVer = 0;          // naik tiap status berubah -> dipublikasi
static volatile bool     g_publishNow = false;     // minta publish telemetry segera
static volatile uint32_t g_whRejected = 0;         // webhook ditolak permanen (mis. token salah)
static uint32_t g_stateEnteredAt = 0;

static char TOPIC_TELE[64], TOPIC_STATUS[64];

// ======================================================================
// 4. UTILITAS  (ditaruh sebelum bagian 3 agar bisa dipakai bersama)
// ======================================================================
// Angka -> teks JSON. NaN menjadi null (tidak pernah memalsukan nilai).
static const char* fnum(char* buf, size_t n, float v, uint8_t dec) {
  if (isnan(v) || isinf(v)) snprintf(buf, n, "null");
  else                      snprintf(buf, n, "%.*f", (int)dec, v);
  return buf;
}

static uint8_t crc8Sht(uint8_t b0, uint8_t b1) {   // CRC SHT2x: poly 0x31
  uint8_t crc = 0;
  uint8_t d[2] = { b0, b1 };
  for (int i = 0; i < 2; i++) {
    crc ^= d[i];
    for (int k = 0; k < 8; k++) crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
  }
  return crc;
}

static const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  return "POWERON";
    case ESP_RST_SW:       return "SW";
    case ESP_RST_PANIC:    return "PANIC";
    case ESP_RST_INT_WDT:  return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT:      return "WDT";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    default:               return "OTHER";
  }
}

static int queueDepth();   // didefinisikan di bagian 5

// ======================================================================
// 3. KONFIGURASI RUNTIME  (default dari secrets.h, bisa ditimpa via Serial)
// ======================================================================
enum CfgType : uint8_t { CT_STR, CT_U8, CT_U16, CT_U32, CT_FLOAT };
struct CfgItem { const char* key; CfgType type; void* ptr; uint16_t size; bool secret; };

static CfgItem CFG_TABLE[] = {
  { "wifi_ssid",     CT_STR,   cfg.wifiSsid,     sizeof(cfg.wifiSsid),     false },
  { "wifi_pass",     CT_STR,   cfg.wifiPass,     sizeof(cfg.wifiPass),     true  },
  { "mqtt_host",     CT_STR,   cfg.mqttHost,     sizeof(cfg.mqttHost),     false },
  { "mqtt_port",     CT_U16,   &cfg.mqttPort,    sizeof(cfg.mqttPort),     false },
  { "mqtt_tls",      CT_U8,    &cfg.mqttTls,     sizeof(cfg.mqttTls),      false },
  { "mqtt_user",     CT_STR,   cfg.mqttUser,     sizeof(cfg.mqttUser),     false },
  { "mqtt_pass",     CT_STR,   cfg.mqttPass,     sizeof(cfg.mqttPass),     true  },
  { "webhook_url",   CT_STR,   cfg.webhookUrl,   sizeof(cfg.webhookUrl),   true  },
  { "webhook_token", CT_STR,   cfg.webhookToken, sizeof(cfg.webhookToken), true  },
  { "device_id",     CT_STR,   cfg.deviceId,     sizeof(cfg.deviceId),     false },
  { "pub_ms",        CT_U32,   &cfg.publishMs,   sizeof(cfg.publishMs),    false },
  { "log_ms",        CT_U32,   &cfg.logMs,       sizeof(cfg.logMs),        false },
  { "co_mode",       CT_U8,    &cfg.coMode,      sizeof(cfg.coMode),       false },
  { "co_a",          CT_FLOAT, &cfg.coA,         sizeof(cfg.coA),          false },
  { "co_b",          CT_FLOAT, &cfg.coB,         sizeof(cfg.coB),          false },
  { "co_r0",         CT_FLOAT, &cfg.coR0,        sizeof(cfg.coR0),         false },
  { "co_rl",         CT_FLOAT, &cfg.coRl,        sizeof(cfg.coRl),         false },
  { "temp_off",      CT_FLOAT, &cfg.tempOffset,  sizeof(cfg.tempOffset),   false },
  { "rh_off",        CT_FLOAT, &cfg.rhOffset,    sizeof(cfg.rhOffset),     false },
};
static const int CFG_COUNT = sizeof(CFG_TABLE) / sizeof(CFG_TABLE[0]);

static void cfgDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  strlcpy(cfg.wifiSsid,     WIFI_SSID,     sizeof(cfg.wifiSsid));
  strlcpy(cfg.wifiPass,     WIFI_PASSWORD, sizeof(cfg.wifiPass));
  strlcpy(cfg.mqttHost,     MQTT_HOST,     sizeof(cfg.mqttHost));
  cfg.mqttPort = MQTT_PORT;
  cfg.mqttTls  = MQTT_USE_TLS ? 1 : 0;
  strlcpy(cfg.mqttUser,     MQTT_USER,     sizeof(cfg.mqttUser));
  strlcpy(cfg.mqttPass,     MQTT_PASS,     sizeof(cfg.mqttPass));
  strlcpy(cfg.webhookUrl,   WEBHOOK_URL,   sizeof(cfg.webhookUrl));
  strlcpy(cfg.webhookToken, WEBHOOK_TOKEN, sizeof(cfg.webhookToken));
  strlcpy(cfg.deviceId,     DEVICE_ID,     sizeof(cfg.deviceId));
  cfg.publishMs = DEFAULT_PUBLISH_MS;
  cfg.logMs     = DEFAULT_LOG_MS;
  cfg.coMode    = 0;          // BELUM dikalibrasi -> tidak ada nilai PPM (OI-002)
  cfg.coA = 0; cfg.coB = 0; cfg.coR0 = 0; cfg.coRl = 10000.0f;
  cfg.tempOffset = 0; cfg.rhOffset = 0;
}

static void cfgLoad() {
  cfgDefaults();
  Preferences p;
  if (!p.begin("smk", true)) {           // namespace belum ada = belum pernah ditimpa
    return;
  }
  for (int i = 0; i < CFG_COUNT; i++) {
    const CfgItem& it = CFG_TABLE[i];
    if (!p.isKey(it.key)) continue;
    switch (it.type) {
      case CT_STR:   p.getString(it.key, (char*)it.ptr, it.size); break;
      case CT_U8:    *(uint8_t*)it.ptr  = p.getUChar(it.key,  *(uint8_t*)it.ptr);  break;
      case CT_U16:   *(uint16_t*)it.ptr = p.getUShort(it.key, *(uint16_t*)it.ptr); break;
      case CT_U32:   *(uint32_t*)it.ptr = p.getUInt(it.key,   *(uint32_t*)it.ptr); break;
      case CT_FLOAT: *(float*)it.ptr    = p.getFloat(it.key,  *(float*)it.ptr);    break;
    }
  }
  p.end();
}

static const CfgItem* cfgFind(const char* key) {
  for (int i = 0; i < CFG_COUNT; i++) if (strcmp(CFG_TABLE[i].key, key) == 0) return &CFG_TABLE[i];
  return nullptr;
}

static bool cfgSet(const char* key, const char* val) {
  const CfgItem* it = cfgFind(key);
  if (!it) return false;
  Preferences p;
  p.begin("smk", false);
  switch (it->type) {
    case CT_STR:
      if (strlen(val) >= it->size) { p.end(); Serial.println("Nilai terlalu panjang."); return false; }
      p.putString(it->key, val);
      break;
    case CT_U8:    p.putUChar(it->key,  (uint8_t)strtoul(val, nullptr, 10));  break;
    case CT_U16:   p.putUShort(it->key, (uint16_t)strtoul(val, nullptr, 10)); break;
    case CT_U32:   p.putUInt(it->key,   (uint32_t)strtoul(val, nullptr, 10)); break;
    case CT_FLOAT: p.putFloat(it->key,  (float)atof(val));                    break;
  }
  p.end();
  return true;
}

static void cfgPrint() {
  Serial.println("---- Konfigurasi aktif (kata sandi disamarkan) ----");
  for (int i = 0; i < CFG_COUNT; i++) {
    const CfgItem& it = CFG_TABLE[i];
    Serial.printf("  %-14s = ", it.key);
    if (it.secret) { Serial.println(((char*)it.ptr)[0] || it.type != CT_STR ? "********" : "(kosong)"); continue; }
    switch (it.type) {
      case CT_STR:   Serial.println((const char*)it.ptr);                      break;
      case CT_U8:    Serial.println(*(uint8_t*)it.ptr);                        break;
      case CT_U16:   Serial.println(*(uint16_t*)it.ptr);                       break;
      case CT_U32:   Serial.println(*(uint32_t*)it.ptr);                       break;
      case CT_FLOAT: Serial.println(*(float*)it.ptr, 4);                       break;
    }
  }
}

static void buildTopics() {
  snprintf(TOPIC_TELE,   sizeof(TOPIC_TELE),   "smokehouse/%s/telemetry", cfg.deviceId);
  snprintf(TOPIC_STATUS, sizeof(TOPIC_STATUS), "smokehouse/%s/status",    cfg.deviceId);
}

// ======================================================================
// 5. ANTRIAN WEBHOOK (RAM, terbatas)
// ======================================================================
// Setiap kejadian (START, baris DATA tiap menit, STOP) masuk antrian ini dan
// dikirim berurutan oleh webhookTask. Bila Wi-Fi putus, antrian menampung.
// Bila penuh, baris DATA TERTUA dibuang lebih dulu; START/STOP dipertahankan.
enum Act : uint8_t { ACT_START, ACT_DATA, ACT_STOP };

struct QItem {
  uint32_t id;
  Act      act;
  uint8_t  offline;     // 1 = baris dibuat saat Wi-Fi putus
  uint8_t  flt, tflt;
  uint32_t seq, ts;
  float    temp, rh, coMv, coPpm;
  char     sid[20];
  char     sheet[24];   // hanya untuk START
};

static QItem    g_q[QUEUE_CAPACITY];
static int      g_qHead = 0, g_qCount = 0;
static uint32_t g_qNextId = 1;
static uint32_t g_qDropped = 0;

static void qPush(QItem& it) {
  LOCK();
  it.id = g_qNextId++;
  if (g_qCount >= QUEUE_CAPACITY) {
    int victim = 0;                                    // default: yang tertua
    for (int i = 0; i < g_qCount; i++) {
      if (g_q[(g_qHead + i) % QUEUE_CAPACITY].act == ACT_DATA) { victim = i; break; }
    }
    for (int j = victim; j < g_qCount - 1; j++)
      g_q[(g_qHead + j) % QUEUE_CAPACITY] = g_q[(g_qHead + j + 1) % QUEUE_CAPACITY];
    g_qCount--;
    g_qDropped++;
  }
  g_q[(g_qHead + g_qCount) % QUEUE_CAPACITY] = it;
  g_qCount++;
  UNLOCK();
}

static bool qPeek(QItem* out) {
  bool ok = false;
  LOCK();
  if (g_qCount > 0) { *out = g_q[g_qHead]; ok = true; }
  UNLOCK();
  return ok;
}

static void qPopIfId(uint32_t id) {
  LOCK();
  if (g_qCount > 0 && g_q[g_qHead].id == id) { g_qHead = (g_qHead + 1) % QUEUE_CAPACITY; g_qCount--; }
  UNLOCK();
}

static int queueDepth() { return g_qCount; }

// ======================================================================
// 6. SENSOR
// ======================================================================
// ---- 6a. Suhu: PT100 3-wire via MAX31865 (SPI) -----------------------
static Adafruit_MAX31865 g_rtd(PIN_MAX31865_CS);
static float   s_temp = NAN;
static uint8_t s_tFault = 0;

static void tempInit() {
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI, PIN_MAX31865_CS);
  g_rtd.begin(MAX31865_3WIRE);      // jumper solder modul HARUS diset ke 3-wire
}

static void tempRead() {
  uint16_t raw = g_rtd.readRTD();
  uint8_t  f   = g_rtd.readFault();
  if (f) {                          // fault MAX31865 (kabel putus, korsleting, dll.)
    g_rtd.clearFault();
    s_tFault = f; s_temp = NAN;
    return;
  }
  float t = g_rtd.calculateTemperature(raw, PT100_RNOMINAL, PT100_RREF);
  if (isnan(t) || t < -50.0f || t > 400.0f) { s_tFault = 0x80; s_temp = NAN; return; }  // 0x80 = di luar rentang
  s_tFault = 0;
  s_temp = t + cfg.tempOffset;
}

// ---- 6b. RH: SHT20 langsung via I2C (tanpa library, alamat 0x40) -----
static const uint8_t SHT_ADDR = 0x40;
static float    s_rh = NAN;
static bool     s_rhFault = true;          // true sampai ada pembacaan sukses
static bool     s_rhPending = false;
static uint8_t  s_rhFails = 0;
static uint32_t s_rhNextTryAt = 0;         // saat sensor dinyatakan fault: coba lagi tiap RH_RETRY_MS

static void rhSoftReset() {
  Wire.beginTransmission(SHT_ADDR); Wire.write((uint8_t)0xFE); Wire.endTransmission();
  delay(15);
}

static void rhInit() {
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  rhSoftReset();
}

static void rhFail() {
  if (s_rhFails < 250) s_rhFails++;
  if (s_rhFails >= 3) {
    s_rh = NAN; s_rhFault = true;
    s_rhNextTryAt = millis() + RH_RETRY_MS;       // jangan tanya terus tiap detik
  }
  if (s_rhFails % 10 == 0) rhSoftReset();           // coba pulihkan bus/sensor
}

// Dipanggil sekali per siklus: ambil hasil konversi siklus lalu, picu yang baru.
static void rhRead() {
  if (s_rhFault && s_rhFails >= 3 && (int32_t)(millis() - s_rhNextTryAt) < 0) return;   // masih menunggu jeda coba ulang
  if (s_rhPending) {
    s_rhPending = false;
    if (Wire.requestFrom(SHT_ADDR, (uint8_t)3) == 3) {
      uint8_t m = Wire.read(), l = Wire.read(), c = Wire.read();
      if (crc8Sht(m, l) == c) {
        uint16_t raw = (((uint16_t)m << 8) | l) & 0xFFFC;
        float rh = -6.0f + 125.0f * (float)raw / 65536.0f;
        if (rh < 0)   rh = 0;
        if (rh > 100) rh = 100;
        s_rh = rh + cfg.rhOffset;
        s_rhFault = false; s_rhFails = 0;
      } else rhFail();
    } else rhFail();
  }
  Wire.beginTransmission(SHT_ADDR);
  Wire.write((uint8_t)0xF5);                         // trigger RH, no-hold master
  if (Wire.endTransmission() == 0) s_rhPending = true; else rhFail();
}

// ---- 6c. CO: MICS-5524 via ADC1 GPIO34 -------------------------------
static float   s_coPin = NAN;      // tegangan di pin ESP32 (mV), setelah filter
static float   s_coMv  = NAN;      // tegangan AOUT asli (mV), setelah koreksi pembagi
static float   s_coPpm = NAN;      // NaN selama belum dikalibrasi
static bool    s_coInit = false;
static uint8_t s_coFault = 0;

static void coInit() {
  analogSetPinAttenuation(PIN_CO_ADC, ADC_11db);     // rentang ~0-3,1 V
  pinMode(PIN_CO_ADC, INPUT);
}

// Lapisan kalibrasi (OI-002). Mode 0 = tidak mengarang nilai PPM.
static float coConvert(float sensorMv) {
  if (isnan(sensorMv)) return NAN;
  switch (cfg.coMode) {
    case 1: {                                        // linear: ppm = A * mV + B
      float v = cfg.coA * sensorMv + cfg.coB;
      return v < 0 ? 0 : v;
    }
    case 2: {                                        // pangkat: ppm = A * (Rs/R0)^B
      if (cfg.coR0 <= 0 || cfg.coRl <= 0 || sensorMv <= 1.0f) return NAN;
      float rs = cfg.coRl * (CO_SUPPLY_MV / sensorMv - 1.0f);   // Rs = RL*(Vcc/Vout - 1)
      if (rs <= 0) return NAN;
      return cfg.coA * powf(rs / cfg.coR0, cfg.coB);
    }
    default: return NAN;
  }
}

static void coRead() {
  uint16_t buf[CO_ADC_SAMPLES];
  for (int i = 0; i < CO_ADC_SAMPLES; i++) {
    buf[i] = (uint16_t)analogReadMilliVolts(PIN_CO_ADC);
    delayMicroseconds(300);
  }
  for (int i = 1; i < CO_ADC_SAMPLES; i++) {                       // urutkan (insertion sort)
    uint16_t v = buf[i]; int j = i - 1;
    while (j >= 0 && buf[j] > v) { buf[j + 1] = buf[j]; j--; }
    buf[j + 1] = v;
  }
  uint32_t sum = 0; int cnt = 0;
  for (int i = CO_ADC_TRIM; i < CO_ADC_SAMPLES - CO_ADC_TRIM; i++) { sum += buf[i]; cnt++; }
  float pinMv = (float)sum / (float)cnt;

  if (!s_coInit) { s_coPin = pinMv; s_coInit = true; }             // EMA (FR-004)
  else           { s_coPin += CO_FILTER_ALPHA * (pinMv - s_coPin); }

  s_coFault = 0;
  if (s_coPin > ADC_SAT_MV)      s_coFault |= 1;                   // mendekati jenuh
  if (s_coPin < CO_MIN_VALID_MV) s_coFault |= 2;                   // kabel putus / short
  s_coMv  = s_coPin / DIVIDER_RATIO;                               // koreksi pembagi (≈ /0,647)
  s_coPpm = coConvert(s_coMv);
}

// ---- 6d. Satu siklus pembacaan -> snapshot ---------------------------
static void sampleAll() {
  tempRead();
  rhRead();
  coRead();

  Snapshot s;
  s.valid = true;
  time_t now = time(nullptr);
  s.ts = (now > MIN_VALID_EPOCH) ? (uint32_t)now : 0;
  s.temp = s_temp; s.rh = s_rh; s.coMv = s_coMv; s.coPinMv = s_coPin; s.coPpm = s_coPpm;
  s.tflt = s_tFault;
  s.flt  = (s_tFault ? 1 : 0) | (s_rhFault ? 2 : 0) | (s_coFault ? 4 : 0) |
           ((millis() < CO_WARMUP_MS) ? 8 : 0);
  LOCK(); g_snap = s; UNLOCK();

#if PRINT_SAMPLES
  Serial.printf("[%s] T=%.2fC RH=%.1f%% CO: pin=%.0fmV aout=%.0fmV ppm=%s flt=%u q=%d\n",
                stateName(g_state),
                isnan(s.temp) ? -999.0 : s.temp, isnan(s.rh) ? -999.0 : s.rh,
                isnan(s.coPinMv) ? -1.0 : s.coPinMv, isnan(s.coMv) ? -1.0 : s.coMv,
                isnan(s.coPpm) ? "n/a" : "ok", s.flt, queueDepth());
#endif
}

// ======================================================================
// 7. TOGGLE SWITCH  (CR-001: pengganti tombol START/STOP)
// ======================================================================
// Switch SPST antara GPIO32 dan GND. ON = LOW. Dibaca sebagai LEVEL, dengan
// debounce: level baru dianggap sah setelah stabil SWITCH_DEBOUNCE_MS.
static bool     s_swRaw = false, s_swOn = false;
static uint32_t s_swChangedAt = 0;
static bool     s_bootSwitchOn = false;   // true = switch sudah ON saat boot, belum pernah OFF

static void switchInit() {
  pinMode(PIN_SWITCH, INPUT_PULLUP);
  delay(100);                                       // beri waktu pull-up stabil
  s_swRaw = s_swOn = (digitalRead(PIN_SWITCH) == LOW);
  s_bootSwitchOn = s_swOn;
}

static void switchUpdate() {
  bool raw = (digitalRead(PIN_SWITCH) == LOW);
  uint32_t now = millis();
  if (raw != s_swRaw) { s_swRaw = raw; s_swChangedAt = now; return; }
  if (raw != s_swOn && (now - s_swChangedAt) >= SWITCH_DEBOUNCE_MS) {
    s_swOn = raw;
    Serial.printf("Switch -> %s\n", s_swOn ? "ON" : "OFF");
    if (!s_swOn) s_bootSwitchOn = false;
  }
}

// ======================================================================
// 8. SESI
// ======================================================================
static uint32_t s_seq = 0;            // nomor baris data dalam sesi
static uint32_t s_nextLogAt = 0;

static void setState(State s) {
  if (g_state == s) return;
  g_state = s;
  g_stateEnteredAt = millis();
  g_statusVer++;
  Serial.printf(">> STATE: %s\n", stateName(s));
}

static void fillFromSnapshot(QItem& it) {
  Snapshot s;
  LOCK(); s = g_snap; UNLOCK();
  it.temp = s.temp; it.rh = s.rh; it.coMv = s.coMv; it.coPpm = s.coPpm;
  it.flt = s.flt; it.tflt = s.tflt;
}

static void startSession() {
  time_t now = time(nullptr);
  struct tm tmv; localtime_r(&now, &tmv);
  QItem it; memset(&it, 0, sizeof(it));
  it.act = ACT_START; it.seq = 0; it.ts = (uint32_t)now;
  strftime(it.sid,   sizeof(it.sid),   "%Y%m%d-%H%M%S",    &tmv);
  strftime(it.sheet, sizeof(it.sheet), "%Y-%m-%d %H.%M.%S", &tmv);   // nama tab Sheets

  LOCK(); strlcpy(g_sid, it.sid, sizeof(g_sid)); UNLOCK();
  s_seq = 0;
  g_sessionStartTs = (uint32_t)now;
  qPush(it);                                  // webhook START
  s_nextLogAt = millis() + cfg.logMs;         // baris data pertama = 1 menit setelah START
  g_sessionActive = true;
  g_publishNow = true;                        // data pertama segera muncul di dashboard
  setState(ST_SESSION_ACTIVE);
  Serial.printf("SESI MULAI  sid=%s  sheet=\"%s\"\n", it.sid, it.sheet);
}

static void stopSession() {
  QItem it; memset(&it, 0, sizeof(it));
  it.act = ACT_STOP; it.seq = ++s_seq; it.ts = (uint32_t)time(nullptr);
  LOCK(); strlcpy(it.sid, g_sid, sizeof(it.sid)); UNLOCK();
  qPush(it);                                  // webhook STOP (catat waktu selesai)
  g_sessionActive = false;                    // telemetry MQTT berhenti
  Serial.printf("SESI SELESAI sid=%s  baris=%lu\n", it.sid, (unsigned long)(s_seq - 1));
  setState(ST_IDLE);
}

static void logTick() {                       // satu baris data tiap logMs
  if ((int32_t)(millis() - s_nextLogAt) < 0) return;
  s_nextLogAt += cfg.logMs;
  if ((int32_t)(millis() - s_nextLogAt) >= 0) s_nextLogAt = millis() + cfg.logMs;   // kejar-ketinggalan: jangan menumpuk

  QItem it; memset(&it, 0, sizeof(it));
  it.act = ACT_DATA; it.seq = ++s_seq; it.ts = (uint32_t)time(nullptr);
  it.offline = g_wifiOk ? 0 : 1;
  LOCK(); strlcpy(it.sid, g_sid, sizeof(it.sid)); UNLOCK();
  fillFromSnapshot(it);
  qPush(it);
}

// ======================================================================
// 9. JARINGAN: Wi-Fi, NTP, MQTT   (berjalan di mqttTask, core 0)
// ======================================================================
static WiFiClient       g_plainClient;
static WiFiClientSecure g_secureClient;
static PubSubClient     g_mqtt;
static uint32_t         g_teleSeq = 0;

static void wifiMaintain() {
  static uint32_t lastTry = 0;
  static uint8_t  tries = 0;
  bool ok = (WiFi.status() == WL_CONNECTED);
  g_wifiOk = ok;
  if (ok) { tries = 0; return; }
  uint32_t backoff = 5000UL * (1 + (tries < 6 ? tries : 6));       // 5 s ... 35 s
  if (lastTry == 0 || (millis() - lastTry) >= backoff) {
    lastTry = millis();
    WiFi.disconnect(false, false);
    WiFi.begin(cfg.wifiSsid, cfg.wifiPass);                        // tidak memblokir
    if (tries < 250) tries++;
    Serial.printf("Wi-Fi: mencoba menyambung ke \"%s\" (percobaan %u)\n", cfg.wifiSsid, tries);
  }
}

static void timeMaintain() {
  static bool configured = false;
  static uint32_t lastCfg = 0;
  if (!g_wifiOk) return;
  if (!configured || (!g_timeOk && (millis() - lastCfg) > 30000)) {
    configTime(GMT_OFFSET_SEC, 0, NTP_1, NTP_2, NTP_3);
    configured = true; lastCfg = millis();
    Serial.println("NTP: sinkronisasi waktu dimulai");
  }
  if (!g_timeOk && time(nullptr) > MIN_VALID_EPOCH) {
    g_timeOk = true;
    struct tm t; time_t n = time(nullptr); localtime_r(&n, &t);
    Serial.printf("NTP: waktu valid %04d-%02d-%02d %02d:%02d:%02d WIB\n",
                  t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  }
}

static void publishStatus(bool force) {
  static uint32_t lastPub = 0;
  static uint32_t seenVer = 0xFFFFFFFF;
  uint32_t now = millis();
  if (!force && seenVer == g_statusVer && (now - lastPub) < STATUS_HEARTBEAT_MS) return;
  seenVer = g_statusVer; lastPub = now;

  char sid[20];
  LOCK(); strlcpy(sid, g_sid, sizeof(sid)); UNLOCK();
  bool active = g_sessionActive;
  char buf[400];
  int n = snprintf(buf, sizeof(buf),
    "{\"dev\":\"%s\",\"state\":\"%s\",\"fw\":\"%s\",\"sid\":%s%s%s,\"start_ts\":%lu,"
    "\"ts\":%lu,\"rssi\":%d,\"qd\":%d,\"wh_rej\":%lu,\"rst\":\"%s\"}",
    cfg.deviceId, stateName(g_state), FW_VERSION,
    active ? "\"" : "", active ? sid : "null", active ? "\"" : "",
    active ? (unsigned long)g_sessionStartTs : 0UL,
    (unsigned long)time(nullptr), WiFi.RSSI(), queueDepth(), (unsigned long)g_whRejected, resetReasonName());
  if (n > 0) g_mqtt.publish(TOPIC_STATUS, (const uint8_t*)buf, (unsigned int)n, true);   // retained
}

static void publishTelemetry() {
  Snapshot s; char sid[20];
  LOCK(); s = g_snap; strlcpy(sid, g_sid, sizeof(sid)); UNLOCK();
  if (!g_sessionActive || !s.valid) return;

  char a[16], b[16], c[16], d[16], e[16];
  char buf[420];
  int n = snprintf(buf, sizeof(buf),
    "{\"dev\":\"%s\",\"sid\":\"%s\",\"seq\":%lu,\"ts\":%lu,\"temp_c\":%s,\"rh\":%s,"
    "\"co_mv\":%s,\"co_pin_mv\":%s,\"co_ppm\":%s,\"co_cal\":%d,\"flt\":%u,\"t_flt\":%u,"
    "\"rssi\":%d,\"qd\":%d}",
    cfg.deviceId, sid, (unsigned long)(++g_teleSeq), (unsigned long)s.ts,
    fnum(a, sizeof(a), s.temp, 2), fnum(b, sizeof(b), s.rh, 1),
    fnum(c, sizeof(c), s.coMv, 0), fnum(d, sizeof(d), s.coPinMv, 0), fnum(e, sizeof(e), s.coPpm, 1),
    cfg.coMode != 0 ? 1 : 0, s.flt, s.tflt, WiFi.RSSI(), queueDepth());
  if (n > 0) g_mqtt.publish(TOPIC_TELE, (const uint8_t*)buf, (unsigned int)n, false);
}

static void mqttMaintain() {
  static uint32_t lastTry = 0;
  static uint8_t  fails = 0;

  if (!g_wifiOk || cfg.mqttHost[0] == 0) {
    if (g_mqtt.connected()) g_mqtt.disconnect();
    g_mqttOk = false;
    return;
  }
  if (g_mqtt.connected()) { g_mqtt.loop(); g_mqttOk = true; return; }

  if (g_mqttOk) { g_mqttOk = false; g_statusVer++; Serial.println("MQTT: terputus"); }
  uint32_t backoff = 3000UL << (fails < 4 ? fails : 4);            // 3 s ... 48 s
  if (lastTry != 0 && (millis() - lastTry) < backoff) return;
  lastTry = millis();

  char lwt[96];
  snprintf(lwt, sizeof(lwt), "{\"dev\":\"%s\",\"state\":\"OFFLINE\"}", cfg.deviceId);
  bool ok;
  if (cfg.mqttUser[0])
    ok = g_mqtt.connect(cfg.deviceId, cfg.mqttUser, cfg.mqttPass, TOPIC_STATUS, 1, true, lwt);
  else
    ok = g_mqtt.connect(cfg.deviceId, TOPIC_STATUS, 1, true, lwt);

  if (ok) {
    fails = 0; g_mqttOk = true;
    Serial.println("MQTT: tersambung");
    publishStatus(true);
  } else {
    if (fails < 250) fails++;
    Serial.printf("MQTT: gagal (rc=%d), coba lagi nanti\n", g_mqtt.state());
  }
}

static void mqttTask(void*) {
  esp_task_wdt_add(NULL);
  uint32_t lastPub = 0;
  for (;;) {
    esp_task_wdt_reset();
    wifiMaintain();
    timeMaintain();
    mqttMaintain();

    if (g_mqttOk) {
      uint32_t now = millis();
      if (g_sessionActive && (g_publishNow || (now - lastPub) >= cfg.publishMs)) {
        g_publishNow = false;
        lastPub = now;
        publishTelemetry();
      }
      publishStatus(false);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ======================================================================
// 10. WEBHOOK  (webhookTask, core 0)
// ======================================================================
// Kirim ke Google Apps Script lewat HTTP POST (JSON). Apps Script membalas
// {"ok":true,...}. Respons lain = coba lagi (dengan jeda bertambah).
// Apps Script menolak duplikat berdasarkan sid+seq, jadi aman mengirim ulang.
static WiFiClientSecure g_whClient;

static int buildWebhookBody(const QItem& it, char* buf, size_t n) {
  const char* act = (it.act == ACT_START) ? "start" : (it.act == ACT_DATA) ? "data" : "stop";
  int len = snprintf(buf, n,
    "{\"token\":\"%s\",\"action\":\"%s\",\"dev\":\"%s\",\"sid\":\"%s\",\"seq\":%lu,\"ts\":%lu",
    cfg.webhookToken, act, cfg.deviceId, it.sid, (unsigned long)it.seq, (unsigned long)it.ts);
  if (it.act == ACT_START) {
    len += snprintf(buf + len, n - len, ",\"sheet\":\"%s\"", it.sheet);
  } else if (it.act == ACT_DATA) {
    char a[16], b[16], c[16], d[16];
    len += snprintf(buf + len, n - len,
      ",\"temp_c\":%s,\"rh\":%s,\"co_mv\":%s,\"co_ppm\":%s,\"flt\":%u,\"offline\":%u",
      fnum(a, sizeof(a), it.temp, 2), fnum(b, sizeof(b), it.rh, 1),
      fnum(c, sizeof(c), it.coMv, 0),  fnum(d, sizeof(d), it.coPpm, 1),
      it.flt, it.offline);
  }
  len += snprintf(buf + len, n - len, "}");
  return len;
}

// return: 0 = gagal (coba lagi), 1 = sukses, 2 = ditolak permanen (jangan diulang),
//         3 = skrip SUDAH dijalankan Google tetapi hasilnya tidak terbaca
//
// Cara kerja Apps Script: permintaan POST langsung DIJALANKAN, lalu Google membalas
// dengan redirect 302 ke script.googleusercontent.com yang berisi hasilnya.
// Redirect itu ditangani MANUAL di sini (tidak mengandalkan auto-follow HTTPClient,
// yang tidak konsisten antar versi core ESP32): POST -> baca header Location -> GET hasil.
static int sendWebhook(const QItem& it) {
  char body[512];
  int len = buildWebhookBody(it, body, sizeof(body));
  if (len <= 0 || len >= (int)sizeof(body)) return 2;
  const char* what = it.act == ACT_START ? "START" : it.act == ACT_DATA ? "DATA" : "STOP";

  // Koneksi terenkripsi TLS, tetapi sertifikat server Google belum diverifikasi (keputusan sadar,
  // lihat docs/security.md bagian 'Keterbatasan TLS'). Token + anti-duplikat membatasi dampaknya.
  g_whClient.setInsecure();
  String resp, loc;
  int code;
  {
    HTTPClient http;
    if (!http.begin(g_whClient, cfg.webhookUrl)) return 0;
    http.setFollowRedirects(HTTPC_DISABLE_FOLLOW_REDIRECTS);
    http.setTimeout(WEBHOOK_TIMEOUT_MS);
    http.setReuse(false);
    const char* keys[] = { "Location" };
    http.collectHeaders(keys, 1);
    http.addHeader("Content-Type", "application/json");
    code = http.POST((uint8_t*)body, (size_t)len);
    if (code > 0) {
      loc = http.header("Location");
      if (code == 200) resp = http.getString();
    }
    http.end();
  }
  Serial.printf("Webhook %s seq=%lu -> HTTP %d\n", what, (unsigned long)it.seq, code);

  bool viaRedirect = false;
  if (code >= 300 && code < 400) {
    if (loc.length() == 0) { Serial.println("Webhook: redirect tanpa Location"); return 0; }
    if (!loc.startsWith("https://script.googleusercontent.com/") && !loc.startsWith("https://script.google.com/")) {
      // Dialihkan ke halaman login Google = Web App belum diatur "Anyone"; skrip BELUM dijalankan.
      Serial.println("Webhook: dialihkan ke alamat asing. Pastikan deployment: Execute as Me, Who has access = Anyone.");
      return 0;
    }
    viaRedirect = true;                       // skrip sudah berjalan; ambil hasilnya
    HTTPClient h2;
    if (!h2.begin(g_whClient, loc)) return 3;
    h2.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    h2.setTimeout(WEBHOOK_TIMEOUT_MS);
    h2.setReuse(false);
    code = h2.GET();
    resp = (code > 0) ? h2.getString() : String("");
    h2.end();
    Serial.printf("Webhook %s seq=%lu hasil -> HTTP %d\n", what, (unsigned long)it.seq, code);
  }

  if (code == 200) {
    if (resp.indexOf("\"ok\":true") >= 0)         return 1;
    if (resp.indexOf("\"retry\":false") >= 0) { Serial.println("Webhook ditolak: " + resp); return 2; }
    Serial.println("Webhook: balasan tak dikenal: " + resp.substring(0, 160));
  }
  return viaRedirect ? 3 : 0;
}

static void webhookTask(void*) {
  esp_task_wdt_add(NULL);
  uint8_t  fails = 0, unread = 0;
  uint32_t retryAt = 0;
  for (;;) {
    esp_task_wdt_reset();
    QItem it;
    if (!g_wifiOk || cfg.webhookUrl[0] == 0 || !qPeek(&it) || (int32_t)(millis() - retryAt) < 0) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    int r = sendWebhook(it);
    if (r == 1)      { qPopIfId(it.id); fails = 0; unread = 0; }
    else if (r == 2) { qPopIfId(it.id); fails = 0; unread = 0; g_whRejected++; g_statusVer++; }
    else if (r == 3 && ++unread >= 3) {
      // Skrip terbukti berjalan (redirect ke googleusercontent), tetapi hasilnya tiga kali tak terbaca.
      // Karena Apps Script menolak duplikat, aman dianggap terkirim agar antrian tidak macet selamanya.
      Serial.println("Webhook: hasil tak terbaca 3x, dianggap terkirim.");
      qPopIfId(it.id); fails = 0; unread = 0;
    }
    else {
      if (fails < 250) fails++;
      retryAt = millis() + (3000UL << (fails < 4 ? fails : 4));   // 6 s ... 48 s
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ======================================================================
// Perintah Serial (FR-015): ubah konfigurasi tanpa kompilasi ulang
// ======================================================================
static void cliHelp() {
  Serial.println(
    "\nPerintah Serial Monitor (115200 baud, akhiri dengan Enter):\n"
    "  help                  tampilkan bantuan\n"
    "  show                  tampilkan konfigurasi aktif\n"
    "  status                tampilkan status & pembacaan terakhir\n"
    "  set <kunci> <nilai>   ubah konfigurasi (disimpan permanen), contoh:\n"
    "                          set wifi_ssid WifiBaru\n"
    "                          set pub_ms 3000\n"
    "                          set co_mode 1   |  set co_a 0.05  |  set co_b -2\n"
    "  unset <kunci>         kembali ke nilai default dari secrets.h\n"
    "  reset                 hapus SEMUA penimpaan konfigurasi\n"
    "  reboot                mulai ulang (perubahan berlaku setelah reboot)\n"
    "Kunci: wifi_ssid wifi_pass mqtt_host mqtt_port mqtt_tls mqtt_user mqtt_pass\n"
    "       webhook_url webhook_token device_id pub_ms log_ms\n"
    "       co_mode co_a co_b co_r0 co_rl temp_off rh_off");
}

static void cliExec(char* line) {
  while (*line == ' ') line++;
  if (!*line) return;
  char* cmd = strtok(line, " ");
  if (!cmd) return;

  if (!strcmp(cmd, "help"))        { cliHelp(); }
  else if (!strcmp(cmd, "show"))   { cfgPrint(); }
  else if (!strcmp(cmd, "status")) {
    Snapshot s; LOCK(); s = g_snap; UNLOCK();
    Serial.printf("state=%s wifi=%d mqtt=%d time=%d session=%d queue=%d dropped=%lu rejected=%lu\n",
                  stateName(g_state), g_wifiOk, g_mqttOk, g_timeOk, g_sessionActive,
                  queueDepth(), (unsigned long)g_qDropped, (unsigned long)g_whRejected);
    Serial.printf("T=%.2f RH=%.1f CO_pin=%.0fmV CO_aout=%.0fmV flt=%u  heap=%u\n",
                  s.temp, s.rh, s.coPinMv, s.coMv, s.flt, (unsigned)ESP.getFreeHeap());
  }
  else if (!strcmp(cmd, "set")) {
    char* key = strtok(nullptr, " ");
    char* val = key ? key + strlen(key) + 1 : nullptr;       // sisa baris = nilai (boleh berspasi)
    if (!key || !val || !*val) { Serial.println("Format: set <kunci> <nilai>"); return; }
    if (cfgSet(key, val)) Serial.println("OK. Ketik 'reboot' agar berlaku.");
    else Serial.println("Kunci tidak dikenal / nilai tidak valid. Ketik 'help'.");
  }
  else if (!strcmp(cmd, "unset")) {
    char* key = strtok(nullptr, " ");
    if (!key || !cfgFind(key)) { Serial.println("Kunci tidak dikenal."); return; }
    Preferences p; p.begin("smk", false); p.remove(key); p.end();
    Serial.println("OK. Ketik 'reboot' agar berlaku.");
  }
  else if (!strcmp(cmd, "reset"))  {
    Preferences p; p.begin("smk", false); p.clear(); p.end();
    Serial.println("Semua penimpaan dihapus. Ketik 'reboot'.");
  }
  else if (!strcmp(cmd, "reboot")) { Serial.println("Restart..."); delay(200); ESP.restart(); }
  else Serial.println("Perintah tidak dikenal. Ketik 'help'.");
}

static void cliPoll() {
  static char line[320];
  static size_t len = 0;
  while (Serial.available()) {
    char ch = (char)Serial.read();
    if (ch == '\r') continue;
    if (ch == '\n') { line[len] = 0; len = 0; cliExec(line); continue; }
    if (len < sizeof(line) - 1) line[len++] = ch;
  }
}

// ======================================================================
// 11. STATE MACHINE  &  setup() / loop()
// ======================================================================
//  INITIALIZING -> CONNECTING_WIFI -> SYNCING_TIME -> CONNECTING_MQTT -> IDLE
//  IDLE --(switch ON)--> SESSION_ACTIVE --(switch OFF)--> IDLE
//  Jika switch sudah ON saat boot: IDLE ditahan di WAIT_SWITCH_OFF sampai
//  switch digeser OFF (mencegah sesi baru tak sengaja setelah listrik padam).
//  Wi-Fi putus saat SESSION_ACTIVE: sesi TETAP berjalan, data masuk antrian.
//  Wi-Fi putus saat IDLE: kembali ke CONNECTING_WIFI.
static void runStateMachine() {
  switch (g_state) {
    case ST_INITIALIZING:
      setState(ST_CONNECTING_WIFI);
      break;

    case ST_CONNECTING_WIFI:
      if (g_wifiOk) setState(ST_SYNCING_TIME);
      break;

    case ST_SYNCING_TIME:
      if (!g_wifiOk) setState(ST_CONNECTING_WIFI);
      else if (g_timeOk) setState(ST_CONNECTING_MQTT);
      break;

    case ST_CONNECTING_MQTT:
      if (!g_wifiOk) setState(ST_CONNECTING_WIFI);
      else if (g_mqttOk || (millis() - g_stateEnteredAt) > MQTT_READY_WAIT_MS)
        setState(s_bootSwitchOn ? ST_WAIT_SWITCH_OFF : ST_IDLE);   // broker lambat tidak menghalangi pencatatan
      break;

    case ST_WAIT_SWITCH_OFF:
      if (!s_bootSwitchOn) setState(ST_IDLE);
      break;

    case ST_IDLE:
      if (!g_wifiOk)        { setState(ST_CONNECTING_WIFI); break; }
      if (!g_timeOk)        { setState(ST_SYNCING_TIME);    break; }
      if (s_bootSwitchOn)   { setState(ST_WAIT_SWITCH_OFF); break; }
      if (s_swOn) startSession();                                  // switch ON = START
      break;

    case ST_SESSION_ACTIVE:
      if (!s_swOn) { stopSession(); break; }                       // switch OFF = STOP
      logTick();
      break;
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n=== Monitoring Ruang Pengasapan Ikan  fw %s ===\n", FW_VERSION);
  Serial.printf("Reset terakhir: %s\n", resetReasonName());

  g_mtx = xSemaphoreCreateMutex();
  cfgLoad();
  buildTopics();
  if (cfg.publishMs < 500) cfg.publishMs = 500;
  if (cfg.logMs < 5000)    cfg.logMs = 5000;
  if (cfg.publishMs < 3000 || cfg.publishMs > 5000)
    Serial.println("PERINGATAN: pub_ms di luar 3000-5000 ms (rentang PRD FR-009).");
  if (cfg.coMode == 0)
    Serial.println("CO: belum dikalibrasi -> dikirim sebagai mV, tanpa nilai PPM (OI-002).");

  switchInit();
  tempInit();
  rhInit();
  coInit();

  // Watchdog: reset otomatis bila program macet (NFR-006)
#if defined(ESP_ARDUINO_VERSION_MAJOR) && (ESP_ARDUINO_VERSION_MAJOR >= 3)
  esp_task_wdt_config_t twdt;
  twdt.timeout_ms = WDT_TIMEOUT_S * 1000;
  twdt.idle_core_mask = 0;
  twdt.trigger_panic = true;
  esp_task_wdt_reconfigure(&twdt);
#else
  esp_task_wdt_init(WDT_TIMEOUT_S, true);
#endif
  esp_task_wdt_add(NULL);

  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);

  if (cfg.mqttTls) {
#ifdef MQTT_CA_CERT
    g_secureClient.setCACert(MQTT_CA_CERT);
#else
    g_secureClient.setInsecure();
#endif
    g_mqtt.setClient(g_secureClient);
  } else {
    g_mqtt.setClient(g_plainClient);
  }
  g_mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
  g_mqtt.setKeepAlive(30);
  g_mqtt.setSocketTimeout(8);      // TLS (EMQX Serverless) butuh waktu handshake lebih lama
  g_mqtt.setBufferSize(512);

  xTaskCreatePinnedToCore(mqttTask,    "mqtt",    8192,  NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(webhookTask, "webhook", 10240, NULL, 1, NULL, 0);

  Serial.println("Ketik 'help' untuk mengubah konfigurasi lewat Serial Monitor.");
  Serial.printf("Topik MQTT: %s | %s\n", TOPIC_TELE, TOPIC_STATUS);
}

void loop() {
  static uint32_t lastSample = 0;
  esp_task_wdt_reset();

  cliPoll();
  switchUpdate();

  uint32_t now = millis();
  if ((now - lastSample) >= SAMPLE_INTERVAL_MS) { lastSample = now; sampleAll(); }

  runStateMachine();
  delay(2);
}
