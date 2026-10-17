/*
  Leitor RFID: ESP8266 (NodeMCU) + MFRC522 -> HiveMQ Cloud (MQTT com TLS)

  Bibliotecas (Arduino IDE > Gerenciador de Bibliotecas):
    - MFRC522       (GithubCommunity)
    - PubSubClient  (Nick O'Leary)
    - WiFiManager   (tzapu)
  Placa: "NodeMCU 1.0 (ESP-12E Module)"

  Ligacoes MFRC522 -> ESP8266 (NodeMCU)   (alimente em 3V3, nunca em 5V)
    SDA/SS -> D2  (GPIO4)
    SCK    -> D5  (GPIO14)
    MOSI   -> D7  (GPIO13)
    MISO   -> D6  (GPIO12)
    RST    -> D4  (GPIO2)
    GND    -> GND
    3.3V   -> 3V3
    IRQ    -> nao usado

  Buzzer -> D1 (GPIO5):
    - 3 bips agudos curtos : modo de configuracao de WiFi (portal aberto)
    - 2 notas subindo      : WiFi conectado
    - arpejo de 3 notas    : MQTT conectado (primeira vez ou reconexao)
    - 2 notas agudas       : leitura enviada ao MQTT
    - 2 notas graves       : leitura NAO enviada (fica na fila e sai quando reconectar)

  ANTENA DO RFID PULSADA: com a antena do MFRC522 sempre ligada o WiFi do
  ESP8266 degrada (RSSI cai ~8 dB e o DNS/TCP deixam de funcionar). Por isso
  a antena so liga durante a leitura (~35 ms a cada 250 ms). Um cartao e
  detectado em ate ~0,25 s. Para comparar, ANTENA_PULSADA 0 = sempre ligada.

  WiFi: sem SSID/senha no codigo. Se nao conseguir conectar em nenhuma rede
  salva, o ESP cria a rede "ESP8266-RFID": conecte o celular nela e escolha a
  rede do lugar (ou acesse 192.168.4.1).

  Topicos publicados:
    <TOPICO_BASE>/<device_id>/leitura   uma mensagem por passagem de tag
    <TOPICO_BASE>/<device_id>/status    retained; "offline" via Last Will
*/

#include <ESP8266WiFi.h>
#include <WiFiManager.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <time.h>

// ================= CONFIGURACAO =================
const char*    MQTT_HOST    = "c115e6f71aae4b3eae2d554b93ea0250.s1.eu.hivemq.cloud";
const uint16_t MQTT_PORTA   = 8883;
const char*    MQTT_USUARIO = "Alisson";
const char*    MQTT_SENHA   = "Alisson_111";

const char* TOPICO_BASE = "cafe/teste/leitor";
const char* FW_VERSAO   = "0.4.0";

#define PINO_SS      D2   // GPIO4
#define PINO_RST     D4   // GPIO2
#define PINO_BUZZER  D1   // GPIO5

#define ANTENA_PULSADA 1                 // 1 = antena so em pulsos | 0 = sempre ligada
const unsigned long CICLO_MS = 250;      // um pulso de leitura a cada 250 ms
const unsigned long ANTES_MS = 10;       // espera apos ligar a antena (cartao "acorda")

// Mesma tag dentro desta janela = mesma passagem, nao publica de novo
const unsigned long JANELA_REPETICAO_MS = 2000;
// ================================================

BearSSL::WiFiClientSecure wifiCliente;
PubSubClient              mqtt(wifiCliente);
MFRC522                   rfid(PINO_SS, PINO_RST);

String deviceId;
String topicoLeitura;
String topicoStatus;

uint32_t      seq = 0;
String        ultimaUid;
unsigned long ultimaUidMs         = 0;
unsigned long ultimaTentativaMqtt = 0;
unsigned long ultimoCiclo         = 0;
bool          estavaConectado     = false;

// ---- Fila em RAM: segura leituras enquanto o broker esta fora ----
const int FILA_MAX = 16;
String fila[FILA_MAX];
int filaInicio = 0;
int filaQtd = 0;

void enfileira(const String& payload) {
  if (filaQtd == FILA_MAX) {  // cheia: descarta a mais antiga
    filaInicio = (filaInicio + 1) % FILA_MAX;
    filaQtd--;
    Serial.println("[fila] cheia, leitura mais antiga descartada");
  }
  fila[(filaInicio + filaQtd) % FILA_MAX] = payload;
  filaQtd++;
}

void drenaFila() {
  while (filaQtd > 0 && mqtt.connected()) {
    if (!mqtt.publish(topicoLeitura.c_str(), fila[filaInicio].c_str())) {
      Serial.println("[mqtt] falha ao publicar, tenta de novo depois");
      return;
    }
    Serial.printf("[mqtt] publicado da fila: %s\n", fila[filaInicio].c_str());
    fila[filaInicio] = "";
    filaInicio = (filaInicio + 1) % FILA_MAX;
    filaQtd--;
  }
}

// ---- Sons ----
void bipSucesso() {
  tone(PINO_BUZZER, 2600, 100);
  delay(120);
  tone(PINO_BUZZER, 3000, 150);
}

void bipErro() {
  tone(PINO_BUZZER, 220, 200);
  delay(250);
  tone(PINO_BUZZER, 147, 300);
  delay(300);
  noTone(PINO_BUZZER);
}

void bipModoConfig() {
  for (int i = 0; i < 3; i++) {
    tone(PINO_BUZZER, 1500, 100);
    delay(150);
  }
  noTone(PINO_BUZZER);
}

void bipWifiConectado() {
  tone(PINO_BUZZER, 1800, 80);
  delay(120);
  tone(PINO_BUZZER, 2200, 120);
  delay(150);
  noTone(PINO_BUZZER);
}

void bipMqttConectado() {
  tone(PINO_BUZZER, 1200, 80);
  delay(100);
  tone(PINO_BUZZER, 1800, 80);
  delay(100);
  tone(PINO_BUZZER, 2600, 120);
  delay(140);
  noTone(PINO_BUZZER);
}

// ---- RFID ----
String uidHex(const MFRC522::Uid& uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

// Liga a antena so pelo tempo da leitura. Retorna true se leu um cartao.
bool lerCartao(String& uid, String& tipo) {
#if ANTENA_PULSADA
  rfid.PCD_AntennaOn();
  delay(ANTES_MS);
#endif
  bool achou = rfid.PICC_IsNewCardPresent() && rfid.PICC_ReadCardSerial();
  if (achou) {
    uid  = uidHex(rfid.uid);
    tipo = rfid.PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak));
    rfid.PICC_HaltA();
    rfid.PCD_StopCrypto1();
  }
#if ANTENA_PULSADA
  rfid.PCD_AntennaOff();
#endif
  return achou;
}

// ---- WiFi pelo celular (WiFiManager) ----
void aoEntrarModoConfig(WiFiManager* wifiManager) {
  Serial.println("[wifi] modo de configuracao: conecte o celular na rede ESP8266-RFID");
  bipModoConfig();
}

// WiFiManager e local: a RAM dele e liberada assim que a funcao termina
void conectaWifiInicial() {
  WiFiManager wm;
  wm.setAPCallback(aoEntrarModoConfig);
  wm.setConfigPortalTimeout(180);   // 3 min sem ninguem configurar: reinicia

  if (!wm.autoConnect("ESP8266-RFID")) {
    Serial.println("[wifi] nao conectou e o portal expirou, reiniciando...");
    ESP.restart();
  }
}

bool relogioSincronizado() {
  return time(nullptr) > 1700000000;
}

// ---- MQTT ----
void conectaMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - ultimaTentativaMqtt < 3000) return;  // nao martela o broker
  ultimaTentativaMqtt = millis();

  uint32_t heap  = ESP.getFreeHeap();
  uint32_t bloco = ESP.getMaxFreeBlockSize();

  unsigned long t = millis();
  // Last Will: se o ESP cair, o proprio broker publica "offline"
  bool ok = mqtt.connect(deviceId.c_str(), MQTT_USUARIO, MQTT_SENHA,
                         topicoStatus.c_str(), 0, true,
                         "{\"estado\":\"offline\"}");
  unsigned long dt = millis() - t;

  if (!ok) {
    char erro[128];
    int cod = wifiCliente.getLastSSLError(erro, sizeof(erro));
    Serial.printf("[mqtt] FALHOU em %lu ms | rc=%d | ssl=%d (%s) | RSSI %d | heap %u, bloco %u\n",
                  dt, mqtt.state(), cod, erro, WiFi.RSSI(), heap, bloco);
    wifiCliente.stop();   // libera o contexto TLS parcial
    return;
  }

  char status[128];
  snprintf(status, sizeof(status),
           "{\"estado\":\"online\",\"ip\":\"%s\",\"fw\":\"%s\"}",
           WiFi.localIP().toString().c_str(), FW_VERSAO);
  mqtt.publish(topicoStatus.c_str(), status, true);  // retained

  Serial.printf("[mqtt] conectado em %lu ms | RSSI %d\n", dt, WiFi.RSSI());
  bipMqttConectado();
}

void setup() {
  Serial.begin(115200);
  delay(300);
  pinMode(PINO_BUZZER, OUTPUT);

  conectaWifiInicial();
  Serial.printf("\n[wifi] conectado | IP %s | RSSI %d dBm | antena RFID %s\n",
                WiFi.localIP().toString().c_str(), WiFi.RSSI(),
                ANTENA_PULSADA ? "PULSADA" : "SEMPRE LIGADA");
  bipWifiConectado();

  String mac = WiFi.macAddress();   // "24:6F:28:A1:B2:C3"
  mac.replace(":", "");
  mac.toLowerCase();
  deviceId      = "esp8266-" + mac.substring(6);
  topicoLeitura = String(TOPICO_BASE) + "/" + deviceId + "/leitura";
  topicoStatus  = String(TOPICO_BASE) + "/" + deviceId + "/status";
  Serial.printf("device_id: %s\n", deviceId.c_str());

  configTime(0, 0, "a.st1.ntp.br", "pool.ntp.org");  // UTC

  wifiCliente.setInsecure();   // TLS cifrado, sem validar o certificado (bancada)
  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setBufferSize(512);

  SPI.begin();
  rfid.PCD_Init();
#if ANTENA_PULSADA
  rfid.PCD_AntennaOff();
#endif
  byte versao = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (versao == 0x00 || versao == 0xFF) {
    Serial.println("[rfid] MFRC522 nao respondeu. Confira fiacao e alimentacao 3V3.");
  } else {
    Serial.printf("[rfid] MFRC522 ok, versao 0x%02X\n", versao);
  }

  Serial.printf("[diag] pos-setup: heap %u, maior bloco %u\n",
                ESP.getFreeHeap(), ESP.getMaxFreeBlockSize());
}

void loop() {
  conectaMqtt();
  mqtt.loop();
  drenaFila();

  bool conectado = mqtt.connected();
  if (estavaConectado && !conectado) {
    Serial.printf("[mqtt] conexao CAIU | rc=%d | WiFi %s | RSSI %d\n",
                  mqtt.state(),
                  WiFi.status() == WL_CONNECTED ? "ok" : "caiu",
                  WiFi.RSSI());
  }
  estavaConectado = conectado;

  unsigned long agora = millis();
#if ANTENA_PULSADA
  if (agora - ultimoCiclo < CICLO_MS) return;
  ultimoCiclo = agora;
#endif

  String uid, tipo;
  if (!lerCartao(uid, tipo)) return;

  agora = millis();
  if (uid == ultimaUid && agora - ultimaUidMs < JANELA_REPETICAO_MS) {
    ultimaUidMs = agora;   // tag ainda no campo: mesma passagem
    return;
  }
  ultimaUid   = uid;
  ultimaUidMs = agora;

  time_t ts = relogioSincronizado() ? time(nullptr) : 0;

  char payload[256];
  snprintf(payload, sizeof(payload),
           "{\"device_id\":\"%s\",\"uid\":\"%s\",\"tipo\":\"%s\","
           "\"ts\":%ld,\"seq\":%lu,\"rssi_wifi\":%d}",
           deviceId.c_str(), uid.c_str(), tipo.c_str(),
           (long)ts, (unsigned long)++seq, WiFi.RSSI());

  Serial.println("========================");
  Serial.print("Cartao detectado! UID: ");
  Serial.println(uid);
  Serial.print("Tipo: ");
  Serial.println(tipo);

  if (mqtt.connected() && mqtt.publish(topicoLeitura.c_str(), payload)) {
    Serial.println("[mqtt] enviado na hora");
    bipSucesso();
  } else {
    Serial.println("[mqtt] nao foi possivel enviar agora, guardado na fila");
    enfileira(payload);
    bipErro();
  }
  Serial.println("========================");
}
