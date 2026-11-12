/* common.js — fungsi bersama untuk index.html dan riwayat.html */
(() => {
  'use strict';
  const cfg = window.APP_CONFIG || {};
  const TZ = 'Asia/Jakarta';
  const $ = (id) => document.getElementById(id);

  const fTime  = new Intl.DateTimeFormat('en-GB', { timeZone: TZ, hour: '2-digit', minute: '2-digit', second: '2-digit', hourCycle: 'h23' });
  const fDay   = new Intl.DateTimeFormat('en-GB', { timeZone: TZ, day: '2-digit', month: '2-digit', year: 'numeric' });
  const fLong  = new Intl.DateTimeFormat('id-ID', { timeZone: TZ, weekday: 'long', day: 'numeric', month: 'long', year: 'numeric' });

  function setText(id, text) { const el = $(id); if (el) el.textContent = text; }

  // Angka -> teks tampilan (koma desimal Indonesia). Tidak ada nilai -> "—". Tidak pernah mengarang angka.
  function fmtNum(v, dec) {
    if (typeof v !== 'number' || !isFinite(v)) return '—';
    return v.toLocaleString('id-ID', { minimumFractionDigits: dec, maximumFractionDigits: dec });
  }
  const timeWib = (sec) => fTime.format(new Date(sec * 1000));
  const dateTimeWib = (sec) => fDay.format(new Date(sec * 1000)) + ' ' + timeWib(sec);

  function fmtDuration(sec) {
    sec = Math.max(0, Math.floor(sec));
    const h = Math.floor(sec / 3600), m = Math.floor((sec % 3600) / 60), s = sec % 60;
    return h + ':' + String(m).padStart(2, '0') + ':' + String(s).padStart(2, '0');
  }

  const isPlaceholder = (v) => !v || /ISI_|isi-/i.test(String(v));

  function startClock() {
    const tick = () => {
      const now = new Date();
      setText('current-date', fLong.format(now));
      setText('current-time', fTime.format(now) + ' WIB');
    };
    tick();
    setInterval(tick, 1000);
    setText('current-year', String(new Date().getFullYear()));
  }

  function showConfigError(msg) {
    const box = $('config-error');
    if (!box) return;
    box.hidden = false;
    setText('config-error-text', msg);
  }

  // Grafik garis Chart.js. Nilai null = celah pada garis (sensor tidak terbaca), bukan nol.
  function makeLineChart(canvasId, opts) {
    const canvas = $(canvasId);
    if (!canvas || typeof Chart === 'undefined') return null;
    const datasets = opts.datasets.map((d) => ({
      label: d.label, data: [], borderColor: d.color, backgroundColor: d.color + '22',
      borderWidth: 2, pointRadius: opts.pointRadius || 0, pointHoverRadius: 4,
      tension: 0.25, fill: !!d.fill, spanGaps: false, yAxisID: d.axis || 'y'
    }));
    const scales = {
      x: { grid: { display: false }, ticks: { maxTicksLimit: 8, maxRotation: 0 } },
      y: { position: 'left', title: { display: true, text: opts.yTitle }, min: opts.yMin, max: opts.yMax }
    };
    if (opts.y2Title) scales.y1 = { position: 'right', title: { display: true, text: opts.y2Title }, grid: { drawOnChartArea: false } };
    return new Chart(canvas.getContext('2d'), {
      type: 'line',
      data: { labels: [], datasets },
      options: {
        responsive: true, maintainAspectRatio: false, animation: false,
        interaction: { mode: 'index', intersect: false },
        plugins: { legend: { display: datasets.length > 1 }, tooltip: { callbacks: {
          label: (c) => c.dataset.label + ': ' + (c.parsed.y === null ? 'tidak ada data' : fmtNum(c.parsed.y, opts.decimals))
        } } },
        scales
      }
    });
  }

  window.SMK = { cfg, $, setText, fmtNum, timeWib, dateTimeWib, fmtDuration, isPlaceholder, startClock, showConfigError, makeLineChart, TZ };
})();
