/*
  Ninho — chocadeira inteligente (IoT N2 · AMF · Prof. Leonam V. Hemann)
  Brayan Martins · Mell Campos

  Arquitetura do firmware
  - loop() (núcleo 1): SENTIR → DECIDIR → AGIR. Não depende da rede.
  - tarefaRede (núcleo 0): Wi-Fi, NTP, MQTT com TLS (8883) e HTTPS para o Worker.
    Se a internet cair, o controle continua e as leituras ficam num buffer de 6 h.

  Pinos: DHT22=15 · Relé (aquecedor)=26 · Servo (virador)=13 · Buzzer=23 · LED alarme=4

  Tópicos (sis1a/<grupo>/<local>/...):
    sis1a/ninho/chocadeira1/telemetria   ESP32 publica a cada 30 s
    sis1a/ninho/chocadeira1/estado       ESP32 publica (retido) online/offline + LWT
    sis1a/ninho/chocadeira1/alerta       ESP32 publica quando um alarme começa (cooldown 10 min)
    sis1a/ninho/chocadeira1/comando      painel publica · ESP32 assina
    sis1a/ninho/chocadeira1/ack          ESP32 publica a confirmação de cada comando

  Comandos pelo Serial Monitor (teste local, sem assinatura):
    s silenciar · v virar · f falha simulada · d +1 dia · r reiniciar ciclo · a37.8 alvo · w liga/desliga Wi-Fi
*/
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <DHTesp.h>
#include <ESP32Servo.h>
#include <Preferences.h>
#include <time.h>
#include "mbedtls/md.h"
#include "certs.h"
#include "secrets.h"

// ---------------- Pinos ----------------
const int PIN_DHT    = 15;
const int PIN_RELE   = 26;
const int PIN_SERVO  = 13;
const int PIN_BUZZER = 23;
const int PIN_LED    = 4;

// ---------------- Modo demonstração ----------------
#define DEMO 1
#if DEMO
const unsigned long TEMPO_ALARME_MS    = 20UL * 1000;       // real: 2 min
const unsigned long TEMPO_UMIDADE_MS   = 30UL * 1000;       // real: 10 min
const unsigned long INTERVALO_VIRAR_MS = 30UL * 1000;       // real: 1 h
const unsigned long DURACAO_DIA_MS     = 3UL * 60 * 1000;   // real: 24 h
const unsigned long COOLDOWN_ALERTA_MS = 60UL * 1000;       // real: 10 min
#else
const unsigned long TEMPO_ALARME_MS    = 2UL * 60 * 1000;
const unsigned long TEMPO_UMIDADE_MS   = 10UL * 60 * 1000;
const unsigned long INTERVALO_VIRAR_MS = 60UL * 60 * 1000;
const unsigned long DURACAO_DIA_MS     = 24UL * 60 * 60 * 1000;
const unsigned long COOLDOWN_ALERTA_MS = 10UL * 60 * 1000;
#endif

// ---------------- Limites ----------------
const float ALVO_MIN = 36.5, ALVO_MAX = 38.5;
const float HIST_AQUEC = 0.2;      // liga < alvo-0,2 · desliga > alvo+0,2
const float FAIXA_ALARME = 1.0;    // alarme fora de alvo ± 1,0
const float FAIXA_RESET  = 0.5;    // alarme desliga dentro de alvo ± 0,5
const float T_CORTE = 39.5;        // superaquecimento: corta na hora
const int   DIA_PARAR_VIRAR = 19;
const int   DIAS_CICLO = 21;
const unsigned long LEITURA_MS  = 2000;
const unsigned long SILENCIO_MS = 10UL * 60 * 1000;
const int FALHAS_MAX = 3;

// ---------------- Rede ----------------
const char* MQTT_HOST = "broker.hivemq.com";   // broker público e gratuito, com TLS
const int   MQTT_PORTA = 8883;
const char* DISPOSITIVO = "chocadeira1";
#define TOPICO(sufixo) "sis1a/ninho/chocadeira1/" sufixo
const unsigned long TELEMETRIA_MS = 30UL * 1000;
const unsigned long HISTORICO_MS  = 60UL * 1000;
const int BUFFER_MAX = 360;          // 1 leitura/min → 6 h sem internet
const int LOTE_MAX   = 30;           // leituras por envio
const long COMANDO_VALIDADE_S = 120; // comando mais velho que isso é recusado (anti-replay)

// ======================================================================
// Estado do controle (núcleo 1)
// ======================================================================
DHTesp dht;
Servo virador;
Preferences nvs;

float alvo = 37.5;
int diaCiclo = 1;
float tempC = NAN, umid = NAN;
int falhasSeguidas = 0;
bool falhaSensor = false, falhaSimulada = false;
bool aquecedor = false, alarmeTemp = false, superaquecido = false;
unsigned long foraDesde = 0;
bool avisoUmidade = false;
unsigned long umidForaDesde = 0;
int anguloServo = 45;
unsigned long ultimaViragem = 0, inicioDia = 0, silenciadoAte = 0, ultimaLeitura = 0;

// ======================================================================
// Comunicação entre os núcleos
// ======================================================================
struct Retrato {               // cópia do estado para a rede publicar
  float temp, umid, alvo;
  bool aquecedor, alarme, silenciado, falha;
  int dia;
  char estado[24];
};
struct Comando { char id[24]; char cmd[16]; float valor; };
struct Ack     { char id[24]; bool ok; char resultado[48]; };
struct Alerta  { char tipo[20]; char msg[64]; float temp; };
struct Leitura { uint32_t ts; float temp, umid; uint8_t aquecedor; uint8_t dia; char estado[20]; };

SemaphoreHandle_t mtx;
Retrato retrato;
QueueHandle_t filaComandos, filaAcks, filaAlertas;
volatile bool wifiDesligadoTeste = false;

// ---------------- Flash (NVS) ----------------
void carregarConfig() {
  nvs.begin("ninho", true);
  alvo = nvs.getFloat("alvo", 37.5);
  diaCiclo = nvs.getInt("dia", 1);
  nvs.end();
  if (alvo < ALVO_MIN || alvo > ALVO_MAX) alvo = 37.5;
  if (diaCiclo < 1 || diaCiclo > DIAS_CICLO) diaCiclo = 1;
}
void salvarConfig() {
  nvs.begin("ninho", false);
  nvs.putFloat("alvo", alvo);
  nvs.putInt("dia", diaCiclo);
  nvs.end();
}

// ---------------- Sentir ----------------
void lerSensor() {
  if (falhaSimulada) { tempC = NAN; umid = NAN; return; }
  TempAndHumidity v = dht.getTempAndHumidity();
  if (dht.getStatus() != DHTesp::ERROR_NONE) { tempC = NAN; umid = NAN; return; }
  tempC = v.temperature;
  umid = v.humidity;
}

// ---------------- Alertas (com cooldown, no próprio ESP32) ----------------
unsigned long ultimoAlerta[5] = {0};
const char* TIPOS_ALERTA[5] = {"superaquecido", "temp_alta", "temp_baixa", "falha_sensor", "umidade"};

void emitirAlerta(int i, const char* msg) {
  unsigned long agora = millis();
  if (ultimoAlerta[i] != 0 && agora - ultimoAlerta[i] < COOLDOWN_ALERTA_MS) return;  // cooldown
  ultimoAlerta[i] = agora;
  Alerta a;
  strlcpy(a.tipo, TIPOS_ALERTA[i], sizeof(a.tipo));
  strlcpy(a.msg, msg, sizeof(a.msg));
  a.temp = tempC;
  xQueueSend(filaAlertas, &a, 0);
  Serial.printf(">> ALERTA %s: %s\n", a.tipo, a.msg);
}

// ---------------- Decidir ----------------
void decidirTemperatura(unsigned long agora) {
  if (isnan(tempC)) {
    if (++falhasSeguidas >= FALHAS_MAX && !falhaSensor) {
      falhaSensor = true;
      emitirAlerta(3, "Sensor sem leitura: aquecedor desligado por seguranca");
    }
    if (falhaSensor) aquecedor = false;  // estado seguro
    return;
  }
  falhasSeguidas = 0;
  falhaSensor = false;

  superaquecido = tempC >= T_CORTE;
  if (superaquecido)                  aquecedor = false;
  else if (tempC < alvo - HIST_AQUEC) aquecedor = true;
  else if (tempC > alvo + HIST_AQUEC) aquecedor = false;

  bool fora = tempC < alvo - FAIXA_ALARME || tempC > alvo + FAIXA_ALARME;
  bool antes = alarmeTemp;
  if (superaquecido) {
    alarmeTemp = true;
    if (!antes) emitirAlerta(0, "Superaquecimento: aquecedor cortado");
  } else if (fora) {
    if (foraDesde == 0) foraDesde = agora;
    if (agora - foraDesde >= TEMPO_ALARME_MS) {
      alarmeTemp = true;
      if (!antes) emitirAlerta(tempC > alvo ? 1 : 2, tempC > alvo ? "Temperatura alta" : "Temperatura baixa");
    }
  } else if (fabs(tempC - alvo) <= FAIXA_RESET) {
    foraDesde = 0;
    alarmeTemp = false;
  }
}

void decidirUmidade(unsigned long agora) {
  if (isnan(umid)) return;
  float uMin = diaCiclo < DIA_PARAR_VIRAR ? 45 : 60;
  float uMax = diaCiclo < DIA_PARAR_VIRAR ? 60 : 75;
  if (umid < uMin || umid > uMax) {
    if (umidForaDesde == 0) umidForaDesde = agora;
    if (agora - umidForaDesde >= TEMPO_UMIDADE_MS) {
      if (!avisoUmidade) emitirAlerta(4, umid < uMin ? "Umidade baixa: adicionar agua" : "Umidade alta: abrir ventilacao");
      avisoUmidade = true;
    }
  } else {
    umidForaDesde = 0;
    avisoUmidade = false;
  }
}

void virarOvos(unsigned long agora, const char* motivo) {
  anguloServo = (anguloServo == 45) ? 135 : 45;
  virador.write(anguloServo);
  ultimaViragem = agora;
  Serial.printf(">> viragem (%s): bandeja em %d°\n", motivo, anguloServo);
}

void decidirViragem(unsigned long agora) {
  if (diaCiclo >= DIA_PARAR_VIRAR) return;
  if (agora - ultimaViragem >= INTERVALO_VIRAR_MS) virarOvos(agora, "automatica");
}

void avancarDia(unsigned long agora) {
  inicioDia = agora;
  if (diaCiclo < DIAS_CICLO) diaCiclo++;
  salvarConfig();
  Serial.printf(">> dia %d do ciclo%s\n", diaCiclo, diaCiclo >= DIA_PARAR_VIRAR ? " (viragem parada)" : "");
}

// ---------------- Agir ----------------
bool buzzerTocando = false;
void buzzer(bool ligar) {
  if (ligar == buzzerTocando) return;
  buzzerTocando = ligar;
  if (ligar) tone(PIN_BUZZER, 2000); else noTone(PIN_BUZZER);
}

void agir(unsigned long agora) {
  digitalWrite(PIN_RELE, aquecedor ? HIGH : LOW);
  if (falhaSensor && !alarmeTemp) digitalWrite(PIN_LED, (agora / 200) % 2);
  else                            digitalWrite(PIN_LED, alarmeTemp);
  bool som = false;
  if (agora >= silenciadoAte) {
    if (alarmeTemp)       som = true;
    else if (falhaSensor) som = (agora % 5000) < 150;
  }
  buzzer(som);
}

const char* nomeEstado() {
  if (falhaSensor)   return "FALHA_SENSOR";
  if (superaquecido) return "SUPERAQUECIDO";
  if (alarmeTemp)    return "ALARME_TEMP";
  if (foraDesde)     return "FORA_FAIXA_CONTANDO";
  return "NORMAL";
}

// ---------------- Comandos (validados aqui, no núcleo do controle) ----------------
bool executarComando(const char* cmd, float valor, char* resultado, size_t n, unsigned long agora) {
  if (!strcmp(cmd, "silenciar")) {
    silenciadoAte = agora + SILENCIO_MS;
    snprintf(resultado, n, "buzzer silenciado por 10 min");
  } else if (!strcmp(cmd, "virar")) {
    if (diaCiclo >= DIA_PARAR_VIRAR) { snprintf(resultado, n, "viragem parada (dia %d)", diaCiclo); return false; }
    virarOvos(agora, "comando");
    snprintf(resultado, n, "bandeja em %d graus", anguloServo);
  } else if (!strcmp(cmd, "alvo")) {
    if (isnan(valor) || valor < ALVO_MIN || valor > ALVO_MAX) {
      snprintf(resultado, n, "alvo %.1f fora de %.1f-%.1f", valor, ALVO_MIN, ALVO_MAX);
      return false;
    }
    alvo = valor;
    salvarConfig();
    snprintf(resultado, n, "alvo = %.1f C", alvo);
  } else if (!strcmp(cmd, "dia")) {
    int d = (int)valor;
    if (d < 1 || d > DIAS_CICLO) { snprintf(resultado, n, "dia %d invalido", d); return false; }
    diaCiclo = d; inicioDia = agora;
    salvarConfig();
    snprintf(resultado, n, "dia do ciclo = %d", diaCiclo);
  } else {
    snprintf(resultado, n, "comando desconhecido");
    return false;
  }
  return true;
}

void tratarComandosRemotos(unsigned long agora) {
  Comando c;
  while (xQueueReceive(filaComandos, &c, 0) == pdTRUE) {
    Ack a;
    strlcpy(a.id, c.id, sizeof(a.id));
    a.ok = executarComando(c.cmd, c.valor, a.resultado, sizeof(a.resultado), agora);
    Serial.printf(">> comando remoto %s (%s): %s\n", c.cmd, a.ok ? "OK" : "RECUSADO", a.resultado);
    xQueueSend(filaAcks, &a, 0);
  }
}

String linhaSerial;
void tratarComandoSerial(String cmd, unsigned long agora) {
  cmd.trim();
  if (!cmd.length()) return;
  char r[48];
  char c = cmd.charAt(0);
  if (c == 's')      { executarComando("silenciar", NAN, r, sizeof(r), agora); Serial.printf(">> %s\n", r); }
  else if (c == 'v') { executarComando("virar", NAN, r, sizeof(r), agora); }
  else if (c == 'f') { falhaSimulada = !falhaSimulada; Serial.printf(">> falha simulada do DHT22: %s\n", falhaSimulada ? "ON" : "OFF"); }
  else if (c == 'd') { avancarDia(agora); }
  else if (c == 'r') { diaCiclo = 0; avancarDia(agora); }
  else if (c == 'w') { wifiDesligadoTeste = !wifiDesligadoTeste; Serial.printf(">> teste sem internet: %s\n", wifiDesligadoTeste ? "Wi-Fi DESLIGADO" : "Wi-Fi LIGADO"); }
  else if (c == 'a') {
    bool ok = executarComando("alvo", cmd.substring(1).toFloat(), r, sizeof(r), agora);
    Serial.printf(">> %s%s\n", ok ? "" : "REJEITADO: ", r);
  }
}
void lerSerial(unsigned long agora) {
  while (Serial.available()) {
    char ch = Serial.read();
    if (ch == '\n' || ch == '\r') { tratarComandoSerial(linhaSerial, agora); linhaSerial = ""; }
    else linhaSerial += ch;
  }
}

void atualizarRetrato() {
  xSemaphoreTake(mtx, portMAX_DELAY);
  retrato.temp = tempC; retrato.umid = umid; retrato.alvo = alvo;
  retrato.aquecedor = aquecedor; retrato.alarme = alarmeTemp; retrato.falha = falhaSensor;
  retrato.silenciado = millis() < silenciadoAte;
  retrato.dia = diaCiclo;
  strlcpy(retrato.estado, nomeEstado(), sizeof(retrato.estado));
  xSemaphoreGive(mtx);
}

void logar(unsigned long agora) {
  Serial.printf("[%6lus] dia=%d alvo=%.1f temp_c=%s umid_pct=%s aquecedor=%s estado=%s%s%s\n",
    agora / 1000, diaCiclo, alvo,
    isnan(tempC) ? "NaN" : String(tempC, 1).c_str(),
    isnan(umid)  ? "NaN" : String(umid, 0).c_str(),
    aquecedor ? "ON" : "OFF", nomeEstado(),
    avisoUmidade ? " AVISO_UMIDADE" : "",
    agora < silenciadoAte ? " (silenciado)" : "");
}

// ======================================================================
// REDE (núcleo 0)
// ======================================================================
WiFiClientSecure tlsMqtt;
PubSubClient mqtt(tlsMqtt);
Leitura buffer[BUFFER_MAX];
int bufIni = 0, bufQtd = 0;
uint32_t idsVistos[16]; int idsPos = 0;
char clientId[32];

// HMAC-SHA256 em hexadecimal
void hmacHex(const char* msg, char* saida65) {
  uint8_t mac[32];
  const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, (const uint8_t*)PAINEL_KEY, strlen(PAINEL_KEY), (const uint8_t*)msg, strlen(msg), mac);
  for (int i = 0; i < 32; i++) sprintf(saida65 + i * 2, "%02x", mac[i]);
  saida65[64] = 0;
}
bool iguaisTempoConstante(const char* a, const char* b) {
  size_t n = strlen(a); if (n != strlen(b)) return false;
  uint8_t d = 0; for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}
uint32_t agoraEpoch() { time_t t; time(&t); return (uint32_t)t; }
bool relogioOk() { return agoraEpoch() > 1700000000; }
uint32_t hashId(const char* s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }

void publicar(const char* topico, JsonDocument& doc, bool retido = false) {
  char buf[384];
  size_t n = serializeJson(doc, buf, sizeof(buf));
  mqtt.publish(topico, (const uint8_t*)buf, n, retido);
}

// Comando: {"id":"c1","cmd":"alvo","valor":"37.8","ts":1731280000,"sig":"<hmac de id|cmd|valor|ts>"}
void aoReceber(char* topico, byte* payload, unsigned int len) {
  JsonDocument doc;
  if (deserializeJson(doc, payload, len)) { Serial.println(">> comando ignorado: JSON invalido"); return; }
  const char* id = doc["id"] | "";
  const char* cmd = doc["cmd"] | "";
  const char* valorTxt = doc["valor"] | "";
  uint32_t ts = doc["ts"] | 0;
  const char* sig = doc["sig"] | "";

  char msg[96], esperado[65];
  snprintf(msg, sizeof(msg), "%s|%s|%s|%lu", id, cmd, valorTxt, (unsigned long)ts);
  hmacHex(msg, esperado);

  const char* motivo = nullptr;
  if (!strlen(id) || !strlen(cmd))                                motivo = "campos faltando";
  else if (!iguaisTempoConstante(sig, esperado))                  motivo = "assinatura invalida";
  else if (!relogioOk() || labs((long)agoraEpoch() - (long)ts) > COMANDO_VALIDADE_S) motivo = "comando expirado";
  else {
    uint32_t h = hashId(id);
    for (int i = 0; i < 16; i++) if (idsVistos[i] == h) motivo = "comando repetido";
    if (!motivo) { idsVistos[idsPos] = h; idsPos = (idsPos + 1) % 16; }
  }
  if (motivo) {
    Serial.printf(">> COMANDO RECUSADO (%s): %s\n", motivo, cmd);
    Ack a; strlcpy(a.id, id, sizeof(a.id)); a.ok = false; snprintf(a.resultado, sizeof(a.resultado), "recusado: %s", motivo);
    xQueueSend(filaAcks, &a, 0);
    return;
  }
  Comando c;
  strlcpy(c.id, id, sizeof(c.id));
  strlcpy(c.cmd, cmd, sizeof(c.cmd));
  c.valor = strlen(valorTxt) ? atof(valorTxt) : NAN;
  xQueueSend(filaComandos, &c, 0);   // o núcleo do controle executa e responde o ack
}

bool conectarMqtt() {
  JsonDocument lwt; lwt["online"] = false; lwt["dispositivo"] = DISPOSITIVO;
  char lwtBuf[96]; serializeJson(lwt, lwtBuf);
  // usuário/senha vazios: broker público. A segurança dos comandos é a assinatura HMAC.
  if (!mqtt.connect(clientId, nullptr, nullptr, TOPICO("estado"), 1, true, lwtBuf)) return false;
  JsonDocument on; on["online"] = true; on["dispositivo"] = DISPOSITIVO; on["ts"] = agoraEpoch();
  publicar(TOPICO("estado"), on, true);
  mqtt.subscribe(TOPICO("comando"), 1);
  return true;
}

void guardarLeitura() {
  if (!relogioOk()) return;   // sem hora certa não dá para provar quando foi
  Retrato r;
  xSemaphoreTake(mtx, portMAX_DELAY); r = retrato; xSemaphoreGive(mtx);
  Leitura l;
  l.ts = agoraEpoch(); l.temp = r.temp; l.umid = r.umid; l.aquecedor = r.aquecedor; l.dia = r.dia;
  strlcpy(l.estado, r.estado, sizeof(l.estado));
  int pos = (bufIni + bufQtd) % BUFFER_MAX;
  buffer[pos] = l;
  if (bufQtd < BUFFER_MAX) bufQtd++; else bufIni = (bufIni + 1) % BUFFER_MAX;  // cheio: descarta a mais antiga
}

// Envia até LOTE_MAX leituras do buffer. Só apaga depois do 200 OK.
bool enviarLote() {
  if (bufQtd == 0) return true;
  int n = min(bufQtd, LOTE_MAX);
  JsonDocument doc;
  doc["dispositivo"] = DISPOSITIVO;
  JsonArray arr = doc["leituras"].to<JsonArray>();
  for (int i = 0; i < n; i++) {
    Leitura& l = buffer[(bufIni + i) % BUFFER_MAX];
    JsonObject o = arr.add<JsonObject>();
    o["ts"] = l.ts;
    if (!isnan(l.temp)) o["temp_c"] = round(l.temp * 10) / 10.0;
    if (!isnan(l.umid)) o["umid_pct"] = round(l.umid);
    o["aquecedor"] = l.aquecedor;
    o["dia"] = l.dia;
    o["estado"] = l.estado;
  }
  String corpo; serializeJson(doc, corpo);

  WiFiClientSecure tls;
  tls.setCACert(CA_BUNDLE);           // confere o certificado do Worker (sem setInsecure)
  HTTPClient http;
  http.setTimeout(8000);
  if (!http.begin(tls, String(WORKER_URL) + "/api/leituras")) return false;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("Authorization", String("Bearer ") + DEVICE_TOKEN);
  int code = http.POST(corpo);
  http.end();
  if (code == 200) {
    bufIni = (bufIni + n) % BUFFER_MAX;
    bufQtd -= n;
    Serial.printf(">> historico: %d leitura(s) gravada(s) no Worker (restam %d no buffer)\n", n, bufQtd);
    return true;
  }
  Serial.printf(">> historico: falha HTTP %d (mantidas %d no buffer)\n", code, bufQtd);
  return false;
}

void publicarTelemetria() {
  Retrato r;
  xSemaphoreTake(mtx, portMAX_DELAY); r = retrato; xSemaphoreGive(mtx);
  JsonDocument d;
  uint32_t ts = agoraEpoch();
  d["ts"] = ts;
  if (!isnan(r.temp)) d["temp_c"] = round(r.temp * 10) / 10.0;
  if (!isnan(r.umid)) d["umid_pct"] = round(r.umid);
  d["aquecedor"] = r.aquecedor;
  d["alarme"] = r.alarme;
  d["silenciado"] = r.silenciado;
  d["alvo_c"] = r.alvo;
  d["dia"] = r.dia;
  d["estado"] = r.estado;
  d["buffer"] = bufQtd;
  char msg[96], sig[65];
  snprintf(msg, sizeof(msg), "%lu|%s|%d|%s", (unsigned long)ts,
           isnan(r.temp) ? "null" : String(round(r.temp * 10) / 10.0, 1).c_str(), r.aquecedor ? 1 : 0, r.estado);
  hmacHex(msg, sig);
  d["sig"] = sig;
  publicar(TOPICO("telemetria"), d);
}

void tarefaRede(void*) {
  uint64_t mac = ESP.getEfuseMac();
  snprintf(clientId, sizeof(clientId), "ninho-%04x%08x", (uint16_t)(mac >> 32), (uint32_t)mac);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);
  Serial.println(">> conectando ao Wi-Fi " WIFI_SSID "...");
  tlsMqtt.setCACert(CA_BUNDLE);       // confere o certificado do broker (sem setInsecure)
  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setCallback(aoReceber);
  mqtt.setBufferSize(512);
  mqtt.setKeepAlive(30);

  unsigned long proxTentativa = 0, espera = 1000, proxWifi = 30000;
  unsigned long ultTelemetria = 0, ultHistorico = 0, ultEnvio = 0;
  bool estavaOnline = false;

  for (;;) {
    unsigned long agora = millis();

    // Teste "sem internet" (comando w): derruba o Wi-Fi, mas o controle segue.
    if (wifiDesligadoTeste) {
      if (WiFi.status() == WL_CONNECTED) { mqtt.disconnect(); WiFi.disconnect(); }
    } else if (WiFi.status() != WL_CONNECTED && agora >= proxWifi) {
      if (proxWifi) Serial.printf(">> Wi-Fi sem conexao (status %d); tentando de novo\n", WiFi.status());
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_SENHA, 6);
      proxWifi = agora + 30000;   // dá 30 s para conectar antes de tentar de novo
    }

    bool redeOk = WiFi.status() == WL_CONNECTED && !wifiDesligadoTeste;
    // NTP só depois do Wi-Fi conectar (e de novo a cada 30 s até acertar o relógio)
    static unsigned long ultNtp = 0;
    if (redeOk && !relogioOk() && (ultNtp == 0 || agora - ultNtp > 30000)) {
      ultNtp = agora;
      configTzTime("<-03>3", "pool.ntp.org", "time.google.com", "time.cloudflare.com");
      Serial.println(">> Wi-Fi conectado; sincronizando o relogio (NTP)...");
    }
    static bool avisouRelogio = false;
    if (!avisouRelogio && relogioOk()) {
      avisouRelogio = true;
      time_t t = time(nullptr); struct tm tm; localtime_r(&t, &tm);
      Serial.printf(">> Wi-Fi ok e relogio sincronizado por NTP: %02d/%02d/%04d %02d:%02d\n",
                    tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
    }

    // TLS confere a validade do certificado pela data: só conecta depois do NTP acertar o relógio
    if (redeOk && !mqtt.connected() && relogioOk() && agora >= proxTentativa) {
      Serial.println(">> conectando ao broker MQTT (TLS 8883)...");
      if (conectarMqtt()) {
        Serial.println(">> MQTT conectado (certificado do broker conferido)");
        espera = 1000;
      } else {
        char erroTls[96] = "";
        tlsMqtt.lastError(erroTls, sizeof(erroTls));
        Serial.printf(">> MQTT falhou (rc=%d, tls: %s); nova tentativa em %lus\n", mqtt.state(), erroTls, espera / 1000);
        proxTentativa = agora + espera;
        espera = min(espera * 2, 60000UL);   // espera crescente: 1, 2, 4... até 60 s
      }
    }
    bool online = redeOk && mqtt.connected();
    if (online != estavaOnline) {
      Serial.printf(">> %s\n", online ? "ONLINE" : "OFFLINE (controle continua; leituras vao para o buffer)");
      estavaOnline = online;
    }
    if (online) mqtt.loop();

    // 1 leitura por minuto vai para o buffer (online ou offline)
    if (agora - ultHistorico >= HISTORICO_MS) { ultHistorico = agora; guardarLeitura(); }

    if (online) {
      if (agora - ultTelemetria >= TELEMETRIA_MS) { ultTelemetria = agora; publicarTelemetria(); }

      Alerta al;
      while (xQueueReceive(filaAlertas, &al, 0) == pdTRUE) {
        JsonDocument d; uint32_t ts = agoraEpoch();
        d["tipo"] = al.tipo; d["msg"] = al.msg; d["ts"] = ts;
        if (!isnan(al.temp)) d["temp_c"] = round(al.temp * 10) / 10.0;
        char m[64], s[65]; snprintf(m, sizeof(m), "%lu|%s", (unsigned long)ts, al.tipo); hmacHex(m, s);
        d["sig"] = s;
        publicar(TOPICO("alerta"), d);
      }
      Ack a;
      while (xQueueReceive(filaAcks, &a, 0) == pdTRUE) {
        JsonDocument d; uint32_t ts = agoraEpoch();
        d["id"] = a.id; d["ok"] = a.ok; d["resultado"] = a.resultado; d["ts"] = ts;
        char m[64], s[65]; snprintf(m, sizeof(m), "%s|%d|%lu", a.id, a.ok ? 1 : 0, (unsigned long)ts); hmacHex(m, s);
        d["sig"] = s;
        publicar(TOPICO("ack"), d);
      }
      // esvazia o buffer aos poucos (depois de uma queda, manda em lotes de 30)
      if (bufQtd > 0 && agora - ultEnvio >= (bufQtd > LOTE_MAX ? 5000UL : HISTORICO_MS)) {
        ultEnvio = agora;
        enviarLote();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ======================================================================
// Setup / Loop
// ======================================================================
void setup() {
  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, LOW);   // estado seguro antes de tudo: aquecedor desligado
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  Serial.begin(115200);

  mtx = xSemaphoreCreateMutex();
  filaComandos = xQueueCreate(8, sizeof(Comando));
  filaAcks     = xQueueCreate(8, sizeof(Ack));
  filaAlertas  = xQueueCreate(8, sizeof(Alerta));

  carregarConfig();
  dht.setup(PIN_DHT, DHTesp::DHT22);
  virador.setPeriodHertz(50);
  virador.attach(PIN_SERVO, 500, 2400);
  virador.write(anguloServo);
  ultimaViragem = inicioDia = millis();
  atualizarRetrato();

  Serial.printf("Ninho iniciado · dia %d · alvo %.1f °C\n", diaCiclo, alvo);
  Serial.println("Comandos: s silenciar · v virar · f falha · d +1 dia · r reiniciar · a37.8 alvo · w sem internet");
  xTaskCreatePinnedToCore(tarefaRede, "rede", 12288, nullptr, 1, nullptr, 0);
}

void loop() {
  unsigned long agora = millis();
  lerSerial(agora);
  tratarComandosRemotos(agora);

  if (agora - ultimaLeitura >= LEITURA_MS) {
    ultimaLeitura = agora;
    lerSensor();
    decidirTemperatura(agora);
    decidirUmidade(agora);
    atualizarRetrato();
    logar(agora);
  }
  decidirViragem(agora);
  if (agora - inicioDia >= DURACAO_DIA_MS) avancarDia(agora);
  agir(agora);
  delay(10);
}
