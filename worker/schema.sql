-- Histórico de leituras da chocadeira (Cloudflare D1)
CREATE TABLE IF NOT EXISTS leituras (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  dispositivo  TEXT    NOT NULL,
  ts           INTEGER NOT NULL,          -- quando foi medido (epoch, s) — vem do ESP32
  temp_c       REAL,
  umid_pct     REAL,
  aquecedor    INTEGER NOT NULL DEFAULT 0,
  dia          INTEGER,
  estado       TEXT,
  recebido_em  INTEGER NOT NULL,          -- quando chegou na nuvem (epoch, s)
  UNIQUE (dispositivo, ts)                -- reenvio depois de queda não duplica
);
CREATE INDEX IF NOT EXISTS idx_leituras_disp_ts ON leituras (dispositivo, ts);
