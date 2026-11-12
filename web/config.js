window.APP_CONFIG = {
  DEVICE_ID: "smk-01",                       // sama dengan DEVICE_ID di firmware

  MQTT: {
    URL: "wss://c4610710.ala.asia-southeast1.emqxsl.com:8084/mqtt",    // EMQX Serverless: WSS port 8084, path /mqtt
    USERNAME: "smk_dashboard",
    PASSWORD: "12345678"
  },

  SHEETS: {
    SPREADSHEET_ID: "1ES2vGpXducVwsV04WB3GfeOQMopLnffteZBDfJza7YM",
    WEB_APP_URL: "https://script.google.com/macros/s/AKfycbxvyZJbHJQEhCUH0OMz02N1fqyslg15ZzWOmsY2hQir9VbaRSSjQESRMpIBmFcJf7jx7Q/exec"
  },

  STALE_AFTER_S: 15,        // tanpa telemetry selama ini saat sesi aktif -> tampil "tidak ada data baru"
  CHART_MAX_POINTS: 300     // titik grafik real-time yang disimpan (300 x 4 dtk = 20 menit)
};
