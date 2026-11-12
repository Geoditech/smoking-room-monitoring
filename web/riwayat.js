/*
 * riwayat.js — Dashboard Riwayat (FR-017, FR-018)
 * 1) Daftar sesi dari Google Apps Script  (?action=sessions)
 * 2) Data satu sesi dari Google Sheets lewat gviz JSON (angka tetap angka,
 *    sehingga terhindar dari masalah koma/titik desimal locale Indonesia)
 * Tidak ada ambang, alarm, atau notifikasi (FR-019).
 */
(() => {
  'use strict';
  const { cfg, $, setText, fmtNum, dateTimeWib, fmtDuration, isPlaceholder,
          startClock, showConfigError, makeLineChart } = window.SMK;

  startClock();

  const sh = cfg.SHEETS || {};
  if (isPlaceholder(sh.SPREADSHEET_ID) || isPlaceholder(sh.WEB_APP_URL)) {
    showConfigError('Isi SHEETS.SPREADSHEET_ID dan SHEETS.WEB_APP_URL di web/config.js.');
    const sel = $('session-select');
    sel.innerHTML = ''; sel.appendChild(new Option('Belum dikonfigurasi', ''));
    return;
  }
  if (typeof Chart === 'undefined') { showConfigError('Library Chart.js gagal dimuat. Periksa koneksi internet.'); return; }

  const SSID = sh.SPREADSHEET_ID;
  let sessions = [];
  let charts = [];
  let reqToken = 0;                       // mengabaikan respons lama bila pengguna berganti sesi cepat

  // ── Pengambilan data (fetch, dengan cadangan JSONP / script tag) ───
  let cbSeq = 0;
  function jsonp(url, paramName, extra) {
    return new Promise((resolve, reject) => {
      const name = '__smk_cb' + (++cbSeq);
      const s = document.createElement('script');
      const timer = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, 15000);
      function cleanup() { clearTimeout(timer); delete window[name]; s.remove(); }
      window[name] = (data) => { cleanup(); resolve(data); };
      s.onerror = () => { cleanup(); reject(new Error('gagal memuat skrip')); };
      s.src = url + (url.includes('?') ? '&' : '?') + paramName + '=' + name + (extra || '');
      document.head.appendChild(s);
    });
  }

  async function getSessions() {
    const url = sh.WEB_APP_URL + '?action=sessions&limit=200';
    let data;
    try {
      const res = await fetch(url, { cache: 'no-store' });
      data = await res.json();
    } catch (e) {
      data = await jsonp(sh.WEB_APP_URL + '?action=sessions&limit=200', 'callback');
    }
    if (!data || !data.ok) throw new Error((data && data.error) || 'respons tidak valid');
    return data.sessions || [];
  }

  const GVIZ_QUERY = 'select A,B,C,D,E,F,G where A is not null';
  function gvizUrl(gid, handler) {
    return 'https://docs.google.com/spreadsheets/d/' + encodeURIComponent(SSID) + '/gviz/tq?gid=' + encodeURIComponent(gid) +
      '&headers=1&tq=' + encodeURIComponent(GVIZ_QUERY) +
      '&tqx=' + (handler ? 'out:json;responseHandler:' + handler : 'out:json');
  }

  async function getGviz(gid) {
    try {
      const res = await fetch(gvizUrl(gid), { cache: 'no-store' });
      const text = await res.text();
      return parseGviz(text);
    } catch (e) {
      // cadangan: muat sebagai skrip (tanpa CORS)
      const name = '__smk_gviz' + (++cbSeq);
      return new Promise((resolve, reject) => {
        const s = document.createElement('script');
        const timer = setTimeout(() => { cleanup(); reject(new Error('timeout')); }, 20000);
        function cleanup() { clearTimeout(timer); delete window[name]; s.remove(); }
        window[name] = (data) => { cleanup(); resolve(data); };
        s.onerror = () => { cleanup(); reject(new Error('gagal memuat skrip')); };
        s.src = gvizUrl(gid, name);
        document.head.appendChild(s);
      });
    }
  }

  function parseGviz(text) {
    const a = text.indexOf('{'), b = text.lastIndexOf('}');
    if (a < 0 || b < a) throw new Error('format respons tidak dikenali (apakah Sheet dibagikan "Anyone with the link"?)');
    return JSON.parse(text.slice(a, b + 1));
  }

  // ── Penguraian baris gviz ──────────────────────────────────────────
  function cellNum(c) {
    if (!c || c.v === null || c.v === undefined) return null;
    if (typeof c.v === 'number') return isFinite(c.v) ? c.v : null;
    const n = Number(String(c.v).replace(',', '.'));
    return isFinite(n) && String(c.v).trim() !== '' ? n : null;
  }

  function cellDate(c) {
    if (!c || typeof c.v !== 'string') return null;
    const m = c.v.match(/^Date\((\d+),(\d+),(\d+)(?:,(\d+),(\d+),(\d+))?/);
    if (!m) return null;
    // Komponen sudah berupa jam dinding zona waktu Spreadsheet (WIB); dipakai apa adanya.
    return new Date(+m[1], +m[2], +m[3], +(m[4] || 0), +(m[5] || 0), +(m[6] || 0));
  }

  function rowsFromGviz(json) {
    if (json.status === 'error') {
      const e = (json.errors && json.errors[0]) || {};
      throw new Error(e.detailed_message || e.message || 'galat dari Google Sheets');
    }
    const out = [];
    for (const r of (json.table && json.table.rows) || []) {
      const c = r.c || [];
      const t = cellDate(c[0]);
      if (!t) continue;                                   // lewati baris tanpa waktu valid (mis. header)
      out.push({
        t, temp: cellNum(c[1]), rh: cellNum(c[2]), mv: cellNum(c[3]), ppm: cellNum(c[4]),
        status: (c[5] && c[5].v != null) ? String(c[5].v) : ''
      });
    }
    out.sort((x, y) => x.t - y.t);
    return out;
  }

  // ── Tampilan ───────────────────────────────────────────────────────
  const p2 = (n) => String(n).padStart(2, '0');
  const hhmm = (d) => p2(d.getHours()) + ':' + p2(d.getMinutes());
  const full = (d) => p2(d.getDate()) + '/' + p2(d.getMonth() + 1) + '/' + d.getFullYear() + ' ' + hhmm(d) + ':' + p2(d.getSeconds());

  function showError(msg) { $('data-error').hidden = false; setText('data-error-text', msg); }
  function clearError() { $('data-error').hidden = true; }

  function stat(values, dec, unit) {
    const v = values.filter((x) => x !== null);
    if (!v.length) return 'Tidak ada data terukur pada sesi ini';
    const min = Math.min(...v), max = Math.max(...v), avg = v.reduce((a, b) => a + b, 0) / v.length;
    return 'Min ' + fmtNum(min, dec) + ' · Rata-rata ' + fmtNum(avg, dec) + ' · Maks ' + fmtNum(max, dec) + ' ' + unit + ' (' + v.length + ' data)';
  }

  function destroyCharts() { charts.forEach((c) => c && c.destroy()); charts = []; }

  function fillChart(chart, labels, ...series) {
    chart.data.labels = labels;
    series.forEach((s, i) => { chart.data.datasets[i].data = s; });
    chart.update('none');
  }

  function renderSession(meta, rows) {
    destroyCharts();
    $('result').hidden = false;

    // Ringkasan
    setText('sum-count', String(rows.length));
    setText('sum-start', meta.start_ts ? dateTimeWib(meta.start_ts) : (rows.length ? full(rows[0].t) : '—'));
    if (meta.finish_ts) {
      setText('sum-finish', dateTimeWib(meta.finish_ts));
      setText('sum-duration', fmtDuration(meta.finish_ts - meta.start_ts));
    } else {
      setText('sum-finish', 'Belum ditutup');
      setText('sum-duration', rows.length > 1 ? fmtDuration((rows[rows.length - 1].t - rows[0].t) / 1000) + ' (sampai data terakhir)' : '—');
    }

    // Statistik
    const hasPpm = rows.some((r) => r.ppm !== null);
    setText('stat-temp', stat(rows.map((r) => r.temp), 2, '°C'));
    setText('stat-rh', stat(rows.map((r) => r.rh), 1, '%RH'));
    setText('stat-co', hasPpm ? stat(rows.map((r) => r.ppm), 1, 'ppm')
                              : stat(rows.map((r) => r.mv), 0, 'mV') + ' · belum dikalibrasi (tanpa ppm)');

    // Grafik
    const labels = rows.map((r) => hhmm(r.t));
    const cTemp = makeLineChart('chart-temp', { datasets: [{ label: 'Suhu (°C)', color: '#e4572e', fill: true }], yTitle: '°C', decimals: 2, pointRadius: 2 });
    const cRh   = makeLineChart('chart-rh',   { datasets: [{ label: 'RH (%RH)', color: '#0083b0', fill: true }], yTitle: '%RH', yMin: 0, yMax: 100, decimals: 1, pointRadius: 2 });
    // CO: tampilkan ppm bila ada (data terkalibrasi); jika tidak, tegangan mentah (mV).
    const coUnit = hasPpm ? 'ppm' : 'mV';
    const cCo = makeLineChart('chart-co', { datasets: [{ label: 'CO (' + coUnit + ')', color: hasPpm ? '#8e5ea2' : '#6b7a99', fill: true }],
                                            yTitle: coUnit, decimals: hasPpm ? 1 : 0, pointRadius: 2 });
    charts = [cTemp, cRh, cCo];
    fillChart(cTemp, labels, rows.map((r) => r.temp));
    fillChart(cRh, labels, rows.map((r) => r.rh));
    fillChart(cCo, labels, rows.map((r) => (hasPpm ? r.ppm : r.mv)));

    // Tabel
    const tb = $('data-tbody');
    tb.innerHTML = '';
    if (!rows.length) {
      const tr = document.createElement('tr'), td = document.createElement('td');
      td.colSpan = 6; td.className = 'empty'; td.textContent = 'Belum ada baris data pada sesi ini.';
      tr.appendChild(td); tb.appendChild(tr);
    }
    for (const r of rows) {
      const tr = document.createElement('tr');
      [full(r.t), fmtNum(r.temp, 2), fmtNum(r.rh, 1), fmtNum(r.mv, 0), fmtNum(r.ppm, 1), r.status || '—'].forEach((txt, i) => {
        const td = document.createElement('td'); td.textContent = txt; if (i === 5) td.className = 'left'; tr.appendChild(td);
      });
      tb.appendChild(tr);
    }

    // Tautan
    $('btn-open').href = 'https://docs.google.com/spreadsheets/d/' + encodeURIComponent(SSID) + '/edit#gid=' + encodeURIComponent(meta.gid);
    $('btn-csv').href  = 'https://docs.google.com/spreadsheets/d/' + encodeURIComponent(SSID) + '/export?format=csv&gid=' + encodeURIComponent(meta.gid);
  }

  async function loadSession(meta) {
    const my = ++reqToken;
    clearError();
    $('loading').hidden = false;
    $('result').hidden = true;
    try {
      const rows = rowsFromGviz(await getGviz(meta.gid));
      if (my !== reqToken) return;
      renderSession(meta, rows);
    } catch (e) {
      if (my !== reqToken) return;
      showError('Gagal membaca tab sesi: ' + e.message + '. Pastikan Spreadsheet dibagikan "Anyone with the link: Viewer".');
    } finally {
      if (my === reqToken) $('loading').hidden = true;
    }
  }

  // ── Daftar sesi ────────────────────────────────────────────────────
  function sessionLabel(s) {
    const tail = s.finish_ts ? fmtDuration(s.finish_ts - s.start_ts) : 'berjalan / belum ditutup';
    return s.sheet + '  ·  ' + tail;
  }

  async function loadList() {
    const sel = $('session-select');
    sel.disabled = true;
    sel.innerHTML = ''; sel.appendChild(new Option('Memuat daftar sesi...', ''));
    clearError();
    setText('list-hint', '');
    try {
      sessions = await getSessions();
    } catch (e) {
      sel.innerHTML = ''; sel.appendChild(new Option('Gagal memuat daftar sesi', ''));
      showError('Tidak bisa mengambil daftar sesi dari Apps Script (' + e.message + '). Periksa WEB_APP_URL dan pastikan Web App di-deploy untuk "Anyone".');
      return;
    }
    sel.innerHTML = '';
    if (!sessions.length) {
      sel.appendChild(new Option('Belum ada sesi tercatat', ''));
      setText('list-hint', 'Sesi muncul setelah switch di panel digeser ke ON.');
      $('result').hidden = true;
      return;
    }
    sessions.forEach((s, i) => sel.appendChild(new Option(sessionLabel(s), String(i))));
    sel.disabled = false;
    setText('list-hint', sessions.length + ' sesi tersedia (terbaru di atas).');
    loadSession(sessions[0]);
  }

  $('session-select').addEventListener('change', (e) => {
    const s = sessions[Number(e.target.value)];
    if (s) loadSession(s);
  });
  $('btn-refresh').addEventListener('click', loadList);

  loadList();
})();
