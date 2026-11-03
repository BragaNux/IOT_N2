/*
  Ninho — chocadeira inteligente (IoT N2 · AMF · Prof. Leonam V. Hemann)
  Brayan Martins · Mell Campos

  Etapas 2 e 3: sentir, decidir e agir na borda, com estado seguro.
  (MQTT/TLS, Worker e painel entram nas próximas etapas.)

  Pinos: DHT22=15 · Relé (aquecedor)=26 · Servo (virador)=13 · Buzzer=23 · LED alarme=4

  Comandos pelo Serial Monitor (para testar sem nuvem):
    s        silenciar o buzzer por 10 min (simula o comando remoto)
    v        virar os ovos agora
    f        liga/desliga falha simulada do DHT22
    d        avança 1 dia no ciclo
    r        reinicia o ciclo (dia 1)
    a37.8    muda a temperatura alvo (só aceita 36.5–38.5)
*/
#include <DHTesp.h>
#include <ESP32Servo.h>
#include <Preferences.h>

// ---------------- Pinos ----------------
const int PIN_DHT    = 15;
const int PIN_RELE   = 26;
const int PIN_SERVO  = 13;
const int PIN_BUZZER = 23;
const int PIN_LED    = 4;

// ---------------- Modo demonstração ----------------
// DEMO = 1 encurta os tempos para caber na apresentação.
#define DEMO 1

#if DEMO
const unsigned long TEMPO_ALARME_MS   = 20UL * 1000;        // real: 2 min
const unsigned long TEMPO_UMIDADE_MS  = 30UL * 1000;        // real: 10 min
const unsigned long INTERVALO_VIRAR_MS = 30UL * 1000;       // real: 1 h
const unsigned long DURACAO_DIA_MS    = 3UL * 60 * 1000;    // real: 24 h
#else
const unsigned long TEMPO_ALARME_MS   = 2UL * 60 * 1000;
const unsigned long TEMPO_UMIDADE_MS  = 10UL * 60 * 1000;
const unsigned long INTERVALO_VIRAR_MS = 60UL * 60 * 1000;
const unsigned long DURACAO_DIA_MS    = 24UL * 60 * 60 * 1000;
#endif

// ---------------- Limites ----------------
const float ALVO_MIN = 36.5, ALVO_MAX = 38.5;  // faixa aceita para a temperatura alvo
const float HIST_AQUEC = 0.2;                  // liga < alvo-0,2 · desliga > alvo+0,2
const float FAIXA_ALARME = 1.0;                // alarme fora de alvo ± 1,0
const float FAIXA_RESET  = 0.5;                // alarme desliga dentro de alvo ± 0,5
const float T_CORTE = 39.5;                    // superaquecimento: corta na hora
const int   DIA_PARAR_VIRAR = 19;
const int   DIAS_CICLO = 21;

const unsigned long LEITURA_MS  = 2000;
const unsigned long SILENCIO_MS = 10UL * 60 * 1000;
const int FALHAS_MAX = 3;

// ---------------- Estado ----------------
DHTesp dht;
Servo virador;
Preferences nvs;

float alvo = 37.5;            // salvo na flash
int diaCiclo = 1;             // salvo na flash

float tempC = NAN, umid = NAN;
int falhasSeguidas = 0;
bool falhaSensor = false;
bool falhaSimulada = false;

bool aquecedor = false;
bool alarmeTemp = false;
bool superaquecido = false;
unsigned long foraDesde = 0;

bool avisoUmidade = false;
unsigned long umidForaDesde = 0;

int anguloServo = 45;
unsigned long ultimaViragem = 0;
unsigned long inicioDia = 0;

unsigned long silenciadoAte = 0;
unsigned long ultimaLeitura = 0;

// ---------------- Flash (NVS): sobrevive à queda de energia ----------------
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

// ---------------- Decidir ----------------
void decidirTemperatura(unsigned long agora) {
  if (isnan(tempC)) {
    if (++falhasSeguidas >= FALHAS_MAX) falhaSensor = true;
    if (falhaSensor) aquecedor = false;  // estado seguro: sem leitura, não aquece
    return;
  }
  falhasSeguidas = 0;
  falhaSensor = false;

  // Aquecedor com histerese + corte de superaquecimento
  superaquecido = tempC >= T_CORTE;
  if (superaquecido)                       aquecedor = false;
  else if (tempC < alvo - HIST_AQUEC)      aquecedor = true;
  else if (tempC > alvo + HIST_AQUEC)      aquecedor = false;
  // entre alvo-0,2 e alvo+0,2: mantém

  // Alarme com tempo mínimo + histerese
  bool fora = tempC < alvo - FAIXA_ALARME || tempC > alvo + FAIXA_ALARME;
  if (superaquecido) {
    alarmeTemp = true;                     // imediato
  } else if (fora) {
    if (foraDesde == 0) foraDesde = agora;
    if (agora - foraDesde >= TEMPO_ALARME_MS) alarmeTemp = true;
  } else if (fabs(tempC - alvo) <= FAIXA_RESET) {
    foraDesde = 0;
    alarmeTemp = false;
  }
}

void decidirUmidade(unsigned long agora) {
  if (isnan(umid)) return;
  // dias 1–18: 50–55 % (tolera 45–60) · dias 19–21: 65–70 % (tolera 60–75)
  float uMin = diaCiclo < DIA_PARAR_VIRAR ? 45 : 60;
  float uMax = diaCiclo < DIA_PARAR_VIRAR ? 60 : 75;
  if (umid < uMin || umid > uMax) {
    if (umidForaDesde == 0) umidForaDesde = agora;
    if (agora - umidForaDesde >= TEMPO_UMIDADE_MS) avisoUmidade = true;
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
  if (diaCiclo >= DIA_PARAR_VIRAR) return;  // últimos dias: ovos parados
  if (agora - ultimaViragem >= INTERVALO_VIRAR_MS) virarOvos(agora, "automática");
}

void avancarDia(unsigned long agora) {
  inicioDia = agora;
  if (diaCiclo < DIAS_CICLO) diaCiclo++;
  salvarConfig();
  Serial.printf(">> dia %d do ciclo%s\n", diaCiclo,
                diaCiclo >= DIA_PARAR_VIRAR ? " (viragem parada)" : "");
}

// ---------------- Agir ----------------
bool buzzerTocando = false;

void buzzer(bool ligar) {
  if (ligar == buzzerTocando) return;
  buzzerTocando = ligar;
  if (ligar) tone(PIN_BUZZER, 2000);
  else noTone(PIN_BUZZER);
}

void agir(unsigned long agora) {
  digitalWrite(PIN_RELE, aquecedor ? HIGH : LOW);

  if (falhaSensor && !alarmeTemp) digitalWrite(PIN_LED, (agora / 200) % 2);
  else                            digitalWrite(PIN_LED, alarmeTemp);

  bool som = false;
  if (agora >= silenciadoAte) {
    if (alarmeTemp)       som = true;                      // contínuo
    else if (falhaSensor) som = (agora % 5000) < 150;      // bipe a cada 5 s
  }
  buzzer(som);
}

// ---------------- Log e comandos ----------------
const char* nomeEstado() {
  if (falhaSensor)   return "FALHA_SENSOR";
  if (superaquecido) return "SUPERAQUECIDO";
  if (alarmeTemp)    return "ALARME_TEMP";
  if (foraDesde)     return "FORA_FAIXA_CONTANDO";
  return "NORMAL";
}

void logar(unsigned long agora) {
  Serial.printf("[%6lus] dia=%d alvo=%.1f temp_c=%s umid_pct=%s aquecedor=%s estado=%s%s%s\n",
    agora / 1000, diaCiclo, alvo,
    isnan(tempC) ? "NaN" : String(tempC, 1).c_str(),
    isnan(umid)  ? "NaN" : String(umid, 0).c_str(),
    aquecedor ? "ON" : "OFF",
    nomeEstado(),
    avisoUmidade ? " AVISO_UMIDADE" : "",
    agora < silenciadoAte ? " (silenciado)" : "");
}

String linhaSerial;

void tratarComando(String cmd, unsigned long agora) {
  cmd.trim();
  if (cmd.length() == 0) return;
  char c = cmd.charAt(0);
  if (c == 's') {
    silenciadoAte = agora + SILENCIO_MS;
    Serial.println(">> buzzer silenciado por 10 min (LED continua)");
  } else if (c == 'v') {
    virarOvos(agora, "manual");
  } else if (c == 'f') {
    falhaSimulada = !falhaSimulada;
    Serial.printf(">> falha simulada do DHT22: %s\n", falhaSimulada ? "ON" : "OFF");
  } else if (c == 'd') {
    avancarDia(agora);
  } else if (c == 'r') {
    diaCiclo = 0;
    avancarDia(agora);
  } else if (c == 'a') {
    float novo = cmd.substring(1).toFloat();
    if (novo >= ALVO_MIN && novo <= ALVO_MAX) {
      alvo = novo;
      salvarConfig();
      Serial.printf(">> alvo = %.1f °C (salvo)\n", alvo);
    } else {
      Serial.printf(">> REJEITADO: alvo %.1f fora de %.1f–%.1f\n", novo, ALVO_MIN, ALVO_MAX);
    }
  }
}

void lerSerial(unsigned long agora) {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      tratarComando(linhaSerial, agora);
      linhaSerial = "";
    } else {
      linhaSerial += c;
    }
  }
}

// ---------------- Setup / Loop ----------------
void setup() {
  // estado seguro antes de qualquer coisa: aquecedor desligado
  pinMode(PIN_RELE, OUTPUT);
  digitalWrite(PIN_RELE, LOW);
  pinMode(PIN_LED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);

  Serial.begin(115200);
  carregarConfig();
  dht.setup(PIN_DHT, DHTesp::DHT22);

  virador.setPeriodHertz(50);
  virador.attach(PIN_SERVO, 500, 2400);
  virador.write(anguloServo);

  unsigned long agora = millis();
  ultimaViragem = agora;
  inicioDia = agora;

  Serial.printf("Ninho iniciado · dia %d · alvo %.1f °C\n", diaCiclo, alvo);
  Serial.println("Comandos: s silenciar · v virar · f falha · d +1 dia · r reiniciar ciclo · a37.8 alvo");
}

void loop() {
  unsigned long agora = millis();
  lerSerial(agora);

  if (agora - ultimaLeitura >= LEITURA_MS) {
    ultimaLeitura = agora;
    lerSensor();
    decidirTemperatura(agora);
    decidirUmidade(agora);
    logar(agora);
  }

  decidirViragem(agora);
  if (agora - inicioDia >= DURACAO_DIA_MS) avancarDia(agora);

  agir(agora);
}
