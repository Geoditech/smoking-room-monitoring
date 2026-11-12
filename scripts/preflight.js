#!/usr/bin/env node
/*
 * preflight.js — pemeriksa sebelum push ke GitHub.
 * Menjalankan: npm run preflight
 * ERROR  -> rahasia kemungkinan akan terunggah (exit 1).
 * PERINGATAN -> perlu perhatian, tidak menggagalkan.
 */
const { execSync } = require('child_process');
const fs = require('fs');
const errors = [], warns = [];

function git(cmd) {
  try { return execSync('git ' + cmd, { encoding: 'utf8', stdio: ['ignore', 'pipe', 'ignore'] }); }
  catch (e) { return null; }
}

const listed = git('ls-files');
if (listed === null) {
  console.log('Bukan repositori Git (atau Git belum terpasang): pemeriksaan dilewati.');
  process.exit(0);
}
const files = listed.split('\n').filter(Boolean);

const isPlaceholder = (v) => !v || /^(isi|ISI)/.test(v) || /ISI_/.test(v);
const SKIP_CONTENT = /(^|\/)package-lock\.json$|^tests\/|^scripts\/|\.(png|jpg|ico)$/;   // file contoh TETAP dipindai

for (const f of files) {
  // 1. secrets.h tidak boleh dilacak Git
  if (/(^|\/)secrets\.h$/.test(f)) {
    errors.push(f + ' ikut dilacak Git. Lepas dari Git: git rm --cached ' + f);
    continue;
  }
  if (SKIP_CONTENT.test(f) || !fs.existsSync(f)) continue;
  let text;
  try { text = fs.readFileSync(f, 'utf8'); } catch (e) { continue; }

  // 2. nilai rahasia asli pada file terlacak
  for (const m of text.matchAll(/#define\s+(WIFI_PASSWORD|MQTT_PASS|MQTT_USER|WEBHOOK_TOKEN)\s+"([^"]*)"/g)) {
    if (!isPlaceholder(m[2])) errors.push(f + ': ' + m[1] + ' berisi nilai asli (bukan placeholder).');
  }
  if (/BEGIN (RSA |EC |OPENSSH )?PRIVATE KEY/.test(text)) errors.push(f + ': memuat private key.');
  for (const m of text.matchAll(/script\.google\.com\/macros\/s\/([A-Za-z0-9_-]{20,})\/exec/g)) {
    if (f === 'web/config.js') warns.push('web/config.js memuat URL Web App asli: itu memang publik, pastikan Anda memang ingin menerbitkannya.');
    else errors.push(f + ': memuat URL deployment Apps Script asli.');
  }
}

// 3. web/config.js: hanya akun baca; tidak boleh akun publish / token
if (fs.existsSync('web/config.js')) {
  const c = fs.readFileSync('web/config.js', 'utf8');
  if (/smk_device/.test(c)) errors.push('web/config.js memuat akun smk_device (akun publish). Pakai akun smk_dashboard (subscribe-only).');
  if (/TOKEN\s*:/i.test(c.replace(/\/\*[\s\S]*?\*\//g, ''))) errors.push('web/config.js memuat TOKEN. Token webhook tidak boleh ada di dashboard.');
  if (/ISI_/.test(c)) warns.push('web/config.js masih berisi placeholder (ISI_...). Dashboard menampilkan "Konfigurasi belum lengkap" sampai diisi.');
}

// 4. .gitignore harus mengecualikan secrets.h
const gi = fs.existsSync('.gitignore') ? fs.readFileSync('.gitignore', 'utf8') : '';
if (!/secrets\.h/.test(gi)) errors.push('.gitignore tidak mengecualikan secrets.h.');

for (const w of warns) console.log('PERINGATAN: ' + w);
for (const e of errors) console.log('ERROR: ' + e);
if (errors.length) { console.log('\nPemeriksaan GAGAL: perbaiki sebelum push.'); process.exit(1); }
console.log('Pemeriksaan lolos: tidak ada rahasia pada file terlacak.' + (warns.length ? ' (' + warns.length + ' peringatan)' : ''));
