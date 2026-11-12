// ======================================================================
//  secrets.example.h  —  CONTOH. Aman di-upload ke GitHub.
//
//  Cara pakai:
//    1. Salin file ini menjadi  secrets.h  (di folder yang sama).
//    2. Isi nilai aslinya di secrets.h.
//    3. secrets.h TIDAK boleh di-upload (sudah masuk .gitignore).
//
//  Nilai di sini hanyalah DEFAULT. Setelah firmware berjalan, semuanya
//  bisa diganti lewat Serial Monitor tanpa kompilasi ulang
//  (ketik "help" di Serial Monitor, 115200 baud). Lihat FR-015 / NFR-011.
//
//  Jangan memakai tanda kutip ganda (") atau backslash (\) di dalam nilai.
// ======================================================================
#pragma once

// ---- Wi-Fi ----------------------------------------------------------
#define WIFI_SSID        "isi-nama-wifi"
#define WIFI_PASSWORD    "isi-password-wifi"

// ---- Broker MQTT (EMQX) — akun yang boleh PUBLISH ---------------------
// EMQX Cloud Serverless HANYA menerima koneksi TLS: pakai port 8883 dan
// MQTT_USE_TLS 1. Port 1883 TIDAK didukung. Host harus berupa nama domain
// (bukan alamat IP) dari halaman Overview deployment.
#define MQTT_HOST        "isi-host-broker.emqxsl.com"
#define MQTT_PORT        8883          // Serverless: 8883. Broker lain tanpa TLS: 1883
#define MQTT_USE_TLS     1             // Serverless: wajib 1
#define MQTT_USER        "isi-username-publish"
#define MQTT_PASS        "isi-password-publish"
// Opsional: sertifikat CA broker (format PEM). Jika tidak diisi dan TLS aktif,
// koneksi tetap terenkripsi tetapi sertifikat server TIDAK diverifikasi.
// #define MQTT_CA_CERT  "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"

// ---- Google Apps Script (webhook) -------------------------------------
#define WEBHOOK_URL      "https://script.google.com/macros/s/ISI_DEPLOYMENT_ID/exec"
#define WEBHOOK_TOKEN    "isi-token-acak-panjang"   // harus sama dengan token di Script Properties

// ---- Identitas perangkat ----------------------------------------------
#define DEVICE_ID        "smk-01"      // dipakai pada topik MQTT: smokehouse/<DEVICE_ID>/...
