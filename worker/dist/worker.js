// Ninho — Cloudflare Worker (histórico em D1, último estado em KV, painel em /)
// Secrets (Settings → Variables): DEVICE_TOKEN (ESP32 grava) · PAINEL_KEY (painel lê)
// Bindings: DB (D1) · KV (KV namespace)
const PAINEL_HTML = "<!DOCTYPE html>\n<html lang=\"pt-BR\">\n<head>\n<meta charset=\"utf-8\">\n<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>Ninho · Painel da chocadeira</title>\n<script src=\"https://unpkg.com/mqtt@5.10.1/dist/mqtt.min.js\"></script>\n<script src=\"https://cdn.jsdelivr.net/npm/chart.js@4.4.4/dist/chart.umd.min.js\"></script>\n<style>\n  :root{--bg:#f7f3ec;--card:#fffdf9;--ink:#2b2420;--mute:#857a70;--line:#e8dfd2;--amber:#c56a0a;--amber-soft:#fbead2;\n        --ok:#2f7d4a;--ok-soft:#e1f1e6;--bad:#b42318;--bad-soft:#fde4e1;--blue:#2453a6}\n  *{box-sizing:border-box}\n  body{margin:0;font:15px/1.45 system-ui,-apple-system,\"Segoe UI\",Roboto,sans-serif;background:var(--bg);color:var(--ink)}\n  header{display:flex;align-items:center;gap:14px;padding:14px 22px;border-bottom:1px solid var(--line);background:var(--card);position:sticky;top:0;z-index:5}\n  header h1{font-size:18px;margin:0;letter-spacing:.2px}\n  header h1 span{color:var(--amber)}\n  .pill{font-size:12px;padding:3px 10px;border-radius:99px;border:1px solid var(--line);color:var(--mute);white-space:nowrap}\n  .pill.ok{background:var(--ok-soft);color:var(--ok);border-color:transparent}\n  .pill.bad{background:var(--bad-soft);color:var(--bad);border-color:transparent}\n  .pill.warn{background:var(--amber-soft);color:var(--amber);border-color:transparent}\n  .grow{flex:1}\n  main{max-width:1180px;margin:0 auto;padding:20px 22px 40px;display:grid;gap:16px;grid-template-columns:repeat(12,1fr)}\n  .card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px 18px}\n  .card h2{font-size:13px;font-weight:600;text-transform:uppercase;letter-spacing:.6px;color:var(--mute);margin:0 0 10px}\n  .kpis{grid-column:span 12;display:grid;grid-template-columns:repeat(6,1fr);gap:12px}\n  .kpi .v{font-size:28px;font-weight:650;font-variant-numeric:tabular-nums}\n  .kpi .s{font-size:12px;color:var(--mute)}\n  .kpi.alarme{background:var(--bad-soft);border-color:#f3b5ad}\n  .kpi.alarme .v{color:var(--bad)}\n  .grafico{grid-column:span 8}\n  .lado{grid-column:span 4;display:grid;gap:16px;align-content:start}\n  .resumo{grid-column:span 7}\n  .alertas{grid-column:span 5}\n  canvas{width:100%!important;height:290px!important}\n  label{font-size:13px;color:var(--mute)}\n  input{font:inherit;padding:8px 10px;border:1px solid var(--line);border-radius:9px;background:#fff;width:100%}\n  button{font:inherit;font-weight:600;padding:9px 14px;border-radius:9px;border:1px solid var(--amber);background:var(--amber);color:#fff;cursor:pointer}\n  button.sec{background:#fff;color:var(--amber)}\n  button:disabled{opacity:.5;cursor:not-allowed}\n  .linha{display:flex;gap:8px;align-items:end;margin-bottom:10px}\n  .linha > div{flex:1}\n  table{width:100%;border-collapse:collapse;font-size:13px;font-variant-numeric:tabular-nums}\n  th,td{text-align:right;padding:6px 6px;border-bottom:1px solid var(--line)}\n  th:first-child,td:first-child{text-align:left}\n  th{color:var(--mute);font-weight:600}\n  td.fora{color:var(--bad);font-weight:650}\n  .log{list-style:none;margin:0;padding:0;max-height:260px;overflow:auto;font-size:13px}\n  .log li{padding:7px 0;border-bottom:1px solid var(--line);display:flex;gap:8px;align-items:baseline}\n  .log time{color:var(--mute);font-variant-numeric:tabular-nums;min-width:62px}\n  .tag{font-size:11px;font-weight:700;padding:1px 7px;border-radius:6px}\n  .tag.p{background:var(--amber-soft);color:var(--amber)} .tag.ok{background:var(--ok-soft);color:var(--ok)} .tag.bad{background:var(--bad-soft);color:var(--bad)}\n  #login{position:fixed;inset:0;background:rgba(43,36,32,.55);display:flex;align-items:center;justify-content:center;z-index:10}\n  #login .card{width:min(420px,92vw)}\n  #toast{position:fixed;right:18px;bottom:18px;display:grid;gap:8px;z-index:20}\n  #toast div{background:var(--bad);color:#fff;padding:10px 14px;border-radius:10px;box-shadow:0 8px 24px rgba(0,0,0,.18);max-width:340px}\n  .muted{color:var(--mute);font-size:12px}\n  @media (max-width:900px){.kpis{grid-template-columns:repeat(2,1fr)}.grafico,.lado,.resumo,.alertas{grid-column:span 12}}\n</style>\n</head>\n<body>\n<header>\n  <h1>🥚 <span>Ninho</span> · chocadeira1</h1>\n  <span id=\"pDisp\" class=\"pill\">dispositivo: ?</span>\n  <span id=\"pMqtt\" class=\"pill\">tempo real: desconectado</span>\n  <span class=\"grow\"></span>\n  <span id=\"pAtual\" class=\"muted\"></span>\n  <button class=\"sec\" onclick=\"sair()\">Sair</button>\n</header>\n\n<main>\n  <section class=\"kpis\">\n    <div class=\"card kpi\" id=\"kTemp\"><h2>Temperatura</h2><div class=\"v\" id=\"vTemp\">—</div><div class=\"s\">alvo <b id=\"vAlvo\">—</b> · faixa 36,5–38,5 °C</div></div>\n    <div class=\"card kpi\"><h2>Umidade</h2><div class=\"v\" id=\"vUmid\">—</div><div class=\"s\">ideal 50–55 % (65–70 % no fim)</div></div>\n    <div class=\"card kpi\"><h2>Aquecedor</h2><div class=\"v\" id=\"vAquec\">—</div><div class=\"s\">relé no GPIO 26</div></div>\n    <div class=\"card kpi\" id=\"kEstado\"><h2>Estado</h2><div class=\"v\" id=\"vEstado\" style=\"font-size:20px\">—</div><div class=\"s\" id=\"vSil\"> </div></div>\n    <div class=\"card kpi\"><h2>Dia do ciclo</h2><div class=\"v\" id=\"vDia\">—</div><div class=\"s\">de 21 · viragem para no 19</div></div>\n    <div class=\"card kpi\"><h2>Mensagem</h2><div class=\"v\" id=\"vSig\" style=\"font-size:18px\">—</div><div class=\"s\" id=\"vBuf\">assinatura HMAC</div></div>\n  </section>\n\n  <section class=\"card grafico\">\n    <h2>Temperatura · últimas 6 h (histórico D1 + tempo real)</h2>\n    <canvas id=\"graf\"></canvas>\n  </section>\n\n  <section class=\"lado\">\n    <div class=\"card\">\n      <h2>Comandos</h2>\n      <div class=\"linha\">\n        <div><label for=\"iAlvo\">Temperatura alvo (36,5–38,5 °C)</label><input id=\"iAlvo\" type=\"number\" step=\"0.1\" value=\"37.5\"></div>\n        <button onclick=\"enviar('alvo', document.getElementById('iAlvo').value)\">Enviar</button>\n      </div>\n      <div class=\"linha\">\n        <div><label for=\"iDia\">Dia do ciclo (1–21)</label><input id=\"iDia\" type=\"number\" min=\"1\" max=\"21\" value=\"1\"></div>\n        <button onclick=\"enviar('dia', document.getElementById('iDia').value)\">Ajustar</button>\n      </div>\n      <div class=\"linha\">\n        <button class=\"sec\" style=\"flex:1\" onclick=\"enviar('silenciar','')\">Silenciar 10 min</button>\n        <button class=\"sec\" style=\"flex:1\" onclick=\"enviar('virar','')\">Virar ovos agora</button>\n      </div>\n      <p class=\"muted\">\"Executado\" só aparece quando o ESP32 devolve o ack assinado. Sem resposta em 10 s = ESP32 offline ou comando perdido.</p>\n    </div>\n    <div class=\"card\">\n      <h2>Comandos enviados</h2>\n      <ul class=\"log\" id=\"logCmd\"><li class=\"muted\">nenhum ainda</li></ul>\n    </div>\n  </section>\n\n  <section class=\"card resumo\">\n    <h2>Resumo por hora · últimas 24 h (consulta SQL no D1)</h2>\n    <table><thead><tr><th>Hora</th><th>Mín</th><th>Máx</th><th>Média</th><th>Umid.</th><th>Min. fora</th><th>Aquecedor</th></tr></thead>\n      <tbody id=\"tbResumo\"><tr><td colspan=\"7\" class=\"muted\">carregando…</td></tr></tbody></table>\n  </section>\n\n  <section class=\"card alertas\">\n    <h2>Alertas (cooldown no ESP32)</h2>\n    <ul class=\"log\" id=\"logAlerta\"><li class=\"muted\">nenhum alerta</li></ul>\n  </section>\n</main>\n\n<div id=\"login\">\n  <form class=\"card\" onsubmit=\"entrar(event)\">\n    <h2>Entrar no painel</h2>\n    <p class=\"muted\">Digite a chave do painel (PAINEL_KEY). Ela fica só nesta aba e serve para ler o histórico e assinar os comandos.</p>\n    <input id=\"iChave\" type=\"password\" autocomplete=\"current-password\" placeholder=\"chave do painel\" required>\n    <p id=\"loginErro\" class=\"muted\" style=\"color:var(--bad)\"></p>\n    <button type=\"submit\" style=\"width:100%\">Entrar</button>\n  </form>\n</div>\n<div id=\"toast\"></div>\n\n<script>\nconst BASE = \"sis1a/ninho/chocadeira1/\";\nconst BROKER = \"wss://test.mosquitto.org:8081/mqtt\";  // broker público, WebSocket com TLS (Let's Encrypt)\nconst TIMEOUT_ACK_MS = 10000;\nconst COOLDOWN_TOAST_MS = 60000;\nlet CHAVE = sessionStorage.getItem(\"ninho_chave\") || \"\";\nlet chaveCripto = null, cliente = null, grafico = null;\nconst pendentes = new Map();\nconst ultimoToast = {};\nlet ultimaTelemetria = 0, dispOnline = null;\n\nconst $ = id => document.getElementById(id);\nconst hora = ts => new Date(ts * 1000).toLocaleTimeString(\"pt-BR\", { hour: \"2-digit\", minute: \"2-digit\", second: \"2-digit\" });\nconst fmt = (v, d = 1) => (v === null || v === undefined || Number.isNaN(v)) ? \"—\" : Number(v).toFixed(d).replace(\".\", \",\");\n\nasync function prepararChave() {\n  chaveCripto = await crypto.subtle.importKey(\"raw\", new TextEncoder().encode(CHAVE), { name: \"HMAC\", hash: \"SHA-256\" }, false, [\"sign\"]);\n}\nasync function hmac(msg) {\n  const sig = await crypto.subtle.sign(\"HMAC\", chaveCripto, new TextEncoder().encode(msg));\n  return [...new Uint8Array(sig)].map(b => b.toString(16).padStart(2, \"0\")).join(\"\");\n}\nasync function api(caminho) {\n  const r = await fetch(caminho, { headers: { authorization: \"Bearer \" + CHAVE } });\n  if (r.status === 401) throw new Error(\"chave inválida\");\n  return r.json();\n}\n\nasync function entrar(ev) {\n  ev && ev.preventDefault();\n  CHAVE = CHAVE || $(\"iChave\").value.trim();\n  try {\n    await api(\"/api/estado?d=chocadeira1\");\n  } catch (e) {\n    $(\"loginErro\").textContent = \"Chave recusada pelo Worker.\";\n    CHAVE = \"\"; sessionStorage.removeItem(\"ninho_chave\");\n    return;\n  }\n  sessionStorage.setItem(\"ninho_chave\", CHAVE);\n  $(\"login\").style.display = \"none\";\n  await prepararChave();\n  iniciar();\n}\nfunction sair() { sessionStorage.removeItem(\"ninho_chave\"); location.reload(); }\n\nasync function iniciar() {\n  montarGrafico();\n  await Promise.all([carregarEstado(), carregarHistorico(), carregarResumo()]);\n  setInterval(carregarResumo, 60000);\n  setInterval(atualizarFrescor, 2000);\n  conectarMqtt();\n}\n\nasync function carregarEstado() {\n  const e = await api(\"/api/estado?d=chocadeira1\");\n  if (e) mostrar({ ts: e.ts, temp_c: e.temp_c, umid_pct: e.umid_pct, aquecedor: !!e.aquecedor, estado: e.estado, dia: e.dia }, \"histórico\");\n}\nasync function carregarHistorico() {\n  const linhas = await api(\"/api/leituras?d=chocadeira1&horas=6\");\n  grafico.data.datasets[0].data = linhas.filter(l => l.temp_c !== null).map(l => ({ x: l.ts * 1000, y: l.temp_c }));\n  grafico.update();\n}\nasync function carregarResumo() {\n  const linhas = await api(\"/api/resumo?d=chocadeira1&horas=24\");\n  $(\"tbResumo\").innerHTML = linhas.length ? linhas.map(l => `<tr>\n      <td>${l.hora.slice(5).replace(\"-\", \"/\")}</td><td>${fmt(l.temp_min)}</td><td>${fmt(l.temp_max)}</td><td>${fmt(l.temp_media, 2)}</td>\n      <td>${fmt(l.umid_media, 0)}%</td><td class=\"${l.minutos_fora > 0 ? \"fora\" : \"\"}\">${l.minutos_fora}</td><td>${fmt(l.pct_aquecedor, 0)}%</td></tr>`).join(\"\")\n    : `<tr><td colspan=\"7\" class=\"muted\">sem leituras nas últimas 24 h</td></tr>`;\n}\n\nfunction montarGrafico() {\n  const agora = Date.now();\n  grafico = new Chart($(\"graf\"), {\n    type: \"line\",\n    data: { datasets: [\n      { label: \"temp (°C)\", data: [], borderColor: \"#c56a0a\", backgroundColor: \"#c56a0a\", pointRadius: 0, borderWidth: 2, tension: .25 },\n      { label: \"mín 36,5\", data: [], borderColor: \"#b42318\", borderDash: [6, 5], pointRadius: 0, borderWidth: 1 },\n      { label: \"máx 38,5\", data: [], borderColor: \"#b42318\", borderDash: [6, 5], pointRadius: 0, borderWidth: 1 },\n    ] },\n    options: { animation: false, maintainAspectRatio: false, parsing: false,\n      scales: { x: { type: \"linear\", ticks: { callback: v => new Date(v).toLocaleTimeString(\"pt-BR\", { hour: \"2-digit\", minute: \"2-digit\" }) } },\n                y: { suggestedMin: 35.5, suggestedMax: 40 } },\n      plugins: { legend: { labels: { boxWidth: 12 } } } }\n  });\n  atualizarLimites();\n}\nfunction atualizarLimites() {\n  const d = grafico.data.datasets[0].data;\n  const x0 = d.length ? d[0].x : Date.now() - 6 * 3600e3, x1 = Date.now();\n  grafico.data.datasets[1].data = [{ x: x0, y: 36.5 }, { x: x1, y: 36.5 }];\n  grafico.data.datasets[2].data = [{ x: x0, y: 38.5 }, { x: x1, y: 38.5 }];\n}\n\nfunction mostrar(t, origem, assinaturaOk) {\n  $(\"vTemp\").textContent = t.temp_c == null ? \"sem leitura\" : fmt(t.temp_c) + \" °C\";\n  $(\"vUmid\").textContent = t.umid_pct == null ? \"—\" : fmt(t.umid_pct, 0) + \" %\";\n  $(\"vAquec\").textContent = t.aquecedor ? \"ligado 🔥\" : \"desligado\";\n  $(\"vEstado\").textContent = t.estado || \"—\";\n  $(\"vDia\").textContent = t.dia ?? \"—\";\n  if (t.alvo_c != null) $(\"vAlvo\").textContent = fmt(t.alvo_c) + \" °C\";\n  $(\"vSil\").textContent = t.silenciado ? \"buzzer silenciado\" : \" \";\n  const alarme = t.alarme || [\"SUPERAQUECIDO\", \"ALARME_TEMP\", \"FALHA_SENSOR\"].includes(t.estado);\n  $(\"kTemp\").classList.toggle(\"alarme\", !!alarme);\n  $(\"kEstado\").classList.toggle(\"alarme\", !!alarme);\n  if (assinaturaOk !== undefined) {\n    $(\"vSig\").textContent = assinaturaOk ? \"autêntica ✓\" : \"assinatura inválida ✗\";\n    $(\"vSig\").style.color = assinaturaOk ? \"var(--ok)\" : \"var(--bad)\";\n  }\n  if (t.buffer !== undefined) $(\"vBuf\").textContent = t.buffer > 0 ? `${t.buffer} leitura(s) aguardando envio` : \"assinatura HMAC · buffer vazio\";\n  $(\"pAtual\").textContent = `atualizado ${hora(t.ts)} (${origem})`;\n}\n\nfunction atualizarFrescor() {\n  const p = $(\"pDisp\");\n  const recente = Date.now() - ultimaTelemetria < 75000;\n  if (dispOnline === false) { p.className = \"pill bad\"; p.textContent = \"ESP32 offline (LWT)\"; }\n  else if (recente) { p.className = \"pill ok\"; p.textContent = \"ESP32 online\"; }\n  else if (dispOnline) { p.className = \"pill warn\"; p.textContent = \"ESP32 sem telemetria recente\"; }\n  else { p.className = \"pill\"; p.textContent = \"dispositivo: aguardando\"; }\n  for (const [id, c] of pendentes) {\n    if (Date.now() - c.enviado > TIMEOUT_ACK_MS) { pendentes.delete(id); marcar(id, \"bad\", \"sem resposta em 10 s (ESP32 offline?)\"); }\n  }\n}\n\nfunction conectarMqtt() {\n  const id = \"ninho-painel-\" + Math.random().toString(16).slice(2, 8);\n  cliente = mqtt.connect(BROKER, { clientId: id, clean: true, reconnectPeriod: 3000, connectTimeout: 8000 });\n  const pm = $(\"pMqtt\");\n  cliente.on(\"connect\", () => {\n    pm.className = \"pill ok\"; pm.textContent = \"tempo real: WebSocket TLS\";\n    cliente.subscribe([BASE + \"telemetria\", BASE + \"estado\", BASE + \"alerta\", BASE + \"ack\"], { qos: 1 });\n  });\n  cliente.on(\"reconnect\", () => { pm.className = \"pill warn\"; pm.textContent = \"tempo real: reconectando…\"; });\n  cliente.on(\"close\", () => { pm.className = \"pill bad\"; pm.textContent = \"tempo real: desconectado\"; });\n  cliente.on(\"message\", async (topico, payload) => {\n    let m; try { m = JSON.parse(payload.toString()); } catch { return; }\n    const sub = topico.slice(BASE.length);\n    if (sub === \"estado\") { dispOnline = !!m.online; atualizarFrescor(); return; }\n    if (sub === \"telemetria\") {\n      const msg = `${m.ts}|${m.temp_c == null ? \"null\" : Number(m.temp_c).toFixed(1)}|${m.aquecedor ? 1 : 0}|${m.estado}`;\n      const ok = m.sig === await hmac(msg);\n      mostrar(m, \"tempo real\", ok);\n      if (!ok) return;            // telemetria forjada não entra no gráfico\n      ultimaTelemetria = Date.now(); dispOnline = true;\n      if (m.temp_c != null) { grafico.data.datasets[0].data.push({ x: m.ts * 1000, y: m.temp_c }); atualizarLimites(); grafico.update(); }\n      if (m.alvo_c != null) $(\"iAlvo\").placeholder = m.alvo_c;\n    }\n    if (sub === \"alerta\") {\n      const ok = m.sig === await hmac(`${m.ts}|${m.tipo}`);\n      if (!ok) return;\n      adicionarAlerta(m);\n    }\n    if (sub === \"ack\") {\n      const ok = m.sig === await hmac(`${m.id}|${m.ok ? 1 : 0}|${m.ts}`);\n      if (!ok || !pendentes.has(m.id)) return;\n      pendentes.delete(m.id);\n      marcar(m.id, m.ok ? \"ok\" : \"bad\", (m.ok ? \"executado: \" : \"recusado pelo ESP32: \") + m.resultado);\n    }\n  });\n}\n\nasync function enviar(cmd, valor) {\n  if (!cliente || !cliente.connected) return toast(\"Sem conexão com o broker.\");\n  const id = \"c\" + Date.now().toString(36) + Math.random().toString(36).slice(2, 5);\n  const ts = Math.floor(Date.now() / 1000);\n  valor = String(valor ?? \"\").trim();\n  const sig = await hmac(`${id}|${cmd}|${valor}|${ts}`);\n  cliente.publish(BASE + \"comando\", JSON.stringify({ id, cmd, valor, ts, sig }), { qos: 1 });\n  pendentes.set(id, { enviado: Date.now() });\n  const ul = $(\"logCmd\");\n  if (ul.querySelector(\".muted\")) ul.innerHTML = \"\";\n  ul.insertAdjacentHTML(\"afterbegin\", `<li id=\"${id}\"><time>${hora(ts)}</time><span class=\"tag p\">enviado</span><span>${cmd}${valor ? \" = \" + valor : \"\"} · aguardando ack…</span></li>`);\n}\nfunction marcar(id, cls, texto) {\n  const li = $(id); if (!li) return;\n  li.querySelector(\".tag\").className = \"tag \" + cls;\n  li.querySelector(\".tag\").textContent = cls === \"ok\" ? \"executado\" : \"falhou\";\n  const s = li.querySelectorAll(\"span\")[1]; s.textContent = s.textContent.split(\" · \")[0] + \" · \" + texto;\n}\n\nfunction adicionarAlerta(a) {\n  const ul = $(\"logAlerta\");\n  if (ul.querySelector(\".muted\")) ul.innerHTML = \"\";\n  ul.insertAdjacentHTML(\"afterbegin\", `<li><time>${hora(a.ts)}</time><span class=\"tag bad\">${a.tipo}</span><span>${a.msg}${a.temp_c != null ? \" · \" + fmt(a.temp_c) + \" °C\" : \"\"}</span></li>`);\n  // cooldown também no painel: o mesmo tipo não pisca de novo antes de 1 min\n  if (!ultimoToast[a.tipo] || Date.now() - ultimoToast[a.tipo] > COOLDOWN_TOAST_MS) {\n    ultimoToast[a.tipo] = Date.now();\n    toast(\"⚠ \" + a.msg);\n    if (\"Notification\" in window && Notification.permission === \"granted\") new Notification(\"Ninho: \" + a.tipo, { body: a.msg });\n  }\n}\nfunction toast(texto) {\n  const d = document.createElement(\"div\"); d.textContent = texto; $(\"toast\").appendChild(d);\n  setTimeout(() => d.remove(), 6000);\n}\n\nif (\"Notification\" in window && Notification.permission === \"default\") Notification.requestPermission();\nif (CHAVE) entrar();\n</script>\n</body>\n</html>\n";

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
