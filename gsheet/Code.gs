/**
 * ======================================================================
 *  MONITORING RUANG PENGASAPAN IKAN — Google Apps Script
 *  PRD-SMLRPI-001 v2.0  |  FR-010, FR-011, FR-017, INT-006, INT-009
 * ======================================================================
 *  Pengganti sensor1.gs / sensor2.gs (satu skrip untuk satu perangkat).
 *
 *  ENDPOINT (satu Web App):
 *    POST  (dari ESP32, JSON, wajib token)
 *       {"action":"start", ...}  -> buat TEPAT SATU tab baru untuk sesi
 *       {"action":"data",  ...}  -> tambah TEPAT SATU baris
 *       {"action":"stop",  ...}  -> catat waktu selesai
 *    GET   ?action=sessions[&limit=200][&callback=fn]
 *       -> daftar sesi (dipakai dashboard Riwayat)
 *    GET   ?action=ping
 *
 *  KEAMANAN / DATA RAHASIA (jangan tulis di kode ini):
 *    Project Settings (ikon roda gigi) -> Script Properties:
 *      TOKEN          wajib   kata sandi yang sama dengan WEBHOOK_TOKEN di firmware
 *      SPREADSHEET_ID opsional isi hanya bila skrip BUKAN dibuat dari menu
 *                     Extensions > Apps Script milik Spreadsheet-nya
 *
 *  KEANDALAN: ESP32 boleh mengirim ulang permintaan yang sama. Skrip ini
 *  idempoten: START diabaikan bila sid sudah ada; DATA diabaikan bila seq
 *  tidak lebih besar dari baris terakhir; STOP diabaikan bila sudah selesai.
 *  Jadi tidak pernah ada tab ganda atau baris ganda.
 * ======================================================================
 */

const CFG = {
  HEADERS: ['Waktu', 'Suhu (°C)', 'RH (%RH)', 'CO (mV)', 'CO (ppm)', 'Status Data', 'Seq'],
  TZ: 'Asia/Jakarta',
  LOCK_WAIT_MS: 25000,
  LIST_CACHE_KEY: 'sessions_v1',
  LIST_CACHE_SEC: 20,
  LIST_LIMIT_DEFAULT: 200,
  SID_RE: /^\d{8}-\d{6}$/,            // contoh: 20261110-143005
  DEV_RE: /^[A-Za-z0-9_-]{1,24}$/,
  MIN_TS: 1735689600,                 // 2025-01-01
  DATE_FMT: 'yyyy-mm-dd hh:mm:ss',
  META_SID: 'sid',                    // metadata untuk mencari tab dari sid
  META_INFO: 'smk'                    // metadata JSON info sesi
};

// ----------------------------------------------------------------------
//  POST — dari ESP32
// ----------------------------------------------------------------------
function doPost(e) {
  try {
    const body = parseBody_(e);
    authorize_(body.token);

    const lock = LockService.getScriptLock();
    if (!lock.tryLock(CFG.LOCK_WAIT_MS)) return json_({ ok: false, error: 'busy', retry: true });
    try {
      switch (body.action) {
        case 'start': return json_(handleStart_(body));
        case 'data':  return json_(handleData_(body));
        case 'stop':  return json_(handleStop_(body));
        default:      throw fail_('bad_action', false);
      }
    } finally {
      lock.releaseLock();
    }
  } catch (err) {
    return json_(errorBody_(err));
  }
}

// ----------------------------------------------------------------------
//  GET — dari dashboard
// ----------------------------------------------------------------------
function doGet(e) {
  const p = (e && e.parameter) || {};
  try {
    const action = p.action || 'sessions';
    if (action === 'ping')     return json_({ ok: true, now: Date.now() }, p.callback);
    if (action === 'sessions') {
      const limit = Math.max(1, Math.min(1000, parseInt(p.limit, 10) || CFG.LIST_LIMIT_DEFAULT));
      return json_({ ok: true, generated: Date.now(), sessions: listSessions_(limit) }, p.callback);
    }
    throw fail_('bad_action', false);
  } catch (err) {
    return json_(errorBody_(err), p.callback);
  }
}

// ----------------------------------------------------------------------
//  Handler aksi
// ----------------------------------------------------------------------
function handleStart_(b) {
  const sid = checkSid_(b.sid);
  const ts  = checkTs_(b.ts);
  const dev = checkDev_(b.dev);
  const ss  = getSS_();

  const existing = findSheetBySid_(ss, sid);
  if (existing) return { ok: true, dup: true, sheet: existing.getName(), gid: existing.getSheetId() };

  const name  = uniqueSheetName_(ss, sanitizeName_(b.sheet, sid));
  const sheet = ss.insertSheet(name, 0);                 // tab terbaru di paling kiri
  initSessionSheet_(sheet, sid, dev, ts);

  sheet.addDeveloperMetadata(CFG.META_SID, sid);
  sheet.addDeveloperMetadata(CFG.META_INFO, JSON.stringify({
    sid: sid, sheet: name, gid: sheet.getSheetId(), dev: dev, start_ts: ts, finish_ts: null
  }));
  invalidateList_();
  return { ok: true, sheet: name, gid: sheet.getSheetId() };
}

function handleData_(b) {
  const sid = checkSid_(b.sid);
  const ts  = checkTs_(b.ts);
  const seq = checkSeq_(b.seq);
  const ss  = getSS_();

  const sheet = findSheetBySid_(ss, sid);
  if (!sheet) throw fail_('unknown_session', false);
  const meta = readMeta_(sheet);
  if (meta && meta.finish_ts) return { ok: true, dup: true, closed: true };   // sesi sudah selesai

  const lastRow = lastDataRow_(sheet);
  const lastSeq = lastRow >= 2 ? Number(sheet.getRange(lastRow, 7).getValue()) : 0;
  if (seq <= lastSeq) return { ok: true, dup: true };

  const row = lastRow + 1;
  if (row > sheet.getMaxRows()) sheet.insertRowsAfter(sheet.getMaxRows(), 500);

  sheet.getRange(row, 1, 1, 7).setValues([[
    new Date(ts * 1000),
    num_(b.temp_c, -50, 400),
    num_(b.rh, 0, 100),
    num_(b.co_mv, 0, 20000),
    num_(b.co_ppm, 0, 1000000),
    statusText_(b.flt, b.offline),
    seq
  ]]);
  return { ok: true, row: row, seq: seq };
}

function handleStop_(b) {
  const sid = checkSid_(b.sid);
  const ts  = checkTs_(b.ts);
  const ss  = getSS_();

  const sheet = findSheetBySid_(ss, sid);
  if (!sheet) throw fail_('unknown_session', false);

  const metaObj = sheet.createDeveloperMetadataFinder().withKey(CFG.META_INFO).find()[0];
  const meta = metaObj ? JSON.parse(metaObj.getValue()) : null;
  if (meta && meta.finish_ts) return { ok: true, dup: true };

  if (metaObj) { meta.finish_ts = ts; metaObj.setValue(JSON.stringify(meta)); }
  sheet.getRange(4, 10).setValue(new Date(ts * 1000)).setNumberFormat(CFG.DATE_FMT);   // J4 = Selesai
  invalidateList_();
  return { ok: true };
}

// ----------------------------------------------------------------------
//  Daftar sesi (FR-017 / INT-009)
// ----------------------------------------------------------------------
function listSessions_(limit) {
  const cache = CacheService.getScriptCache();
  let all = null;
  const hit = cache.get(CFG.LIST_CACHE_KEY);
  if (hit) {
    try { all = JSON.parse(hit); } catch (e) { all = null; }
  }
  if (!all) {
    all = getSS_().createDeveloperMetadataFinder().withKey(CFG.META_INFO).find()
      .map(function (m) { try { return JSON.parse(m.getValue()); } catch (e) { return null; } })
      .filter(function (x) { return x && x.sid; })
      .map(function (x) {
        return {
          sid: x.sid, sheet: x.sheet, gid: x.gid, dev: x.dev,
          start_ts: x.start_ts, finish_ts: x.finish_ts || null,
          status: x.finish_ts ? 'FINISHED' : 'ACTIVE'      // ACTIVE tanpa STOP = belum ditutup
        };
      })
      .sort(function (a, b) { return b.start_ts - a.start_ts; });
    try { cache.put(CFG.LIST_CACHE_KEY, JSON.stringify(all), CFG.LIST_CACHE_SEC); } catch (e) { /* terlalu besar: abaikan */ }
  }
  return all.slice(0, limit);
}

function invalidateList_() {
  CacheService.getScriptCache().remove(CFG.LIST_CACHE_KEY);
}

// ----------------------------------------------------------------------
//  Pembantu sheet
// ----------------------------------------------------------------------
function getSS_() {
  const id = PropertiesService.getScriptProperties().getProperty('SPREADSHEET_ID');
  const ss = id ? SpreadsheetApp.openById(id) : SpreadsheetApp.getActiveSpreadsheet();
  if (!ss) throw fail_('no_spreadsheet', true);
  return ss;
}

function initSessionSheet_(sheet, sid, dev, ts) {
  sheet.getRange(1, 1, 1, CFG.HEADERS.length).setValues([CFG.HEADERS])
    .setFontWeight('bold').setBackground('#e8eaed');
  sheet.setFrozenRows(1);

  // Format kolom data. Nilai tetap ANGKA (bukan teks), sehingga aman dari
  // masalah koma/titik desimal pada locale Indonesia.
  const n = Math.max(1, sheet.getMaxRows() - 1);
  sheet.getRange(2, 1, n, 1).setNumberFormat(CFG.DATE_FMT);
  sheet.getRange(2, 2, n, 1).setNumberFormat('0.00');
  sheet.getRange(2, 3, n, 1).setNumberFormat('0.0');
  sheet.getRange(2, 4, n, 1).setNumberFormat('0');
  sheet.getRange(2, 5, n, 1).setNumberFormat('0.0');
  sheet.setColumnWidth(1, 150);
  sheet.setColumnWidth(6, 170);

  // Blok info sesi di kolom I:J (di luar tabel data A:G)
  sheet.getRange(1, 9, 4, 2).setValues([
    ['Session ID', sid],
    ['Perangkat', dev],
    ['Mulai', new Date(ts * 1000)],
    ['Selesai', 'sesi berjalan']
  ]);
  sheet.getRange(1, 9, 4, 1).setFontWeight('bold');
  sheet.getRange(3, 10).setNumberFormat(CFG.DATE_FMT);
  sheet.setColumnWidth(9, 100);
  sheet.setColumnWidth(10, 150);
}

function findSheetBySid_(ss, sid) {
  const found = ss.createDeveloperMetadataFinder().withKey(CFG.META_SID).withValue(sid).find();
  if (!found.length) return null;
  return found[0].getLocation().getSheet();
}

function readMeta_(sheet) {
  const m = sheet.createDeveloperMetadataFinder().withKey(CFG.META_INFO).find()[0];
  if (!m) return null;
  try { return JSON.parse(m.getValue()); } catch (e) { return null; }
}

// Baris terakhir yang kolom A-nya terisi (blok info I:J tidak ikut dihitung).
function lastDataRow_(sheet) {
  const lr = sheet.getLastRow();
  if (lr < 2) return 1;
  const vals = sheet.getRange(2, 1, lr - 1, 1).getValues();
  for (let i = vals.length - 1; i >= 0; i--) {
    if (vals[i][0] !== '' && vals[i][0] !== null) return i + 2;
  }
  return 1;
}

function sanitizeName_(raw, fallback) {
  let s = (typeof raw === 'string' ? raw : '').replace(/[^0-9A-Za-z _.\-]/g, '_').trim().substring(0, 60);
  return s || fallback;
}

function uniqueSheetName_(ss, base) {
  let name = base, i = 2;
  while (ss.getSheetByName(name)) { name = base + ' (' + i + ')'; i++; }
  return name;
}

// ----------------------------------------------------------------------
//  Validasi & utilitas
// ----------------------------------------------------------------------
function parseBody_(e) {
  if (!e || !e.postData || !e.postData.contents) throw fail_('empty_body', false);
  let b;
  try { b = JSON.parse(e.postData.contents); } catch (x) { throw fail_('bad_json', false); }
  if (!b || typeof b !== 'object') throw fail_('bad_json', false);
  return b;
}

function authorize_(token) {
  const expected = PropertiesService.getScriptProperties().getProperty('TOKEN');
  if (!expected) throw fail_('server_not_configured', false);
  if (typeof token !== 'string' || !safeEqual_(token, expected)) throw fail_('unauthorized', false);
}

function safeEqual_(a, b) {                     // perbandingan waktu-tetap
  let diff = a.length ^ b.length;
  const n = Math.max(a.length, b.length);
  for (let i = 0; i < n; i++) diff |= (a.charCodeAt(i) || 0) ^ (b.charCodeAt(i) || 0);
  return diff === 0;
}

function checkSid_(v) { if (typeof v !== 'string' || !CFG.SID_RE.test(v)) throw fail_('bad_sid', false); return v; }
function checkDev_(v) { if (typeof v !== 'string' || !CFG.DEV_RE.test(v)) throw fail_('bad_dev', false); return v; }
function checkTs_(v)  { const n = Number(v); if (!isFinite(n) || n < CFG.MIN_TS) throw fail_('bad_ts', false); return Math.floor(n); }
function checkSeq_(v) { const n = Number(v); if (!isFinite(n) || n < 1 || n !== Math.floor(n)) throw fail_('bad_seq', false); return n; }

// null / tidak valid -> sel KOSONG (tidak pernah mengarang nilai)
function num_(v, min, max) {
  if (v === null || v === undefined || typeof v !== 'number' || !isFinite(v)) return '';
  if (v < min || v > max) return '';
  return v;
}

function statusText_(flt, offline) {
  const f = Number(flt) || 0, parts = [];
  if (Number(offline) === 1) parts.push('OFFLINE (tersimpan lalu dikirim)');
  if (f & 1) parts.push('FAULT SUHU');
  if (f & 2) parts.push('FAULT RH');
  if (f & 4) parts.push('FAULT CO');
  if (f & 8) parts.push('CO PEMANASAN');
  return parts.length ? parts.join(', ') : 'OK';
}

function fail_(code, retry) { return { api: true, code: code, retry: !!retry }; }

function errorBody_(err) {
  if (err && err.api) return { ok: false, error: err.code, retry: err.retry };
  console.error(err && err.stack ? err.stack : String(err));
  return { ok: false, error: 'internal', retry: true };
}

function json_(obj, callback) {
  const text = JSON.stringify(obj);
  if (callback && /^[A-Za-z_$][0-9A-Za-z_$.]{0,60}$/.test(callback)) {       // JSONP opsional
    return ContentService.createTextOutput(callback + '(' + text + ');')
      .setMimeType(ContentService.MimeType.JAVASCRIPT);
  }
  return ContentService.createTextOutput(text).setMimeType(ContentService.MimeType.JSON);
}

// ----------------------------------------------------------------------
//  Alat bantu manual (jalankan dari editor Apps Script)
// ----------------------------------------------------------------------
/** Jalankan SEKALI setelah menempel kode: atur zona waktu & cek konfigurasi. */
function setup() {
  const ss = getSS_();
  ss.setSpreadsheetTimeZone(CFG.TZ);
  const hasToken = !!PropertiesService.getScriptProperties().getProperty('TOKEN');
  console.log('Spreadsheet: ' + ss.getName() + ' | zona waktu: ' + CFG.TZ);
  console.log(hasToken ? 'TOKEN sudah diisi.' : 'PERHATIAN: Script Property TOKEN belum diisi!');
}

/** Uji alur START -> DATA -> DATA ulang -> STOP lalu hapus tab uji. */
function selfTest() {
  const ss = getSS_();
  const now = Math.floor(Date.now() / 1000);
  const sid = '20990101-000000';
  const r = [];
  r.push(handleStart_({ sid: sid, ts: now, dev: 'test', sheet: 'TEST selfTest' }));
  r.push(handleStart_({ sid: sid, ts: now, dev: 'test', sheet: 'TEST selfTest' }));          // ulang -> dup
  r.push(handleData_({ sid: sid, ts: now + 60, seq: 1, temp_c: 80.5, rh: 40, co_mv: 500, co_ppm: null, flt: 0, offline: 0 }));
  r.push(handleData_({ sid: sid, ts: now + 60, seq: 1, temp_c: 80.5, rh: 40, co_mv: 500, co_ppm: null, flt: 0, offline: 0 })); // ulang -> dup
  r.push(handleStop_({ sid: sid, ts: now + 90 }));
  r.push(handleStop_({ sid: sid, ts: now + 90 }));                                           // ulang -> dup
  console.log(JSON.stringify(r));
  const sh = findSheetBySid_(ss, sid);
  if (sh) ss.deleteSheet(sh);
  invalidateList_();
}
