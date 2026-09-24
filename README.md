# Smart Coffee — kit de teste de rastreabilidade

Protótipo do caminho **tag RFID → ESP32 → broker MQTT → tela de rastreabilidade**,
mais o núcleo de banco que sustenta a rastreabilidade de verdade.

```
tag → MFRC522 → ESP32 → Wi-Fi → HiveMQ Cloud → WebSocket → painel.html
                                     ↑
                        simulador_esp32.js (quando não há hardware)
```

## O que tem aqui

| Pasta | Arquivo | Para quê |
|---|---|---|
| `firmware/` | `leitor_rfid_mqtt.ino` | ESP32 + MFRC522: lê a tag e publica no broker |
| `painel/` | `painel.html` | Tela de rastreabilidade que consome as leituras |
| `simulador/` | `simulador_esp32.js` | Finge ser o ESP32, para testar sem hardware |
| `broker/` | `mosquitto.conf` | Broker local, alternativa à HiveMQ Cloud |
| `banco/` | `schema.sql` | Núcleo append-only de rastreabilidade (PostgreSQL) |
| `docs/` | `fluxo-rastreabilidade-cafe.mermaid` | Arquitetura completa proposta |

O firmware, o painel e o simulador formam o teste de ponta a ponta.
O `schema.sql` é a base do sistema real e não é usado por eles.

## Antes de tudo: as credenciais

A HiveMQ Cloud não aceita conexão anônima. Preencha em dois arquivos:

**`painel/painel.html`**, no bloco `CONFIG` no início do `<script>`:
```js
usuario: "Alisson",
senha: "SUA_SENHA",
```

**`firmware/leitor_rfid_mqtt/leitor_rfid_mqtt.ino`**, dentro do `#if HIVEMQ_CLOUD`:
```cpp
const char*    MQTT_USUARIO = "Alisson";
const char*    MQTT_SENHA   = "SUA_SENHA";
```

O simulador lê de variável de ambiente, não precisa editar.

A credencial é a de **Access Management > Credentials** no painel do cluster,
não a do login do site, e diferencia maiúsculas.

## Portas

| Quem conecta | Porta | Endereço |
|---|---|---|
| ESP32 e simulador | 8883 | `mqtts://SEU-CLUSTER.s1.eu.hivemq.cloud:8883` |
| Navegador | 8884 | `wss://SEU-CLUSTER.s1.eu.hivemq.cloud:8884/mqtt` |

Navegador não fala MQTT por TCP. A porta 8883 não funciona no painel, e a 8884
não funciona no ESP32.

## Ordem de montagem

### 1. Painel
Abra `painel/painel.html` direto do disco, com dois cliques. Ele conecta sozinho.
A pílula no topo deve mostrar "Leitor conectado".

Clique em **+ Nova Amostra**: uma linha deve aparecer. Isso já prova cluster,
credencial e caminho de ida e volta pelo broker.

### 2. Simulador
```bash
cd simulador
npm install mqtt
MQTT_USUARIO=Alisson MQTT_SENHA=... node simulador_esp32.js
```
No PowerShell:
```powershell
$env:MQTT_USUARIO="Alisson"; $env:MQTT_SENHA="..."; node simulador_esp32.js
```

Com o painel aberto ao lado:

| Tecla | O que faz |
|---|---|
| 1 a 5 | passa uma das cinco tags no leitor |
| a | liga ou desliga o envio automático |
| r | simula reinício do ESP32 (o `seq` volta para 1) |
| p | simula perda de leituras (abre lacuna no `seq`) |
| q | sai publicando offline |

Vale testar `r` e `p` agora: são os casos que dão trabalho depois, e a tela
precisa mostrá-los como "reiniciou" e "N perdidas".

Dois leitores ao mesmo tempo: rode duas vezes com `--device esp32-a` e `--device esp32-b`.

### 3. ESP32 com MFRC522

Ligações (alimente o módulo em **3V3**, nunca em 5 V):

| MFRC522 | ESP32 |
|---|---|
| SDA / SS | GPIO 5 |
| SCK | GPIO 18 |
| MOSI | GPIO 23 |
| MISO | GPIO 19 |
| RST | GPIO 22 |
| GND | GND |
| 3.3V | 3V3 |

Na Arduino IDE, instale as bibliotecas **MFRC522** (GithubCommunity) e
**PubSubClient** (Nick O'Leary). Placa: ESP32 Dev Module.

Preencha `WIFI_SSID`, `WIFI_SENHA` e a credencial, e grave. O monitor serial
a 115200 mostra o `device_id` e se o MFRC522 respondeu.

## Broker local, se preferir

Em vez da HiveMQ Cloud, dentro de `broker/`:
```bash
docker run -it --rm -p 1883:1883 -p 9001:9001 \
  -v "$PWD/mosquitto.conf:/mosquitto/config/mosquitto.conf" eclipse-mosquitto:2
```
Depois troque nos arquivos:
- painel: `broker: "ws://localhost:9001"`
- firmware: `#define HIVEMQ_CLOUD 0` e `MQTT_HOST` com o **IP do PC na rede**
- simulador: `broker: "mqtt://IP_DO_PC:1883"`

O ESP32 nunca deve apontar para `localhost`: para ele, localhost é ele mesmo.

## Tópicos e payload

```
cafe/teste/leitor/<device_id>/leitura    uma mensagem por passagem de tag
cafe/teste/leitor/<device_id>/status     retained, com Last Will
```

```json
{
  "device_id": "esp32-a1b2c3",
  "uid": "04A1B2C3",
  "tipo": "MIFARE 1KB",
  "ts": 1758550000,
  "seq": 42,
  "rssi_wifi": -58
}
```

`ts` é o relógio do próprio leitor, via NTP; vem `0` enquanto não sincroniza.
`seq` é monotônico e reinicia junto com o ESP32 — é ele que revela lacuna e reboot.

## Quando algo não conecta

| Sintoma | Causa provável |
|---|---|
| Painel: "Broker sem resposta" | URL sem `/mqtt` no fim, ou porta 8883 no lugar da 8884 |
| Painel: "Not authorized" | usuário ou senha; é a credencial de Access Management |
| Serial: `rc=-2` | host errado, firewall, ou CA errada com `VALIDA_CERTIFICADO 1` |
| Serial: `rc=4` ou `rc=5` | usuário ou senha recusados |
| Serial: `MFRC522 nao respondeu` | fiação, ou módulo alimentado em 5 V |
| Conecta e nada aparece na tela | `TOPICO_BASE` diferente entre firmware e painel |

## Limites deste protótipo

O painel fala MQTT direto do navegador, o que serve para bancada e não para
produção. No sistema real quem consome o broker é o backend, que valida, aplica
a máquina de estado e grava; a tela recebe do próprio servidor. Credencial de
broker não pode ficar em arquivo servido ao cliente.

Local, processo, variedade, colheita e secagem são **simulados** a partir de um
hash do UID da tag, só para a tela ter conteúdo. No sistema real a tag é
consultada no banco (`unidade_fisica → lote`) e devolve os dados verdadeiros.

Troque a credencial do cluster quando terminar os testes.
