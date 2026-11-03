# Ninho — chocadeira inteligente

Trabalho N2 de IoT (AMF · Sistemas de Informação · Prof. Leonam V. Hemann · 2026/02)
**Dupla:** Brayan Martins · Mell Campos

> Para pequenos produtores rurais, que perdem ninhadas inteiras quando a chocadeira esquenta ou esfria sem ninguém ver,
> o **Ninho** sente a temperatura e a umidade (DHT22), decide quando aquecer, quando virar os ovos e quando alarmar,
> e age com o aquecedor (relé), o virador (servo) e a sirene (buzzer + LED), **mesmo sem internet**.

## O que o sistema faz

- Mantém a chocadeira em **37,5 °C** ligando e desligando o aquecedor com histerese (liga < 37,3 °C, desliga > 37,7 °C).
- **Corta o aquecedor na hora** se passar de 39,5 °C e dispara o alarme.
- Alarma (buzzer + LED) se a temperatura ficar fora de 36,5–38,5 °C por mais de 2 min.
- Vira os ovos a cada 1 h com o servo e para no dia 19 do ciclo de 21 dias.
- Avisa quando a umidade sai da faixa do dia (50–55 % até o dia 18, 65–70 % nos dias 19–21).
- Falha do sensor → **estado seguro**: aquecedor desligado, LED piscando e bipe.
- Dia do ciclo e temperatura alvo ficam salvos na flash (NVS) e sobrevivem à queda de energia.

## Estrutura

```
firmware/   código do ESP32 + circuito do Wokwi
docs/       caderno de projeto (entrega)
worker/     Cloudflare Worker + D1/KV (em breve)
painel/     painel web (em breve)
```

## Como montar (Wokwi)

1. Crie um projeto ESP32 em [wokwi.com](https://wokwi.com).
2. Copie `firmware/sketch.ino`, `firmware/diagram.json` e `firmware/libraries.txt` para as abas de mesmo nome.
3. Rode a simulação e abra o Serial Monitor.

| Componente | GPIO | Representa |
|---|---|---|
| DHT22 | 15 | sensor de temperatura e umidade |
| Relé + LED laranja | 26 | aquecedor (lâmpada) |
| Servo | 13 | virador da bandeja de ovos |
| Buzzer | 23 | sirene |
| LED vermelho | 4 | luz de alarme |

Com `#define DEMO 1` os tempos ficam curtos para a apresentação: alarme em 20 s, viragem a cada 30 s e 1 dia = 3 min.

## Como testar

Clique no DHT22 durante a simulação para mudar a temperatura. Pelo Serial Monitor:

| Comando | Efeito |
|---|---|
| `s` | silencia o buzzer por 10 min (o LED continua) |
| `v` | vira os ovos agora |
| `f` | simula falha do DHT22 (testa o estado seguro) |
| `d` | avança 1 dia no ciclo |
| `r` | reinicia o ciclo (dia 1) |
| `a37.8` | muda a temperatura alvo (só aceita 36,5–38,5) |

| Teste | O que fazer | Esperado |
|---|---|---|
| Aquecedor | DHT22 em 37,0 → 37,5 → 37,8 °C | lâmpada acende → mantém → apaga |
| Superaquecimento | DHT22 em 40 °C | alarme e aquecedor desligado na hora |
| Alarme com tempo | DHT22 em 39,0 °C | alarme depois de 20 s (DEMO) |
| Falha do sensor | comando `f` | aquecedor OFF, LED piscando, bipe a cada 5 s |
| Comando inválido | `a42` | `REJEITADO` |
| Reinício | parar e rodar de novo | mantém dia e alvo |
