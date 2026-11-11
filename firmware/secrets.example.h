// Copie este arquivo para "secrets.h" e preencha.
// O secrets.h está no .gitignore: NUNCA suba senhas ou chaves para o GitHub.
// No Wokwi: crie uma aba "secrets.h" no projeto (e não deixe o projeto público).
#pragma once

// Wi-Fi (no Wokwi a rede é aberta)
#define WIFI_SSID   "Wokwi-GUEST"
#define WIFI_SENHA  ""

// URL do Cloudflare Worker (sem barra no fim)
#define WORKER_URL  "https://ninho.SEU-SUBDOMINIO.workers.dev"

// Token que o ESP32 usa para gravar no histórico (igual ao secret DEVICE_TOKEN do Worker)
#define DEVICE_TOKEN "troque-por-um-token-longo-e-aleatorio"

// Chave compartilhada com o painel para assinar comandos e mensagens (HMAC-SHA256)
// (igual ao secret PAINEL_KEY do Worker e à chave digitada no login do painel)
#define PAINEL_KEY  "troque-por-outra-chave-longa-e-aleatoria"
