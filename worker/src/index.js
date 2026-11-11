// Ninho — Cloudflare Worker (histórico em D1, último estado em KV, painel em /)
// Secrets (Settings → Variables): DEVICE_TOKEN (ESP32 grava) · PAINEL_KEY (painel lê)
// Bindings: DB (D1) · KV (KV namespace)
import PAINEL_HTML from "./painel.html";

const RETENCAO_DIAS = 180;       // sem dado pessoal: guarda 6 meses (≈ 8 ciclos de 21 dias)
const LOTE_MAX = 100;
const FAIXA_MIN = 36.5, FAIXA_MAX = 38.5;

const json = (obj, status = 200) =>
  new Response(JSON.stringify(obj), {
    status,
    headers: { "content-type": "application/json; charset=utf-8", "cache-control": "no-store" },
  });

// comparação em tempo constante (não vaza o token pelo tempo de resposta)
function iguais(a, b) {
  const ea = new TextEncoder().encode(a), eb = new TextEncoder().encode(b);
  let d = ea.length ^ eb.length;
  for (let i = 0; i < Math.max(ea.length, eb.length); i++) d |= (ea[i] || 0) ^ (eb[i] || 0);
  return d === 0;
}
function autorizado(req, segredo) {
  const h = req.headers.get("authorization") || "";
  return !!segredo && h.startsWith("Bearer ") && iguais(h.slice(7), segredo);
}
const num = (v, min, max) => (typeof v === "number" && isFinite(v) && v >= min && v <= max ? v : null);

async function gravarLeituras(req, env) {
  if (!autorizado(req, env.DEVICE_TOKEN)) return json({ erro: "nao autorizado" }, 401);
  let body;
  try { body = await req.json(); } catch { return json({ erro: "JSON invalido" }, 400); }
  const disp = String(body.dispositivo || "").slice(0, 32);
  const lista = Array.isArray(body.leituras) ? body.leituras : [];
  if (!/^[a-z0-9_-]+$/.test(disp) || lista.length === 0 || lista.length > LOTE_MAX)
    return json({ erro: "lote invalido" }, 400);

  const agora = Math.floor(Date.now() / 1000);
  const stmt = env.DB.prepare(
    "INSERT OR IGNORE INTO leituras (dispositivo, ts, temp_c, umid_pct, aquecedor, dia, estado, recebido_em) VALUES (?,?,?,?,?,?,?,?)"
  );
  const lote = [];
  let ultima = null;
  for (const l of lista) {
    const ts = num(l.ts, 1700000000, agora + 300);
    if (ts === null) continue;
    const linha = {
      ts,
      temp_c: num(l.temp_c, -40, 80),
      umid_pct: num(l.umid_pct, 0, 100),
      aquecedor: l.aquecedor ? 1 : 0,
      dia: num(l.dia, 1, 21),
      estado: String(l.estado || "").slice(0, 24),
    };
    lote.push(stmt.bind(disp, linha.ts, linha.temp_c, linha.umid_pct, linha.aquecedor, linha.dia, linha.estado, agora));
    if (!ultima || linha.ts > ultima.ts) ultima = linha;
  }
  if (!lote.length) return json({ erro: "nenhuma leitura valida" }, 400);
  const res = await env.DB.batch(lote);
  const gravadas = res.reduce((s, r) => s + (r.meta?.changes || 0), 0);

  // KV guarda só o mais recente (o painel abre rápido mesmo sem MQTT)
  const atual = JSON.parse((await env.KV.get(`estado:${disp}`)) || "{}");
  if (!atual.ts || ultima.ts >= atual.ts)
    await env.KV.put(`estado:${disp}`, JSON.stringify({ ...ultima, recebido_em: agora, atrasada: agora - ultima.ts > 120 }));

  return json({ ok: true, recebidas: lista.length, gravadas, duplicadas: lote.length - gravadas });
}

async function consultar(url, env) {
  const disp = url.searchParams.get("d") || "chocadeira1";
  const horas = Math.min(Math.max(parseInt(url.searchParams.get("horas") || "24", 10) || 24, 1), 24 * 30);
  const desde = Math.floor(Date.now() / 1000) - horas * 3600;

  if (url.pathname === "/api/estado") {
    return json(JSON.parse((await env.KV.get(`estado:${disp}`)) || "null"));
  }
  if (url.pathname === "/api/leituras") {
    const { results } = await env.DB.prepare(
      "SELECT ts, temp_c, umid_pct, aquecedor, estado FROM leituras WHERE dispositivo = ? AND ts >= ? ORDER BY ts LIMIT 5000"
    ).bind(disp, desde).all();
    return json(results);
  }
  if (url.pathname === "/api/resumo") {
    // Pergunta: em quais horas a chocadeira saiu da faixa (36,5–38,5 °C) e por quantos minutos?
    const { results } = await env.DB.prepare(
      `SELECT strftime('%Y-%m-%d %H:00', ts, 'unixepoch', '-3 hours') AS hora,
              COUNT(*)                                     AS leituras,
              ROUND(MIN(temp_c), 1)                        AS temp_min,
              ROUND(MAX(temp_c), 1)                        AS temp_max,
              ROUND(AVG(temp_c), 2)                        AS temp_media,
              ROUND(AVG(umid_pct), 0)                      AS umid_media,
              SUM(CASE WHEN temp_c < ?3 OR temp_c > ?4 OR temp_c IS NULL THEN 1 ELSE 0 END) AS minutos_fora,
              ROUND(100.0 * AVG(aquecedor), 0)             AS pct_aquecedor
         FROM leituras
        WHERE dispositivo = ?1 AND ts >= ?2
        GROUP BY hora
        ORDER BY hora DESC`
    ).bind(disp, desde, FAIXA_MIN, FAIXA_MAX).all();
    return json(results);
  }
  return json({ erro: "rota nao encontrada" }, 404);
}

export default {
  async fetch(req, env) {
    const url = new URL(req.url);
    if (req.method === "GET" && (url.pathname === "/" || url.pathname === "/index.html"))
      return new Response(PAINEL_HTML, { headers: { "content-type": "text/html; charset=utf-8" } });

    if (url.pathname === "/api/leituras" && req.method === "POST") return gravarLeituras(req, env);

    if (url.pathname.startsWith("/api/") && req.method === "GET") {
      if (!autorizado(req, env.PAINEL_KEY)) return json({ erro: "nao autorizado" }, 401);
      return consultar(url, env);
    }
    return json({ erro: "rota nao encontrada" }, 404);
  },

  // Cron diário: apaga o que passou do prazo de retenção
  async scheduled(_evt, env) {
    const limite = Math.floor(Date.now() / 1000) - RETENCAO_DIAS * 86400;
    await env.DB.prepare("DELETE FROM leituras WHERE ts < ?").bind(limite).run();
  },
};
