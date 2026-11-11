# Roteiro da apresentação (4 min) — Ninho

**Quem fala:** Brayan (arquitetura, escolhas, demo) · Mell (problema, testes, produto). Ajustem como preferirem.

## 1. Problema e quem usa (30 s) — Mell
"Pequenos produtores rurais chocam ovos em chocadeiras caseiras. O embrião precisa de 37,5 °C por 21 dias. Uma lâmpada que queima de madrugada ou um superaquecimento de algumas horas perde a ninhada inteira, e o produtor só descobre no dia 21. No interior, a internet é instável."
→ Frase da ideia (está na etapa 0).

## 2. Arquitetura (45 s) — Brayan
Mostrar o diagrama (`docs/img/arquitetura.png`):
- **Dispositivo:** ESP32 + DHT22 + relé (aquecedor) + servo (virador) + buzzer/LED. Decide sozinho.
- **Rede:** MQTT com TLS no broker público test.mosquitto.org e HTTPS para a nuvem.
- **Nuvem:** Cloudflare Worker com D1 (histórico) e KV (último estado).
- **Interface:** painel web em tempo real.

## 3. Escolhas e por quê (60 s) — Brayan
- **Onde decide:** no ESP32. Aquecer, cortar o superaquecimento e virar os ovos não podem esperar a internet.
- **Histerese:** liga abaixo de 37,3 °C e desliga acima de 37,7 °C, para o relé não ficar "batendo".
- **Estado seguro:** sem leitura do sensor, o aquecedor **desliga** (esfriar um pouco é menos grave que cozinhar).
- **Protocolo:** MQTT para tempo real e comandos; HTTPS só para gravar o histórico em lote.
- **KV e D1:** KV para o último estado (painel abre na hora); D1 para perguntas sobre o passado (resumo por hora).
- **Segurança:** TLS com certificado conferido (sem setInsecure), comandos assinados com HMAC, token no Worker, segredos fora do GitHub.

## 4. Demonstração ao vivo (75 s) — Brayan
1. Wokwi rodando: arrastar o DHT22 para **37,0 °C** → a lâmpada do aquecedor acende.
2. Arrastar para **40 °C** → alarme na hora, aquecedor corta, alerta aparece no painel.
3. No painel: enviar **alvo = 37,8** → aparece "enviado" e, em seguida, "executado" (ack).
4. Mostrar o **resumo por hora** (minutos fora da faixa).
> Se a internet da sala falhar: mostrar o vídeo/prints de `docs/prints/` (backup).

## 5. O que falta para virar produto (30 s) — Mell
- **1.000 unidades:** broker privado com usuário/ACL por dispositivo (ex.: EMQX/HiveMQ Cloud), provisionamento de chaves por unidade, atualização OTA.
- **Energia:** nobreak/bateria para o ESP32 e o alarme continuarem numa queda de luz (o aquecedor não roda em bateria, mas o aviso sim).
- **Custo estimado por unidade:** ESP32 (~R$ 35) + DHT22 (~R$ 25) + relé (~R$ 10) + servo (~R$ 15) + buzzer/LED (~R$ 5) ≈ **R$ 90** + caixa e fonte.
- **Hardware real:** sensor melhor (SHT31), relé de estado sólido para a lâmpada, ventoinha para distribuir o calor.

## Perguntas prováveis
- **"E se a internet cair?"** Nada muda no controle. As leituras vão para um buffer de 6 h e são reenviadas quando volta (sem duplicar, por causa do UNIQUE no D1). O painel mostra "offline" pelo LWT.
- **"E se alguém publicar no seu tópico?"** O broker é público, então qualquer um publica — mas o ESP32 só aceita comando com assinatura HMAC válida, com menos de 120 s e id novo, e ainda confere se o alvo está entre 36,5 e 38,5 °C. Testamos com comando falso: recusado.
- **"E com 1.000 unidades?"** Um tópico por chocadeira (`sis1a/ninho/<id>/…`), a mesma tabela D1 com a coluna `dispositivo`, broker privado com ACL e chave por unidade.
- **"Por que DHT22 e não DHT11?"** Precisão de ±0,5 °C contra ±2 °C; a faixa ideal é de só ±0,5 °C.
