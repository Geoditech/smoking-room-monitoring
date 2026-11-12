/*
 * script.js — Dashboard Real-Time (FR-016)
 * Data dari MQTT (WSS) -> 3 kartu + 3 grafik Chart.js yang berdiri sendiri.
 * Aturan: tidak ada alarm, ambang peringatan, atau notifikasi (FR-019);
 * tidak ada data palsu (tidak ada nilai -> tampil "—" dan celah di grafik).
 */
(() => {
  'use strict';
  const { cfg, $, setText, fmtNum, timeWib, dateTimeWib, fmtDuration, isPlaceholder,
          startClock, showConfigError, makeLineChart } = window.SMK;

  startClock();

  // ── Tautan Google Sheets ───────────────────────────────────────────
  const sheets = cfg.SHEETS || {};
  const btnSheet = $('btn-sheet');
  if (!isPlaceholder(sheets.SPREADSHEET_ID)) {
    btnSheet.href = 'https://docs.google.com/spreadsheets/d/' + encodeURIComponent(sheets.SPREADSHEET_ID) + '/edit';
  } else {
    btnSheet.classList.add('disabled');
  }

  // ── Pemeriksaan konfigurasi ────────────────────────────────────────
  const mq = cfg.MQTT || {};
  if (isPlaceholder(mq.URL) || isPlaceholder(mq.PASSWORD) || isPlaceholder(cfg.DEVICE_ID)) {
    showConfigError('Isi MQTT.URL, MQTT.PASSWORD, dan DEVICE_ID di web/config.js.');
    setConn('disconnected', 'Belum dikonfigurasi');
    setBanner('wait', 'fa-gear', 'Dashboard belum dikonfigurasi', 'Lengkapi file config.js lalu muat ulang halaman.');
    return;
  }
  if (typeof mqtt === 'undefined' || typeof Chart === 'undefined') {
    showConfigError('Library MQTT/Chart.js gagal dimuat. Periksa koneksi internet lalu muat ulang.');
    return;
  }

  // ── Grafik ─────────────────────────────────────────────────────────
  const MAX_POINTS = cfg.CHART_MAX_POINTS || 300;
  const charts = {
    temp: makeLineChart('chart-temp', { datasets: [{ label: 'Suhu (°C)', color: '#e4572e', fill: true }], yTitle: '°C', decimals: 2 }),
    rh:   makeLineChart('chart-rh',   { datasets: [{ label: 'RH (%RH)',  color: '#0083b0', fill: true }], yTitle: '%RH', yMin: 0, yMax: 100, decimals: 1 }),
    co:   makeLineChart('chart-co',   { datasets: [{ label: 'CO (mV)',   color: '#6b7a99', fill: true }], yTitle: 'mV', decimals: 0 })
  };

  function pushPoint(key, label, value) {
    const ch = charts[key];
    if (!ch) return;
    ch.data.labels.push(label);
    ch.data.datasets[0].data.push(value);
    while (ch.data.labels.length > MAX_POINTS) { ch.data.labels.shift(); ch.data.datasets[0].data.shift(); }
    ch.update('none');
    const empty = $('empty-' + key);
    if (empty) empty.hidden = true;
  }

  function resetCharts() {
    for (const key of Object.keys(charts)) {
      const ch = charts[key];
      if (!ch) continue;
      ch.data.labels.length = 0;
      ch.data.datasets[0].data.length = 0;
      ch.update('none');
      const empty = $('empty-' + key);
      if (empty) empty.hidden = false;
    }
  }

  // ── Keadaan ────────────────────────────────────────────────────────
  const S = {
    mqtt: 'connecting',
    status: null,          // pesan status terakhir
    sid: null,             // sesi pada grafik
    lastTeleAt: 0,         // Date.now() saat telemetry terakhir diterima
    lastTeleTs: 0,         // ts perangkat pada telemetry terakhir
    coUnit: 'mV'
  };

  const num = (v) => (typeof v === 'number' && isFinite(v)) ? v : null;

  // ── Tampilan: status koneksi dashboard ─────────────────────────────
  function setConn(state, text) {
    const dot = $('net-mqtt-dot');
    if (dot) dot.className = 'status-dot ' + state;
    setText('net-mqtt-text', text || ({ connected: 'Tersambung', connecting: 'Menghubungkan...', disconnected: 'Terputus' }[state]));
  }

  function setBanner(kind, icon, title, text) {
    const el = $('session-banner');
    el.className = 'session-banner state-' + kind;
    $('sb-icon').className = 'fas ' + icon;
    setText('sb-title', title);
    setText('sb-text', text);
  }

  const DEVICE_STATE_ID = {
    INITIALIZING: 'Perangkat sedang memulai',
    CONNECTING_WIFI: 'Perangkat menyambung ke Wi-Fi',
    SYNCING_TIME: 'Perangkat menyinkronkan waktu (NTP)',
    CONNECTING_MQTT: 'Perangkat menyambung ke broker',
    WAIT_SWITCH_OFF: 'Switch sedang ON saat perangkat menyala: geser ke OFF lalu ON untuk memulai sesi',
    IDLE: 'Menunggu sesi (switch OFF, tidak ada sesi aktif)',
    SESSION_ACTIVE: 'Sesi berjalan',
    OFFLINE: 'Perangkat tidak terhubung ke broker'
  };

  function wifiText(rssi) {
    if (typeof rssi !== 'number' || rssi === 0) return '—';
    const q = rssi >= -60 ? 'kuat' : rssi >= -75 ? 'sedang' : 'lemah';
    return rssi + ' dBm (' + q + ')';
  }

  // Satu fungsi yang merangkum banner + panel info dari keadaan terkini.
  function render() {
    const st = S.status;
    const stale = S.lastTeleAt && (Date.now() - S.lastTeleAt) > (cfg.STALE_AFTER_S || 15) * 1000;

    // Banner
    if (S.mqtt !== 'connected') {
      setBanner('wait', 'fa-plug-circle-xmark', 'Dashboard belum tersambung ke broker', 'Mencoba menyambung kembali secara otomatis.');
    } else if (!st) {
      setBanner('wait', 'fa-hourglass-half', 'Menunggu status dari perangkat', 'Belum ada pesan status. Pastikan perangkat menyala dan terhubung.');
    } else if (st.state === 'SESSION_ACTIVE') {
      const dur = st.start_ts ? fmtDuration(Date.now() / 1000 - st.start_ts) : '—';
      let text = 'ID ' + (st.sid || '—') + ' · mulai ' + (st.start_ts ? dateTimeWib(st.start_ts) : '—') + ' · durasi ' + dur;
      if (stale) text += ' · tidak ada data baru sejak ' + timeWib(S.lastTeleAt / 1000);
      setBanner('active', 'fa-circle-play', 'Sesi pengasapan sedang berjalan', text);
    } else if (st.state === 'IDLE') {
      setBanner('idle', 'fa-circle-pause', 'Tidak ada sesi aktif', 'Geser switch di panel ke ON untuk memulai sesi baru. Grafik menampilkan data sesi terakhir.');
    } else {
      setBanner('wait', 'fa-circle-info', DEVICE_STATE_ID[st.state] || ('Status perangkat: ' + st.state),
        st.state === 'OFFLINE' ? 'Pesan terakhir diterima dari broker saat perangkat terputus.' : 'Perangkat belum siap menerima sesi.');
    }

    // Informasi sesi
    const active = st && st.state === 'SESSION_ACTIVE';
    setText('info-sid', active ? (st.sid || '—') : (S.sid || '—'));
    setText('info-start', active && st.start_ts ? dateTimeWib(st.start_ts) : '—');
    setText('info-duration', active && st.start_ts ? fmtDuration(Date.now() / 1000 - st.start_ts) : '—');
    setText('info-device', st ? ((st.dev || cfg.DEVICE_ID) + ' / fw ' + (st.fw || '—')) : '—');

    // Status jaringan perangkat
    setText('net-device', st ? (DEVICE_STATE_ID[st.state] || st.state) : '—');
    setText('net-wifi', st && st.state !== 'OFFLINE' ? wifiText(st.rssi) : '—');
    let q = '—';
    if (st && typeof st.qd === 'number') {
      q = st.qd === 0 ? 'Kosong (semua data terkirim)' : st.qd + ' data menunggu dikirim';
      if (st.wh_rej > 0) q += ' · ' + st.wh_rej + ' ditolak server (periksa token)';
    }
    setText('net-queue', q);
    setText('net-last', S.lastTeleAt ? timeWib(S.lastTeleAt / 1000) + ' WIB' : '—');
    setText('net-reset', st && st.rst ? st.rst : '—');
  }

  // ── Kartu sensor ───────────────────────────────────────────────────
  function setChip(id, icon, text) {
    const el = $(id);
    el.innerHTML = '';                                   // hanya kita yang mengisi; tanpa data eksternal
    const i = document.createElement('i'); i.className = 'fas ' + icon;
    el.appendChild(i); el.appendChild(document.createTextNode(' ' + text));
  }

  function updateCards(d) {
    const flt = (typeof d.flt === 'number') ? d.flt : 0;
    const tsOk = typeof d.ts === 'number' && d.ts > 0;
    const stamp = tsOk ? d.ts : Math.floor(Date.now() / 1000);
    const label = timeWib(stamp);
    const upd = 'Diperbarui ' + label + ' WIB';

    // Suhu
    const t = num(d.temp_c);
    setText('val-temp', fmtNum(t, 2));
    setText('meta-temp', upd);
    if (flt & 1) setChip('chip-temp', 'fa-plug-circle-xmark', 'Sensor suhu tidak terbaca' + (d.t_flt ? ' (kode ' + d.t_flt + ')' : ''));
    else setChip('chip-temp', 'fa-circle-check', 'Sensor terbaca');
    pushPoint('temp', label, t);

    // RH
    const rh = num(d.rh);
    setText('val-rh', fmtNum(rh, 1));
    setText('meta-rh', upd);
    if (flt & 2) setChip('chip-rh', 'fa-plug-circle-xmark', 'Sensor RH tidak terbaca');
    else setChip('chip-rh', 'fa-circle-check', 'Sensor terbaca');
    pushPoint('rh', label, rh);

    // CO: PPM hanya bila perangkat sudah dikalibrasi (co_cal = 1) dan nilainya ada.
    const ppm = num(d.co_ppm), mv = num(d.co_mv);
    const usePpm = d.co_cal === 1 && ppm !== null;
    const unit = usePpm ? 'ppm' : 'mV';
    if (unit !== S.coUnit) {                             // satuan berubah -> grafik CO dimulai ulang
      S.coUnit = unit;
      const ch = charts.co;
      if (ch) {
        ch.data.labels.length = 0; ch.data.datasets[0].data.length = 0;
        ch.data.datasets[0].label = 'CO (' + unit + ')';
        ch.options.scales.y.title.text = unit;
      }
    }
    setText('unit-co', unit);
    setText('val-co', usePpm ? fmtNum(ppm, 1) : fmtNum(mv, 0));
    setText('meta-co', upd + (usePpm ? ' · mentah ' + fmtNum(mv, 0) + ' mV'
                                      : ' · belum dikalibrasi: ditampilkan sebagai tegangan sensor, bukan ppm'));
    if (flt & 4) setChip('chip-co', 'fa-plug-circle-xmark', 'Sinyal sensor CO di luar rentang (jenuh atau kabel)');
    else if (flt & 8) setChip('chip-co', 'fa-fire-flame-simple', 'Sensor CO masih pemanasan, nilai belum stabil');
    else setChip('chip-co', 'fa-circle-check', 'Sensor terbaca');
    pushPoint('co', label, usePpm ? ppm : mv);
  }

  // ── Penanganan pesan MQTT ──────────────────────────────────────────
  function onTelemetry(d) {
    if (d.dev && d.dev !== cfg.DEVICE_ID) return;
    if (d.sid && d.sid !== S.sid) { resetCharts(); S.sid = d.sid; S.coUnit = 'mV'; }   // sesi baru -> grafik baru
    S.lastTeleAt = Date.now();
    S.lastTeleTs = d.ts || 0;
    updateCards(d);
    render();
  }

  function onStatus(d) {
    if (d.dev && d.dev !== cfg.DEVICE_ID) return;
    S.status = d;
    if (d.state === 'SESSION_ACTIVE' && d.sid && d.sid !== S.sid && S.sid !== null) { resetCharts(); S.sid = d.sid; }
    render();
  }

  // ── Koneksi MQTT (WSS) ─────────────────────────────────────────────
  const base = 'smokehouse/' + cfg.DEVICE_ID;
  const TOPIC_TELE = base + '/telemetry', TOPIC_STATUS = base + '/status';

  const client = mqtt.connect(mq.URL, {
    username: mq.USERNAME, password: mq.PASSWORD,
    clientId: 'smkweb-' + Math.random().toString(16).slice(2, 10),
    clean: true, keepalive: 30, reconnectPeriod: 3000, connectTimeout: 10000
  });

  client.on('connect', () => {
    S.mqtt = 'connected';
    setConn('connected');
    client.subscribe([TOPIC_TELE, TOPIC_STATUS], { qos: 0 }, (err, granted) => {
      if (err) { setConn('connected', 'Tersambung, subscribe gagal'); return; }
      if (granted && granted.some((g) => g.qos === 128)) setConn('connected', 'Tersambung, subscribe ditolak (cek ACL EMQX)');
    });
    render();
  });
  client.on('reconnect', () => { S.mqtt = 'connecting'; setConn('connecting'); render(); });
  client.on('close',     () => { S.mqtt = 'disconnected'; setConn('disconnected'); render(); });
  client.on('error', (e) => {
    console.error('MQTT error', e);
    const msg = String(e && e.message || e);
    if (/not authorized|bad user|code 4|code 5/i.test(msg)) { S.mqtt = 'disconnected'; setConn('disconnected', 'Autentikasi broker gagal (cek username/password)'); }
  });
  client.on('message', (topic, payload) => {
    let d;
    try { d = JSON.parse(payload.toString()); } catch (e) { return; }
    if (!d || typeof d !== 'object') return;
    if (topic === TOPIC_TELE) onTelemetry(d);
    else if (topic === TOPIC_STATUS) onStatus(d);
  });

  // Penyegaran tiap detik: durasi sesi & penanda "tidak ada data baru".
  setInterval(render, 1000);
  render();
})();
