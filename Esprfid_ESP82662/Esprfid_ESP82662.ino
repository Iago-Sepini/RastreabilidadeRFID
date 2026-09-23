/*
  Leitor RFID de teste: ESP8266 (NodeMCU) + MFRC522 -> broker MQTT

  Bibliotecas (Arduino IDE > Gerenciador de Bibliotecas):
    - MFRC522       (GithubCommunity)
    - PubSubClient  (Nick O'Leary)
  Placa: "NodeMCU 1.0 (ESP-12E Module)"

  Ligacoes MFRC522 -> ESP8266 (NodeMCU)   (alimente em 3V3, nunca em 5V)
    SDA/SS -> D8  (GPIO15)
    SCK    -> D5  (GPIO14)
    MOSI   -> D7  (GPIO13)
    MISO   -> D6  (GPIO12)
    RST    -> D3  (GPIO0)
    GND    -> GND
    3.3V   -> 3V3
    IRQ    -> nao usado

  Topicos publicados:
    <TOPICO_BASE>/<device_id>/leitura   uma mensagem por passagem de tag
    <TOPICO_BASE>/<device_id>/status    retained; "offline" via Last Will
*/

#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <MFRC522.h>
#include <time.h>

// ================= CONFIGURACAO =================
const char* WIFI_SSID  = "Iago";
const char* WIFI_SENHA = "jurema10";

// --- Qual broker? -------------------------------------------------
//   1 = HiveMQ Cloud   (TLS na 8883 + usuario e senha)
//   0 = broker.hivemq.com publico, ou Mosquitto na sua rede (1883, sem login)
#define HIVEMQ_CLOUD 1

#if HIVEMQ_CLOUD
  #include <WiFiClientSecure.h>
  // Endereco do cluster, no painel da HiveMQ Cloud (sem "https://")
  const char*    MQTT_HOST    = "c115e6f71aae4b3eae2d554b93ea0250.s1.eu.hivemq.cloud";
  const uint16_t MQTT_PORTA   = 8883;
  // Credencial criada em Access Management > Credentials. PREENCHA.
  const char*    MQTT_USUARIO = "Alisson";
  const char*    MQTT_SENHA   = "Alisson_111";

  // 1 = valida o certificado do servidor (precisa colar a raiz abaixo)
  // 0 = so cifra, sem verificar com quem esta falando. Aceitavel em bancada.
  #define VALIDA_CERTIFICADO 0

  // Raiz que assina o certificado da HiveMQ Cloud: ISRG Root X1 (Let's Encrypt).
  // Baixe em https://letsencrypt.org/certs/isrgrootx1.pem e cole o conteudo aqui.
  const char* CA_RAIZ = R"EOF(
-----BEGIN CERTIFICATE-----
COLE_AQUI_O_CONTEUDO_DO_isrgrootx1.pem
-----END CERTIFICATE-----
)EOF";

  BearSSL::WiFiClientSecure wifiCliente;
#else
  // Broker publico de teste da HiveMQ, ou o IP do PC rodando Mosquitto.
  // NAO use "localhost": para o ESP8266, localhost e ele mesmo.
  const char*    MQTT_HOST    = "broker.hivemq.com";
  const uint16_t MQTT_PORTA   = 1883;
  const char*    MQTT_USUARIO = "Alisson";   // vazio = anonimo
  const char*    MQTT_SENHA   = "Alisson_111";

  WiFiClient wifiCliente;
#endif

// No broker publico QUALQUER UM le este topico. Troque o sufixo por algo seu.
const char* TOPICO_BASE = "cafe/teste/leitor";
const char* FW_VERSAO   = "0.1.0";

#define PINO_SS   D8   // GPIO15
#define PINO_RST  D3   // GPIO0
#define PINO_LED  D0   // GPIO16 - LED indicador (pode trocar por outro pino livre)

// Mesma tag dentro desta janela = mesma passagem, nao publica de novo
const unsigned long JANELA_REPETICAO_MS = 2000;
// ================================================

MFRC522      rfid(PINO_SS, PINO_RST);
PubSubClient mqtt(wifiCliente);

String deviceId;
String topicoLeitura;
String topicoStatus;

uint32_t      seq = 0;
String        ultimaUid;
unsigned long ultimaUidMs = 0;
unsigned long ultimaTentativaMqtt = 0;

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
    Serial.printf("[mqtt] publicado: %s\n", fila[filaInicio].c_str());
    fila[filaInicio] = "";
    filaInicio = (filaInicio + 1) % FILA_MAX;
    filaQtd--;
  }
}

String uidHex(const MFRC522::Uid& uid) {
  String s;
  for (byte i = 0; i < uid.size; i++) {
    if (uid.uidByte[i] < 0x10) s += "0";
    s += String(uid.uidByte[i], HEX);
  }
  s.toUpperCase();
  return s;
}

void piscaLed() {
  digitalWrite(PINO_LED, HIGH);
  delay(60);
  digitalWrite(PINO_LED, LOW);
}

bool relogioSincronizado() {
  return time(nullptr) > 1700000000;
}

void conectaMqtt() {
  if (mqtt.connected() || WiFi.status() != WL_CONNECTED) return;
  if (millis() - ultimaTentativaMqtt < 3000) return;  // nao martela o broker
  ultimaTentativaMqtt = millis();

#if HIVEMQ_CLOUD && VALIDA_CERTIFICADO
  // Sem hora certa o ESP8266 julga o certificado vencido e o TLS falha.
  if (!relogioSincronizado()) {
    Serial.println("[ntp] esperando o relogio sincronizar antes do TLS...");
    return;
  }
#endif

  Serial.printf("[mqtt] conectando em %s:%u ...\n", MQTT_HOST, MQTT_PORTA);
  const char* usuario = strlen(MQTT_USUARIO) ? MQTT_USUARIO : nullptr;
  const char* senha   = strlen(MQTT_SENHA)   ? MQTT_SENHA   : nullptr;

  // Last Will: se o ESP cair, o proprio broker publica "offline"
  bool ok = mqtt.connect(deviceId.c_str(), usuario, senha,
                         topicoStatus.c_str(), 0, true,
                         "{\"estado\":\"offline\"}");
  if (!ok) {
    // -2 = nao alcancou o broker (host, porta, firewall, ou CA errada no TLS)
    //  4 = usuario ou senha recusados      5 = nao autorizado
    Serial.printf("[mqtt] falhou, rc=%d\n", mqtt.state());
    return;
  }

  char status[128];
  snprintf(status, sizeof(status),
           "{\"estado\":\"online\",\"ip\":\"%s\",\"fw\":\"%s\"}",
           WiFi.localIP().toString().c_str(), FW_VERSAO);
  mqtt.publish(topicoStatus.c_str(), status, true);  // retained
  Serial.println("[mqtt] conectado");
}

void setup() {
  Serial.begin(115200);
  pinMode(PINO_LED, OUTPUT);

  WiFi.mode(WIFI_STA);
  String mac = WiFi.macAddress();  // "24:6F:28:A1:B2:C3"
  mac.replace(":", "");
  mac.toLowerCase();
  deviceId      = "esp8266-" + mac.substring(6);
  topicoLeitura = String(TOPICO_BASE) + "/" + deviceId + "/leitura";
  topicoStatus  = String(TOPICO_BASE) + "/" + deviceId + "/status";
  Serial.printf("\ndevice_id: %s\n", deviceId.c_str());

  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_SENHA);

  configTime(0, 0, "a.st1.ntp.br", "pool.ntp.org");  // UTC

#if HIVEMQ_CLOUD
  #if VALIDA_CERTIFICADO
    wifiCliente.setCACert(CA_RAIZ);
  #else
    wifiCliente.setInsecure();
  #endif
#endif

  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setBufferSize(512);

  SPI.begin();  // SCK=D5, MISO=D6, MOSI=D7 (fixos no ESP8266)
  rfid.PCD_Init();
  byte versao = rfid.PCD_ReadRegister(MFRC522::VersionReg);
  if (versao == 0x00 || versao == 0xFF) {
    Serial.println("[rfid] MFRC522 nao respondeu. Confira fiacao e alimentacao 3V3.");
  } else {
    Serial.printf("[rfid] MFRC522 ok, versao 0x%02X\n", versao);
  }
}

void loop() {
  conectaMqtt();
  mqtt.loop();
  drenaFila();

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid  = uidHex(rfid.uid);
  String tipo = rfid.PICC_GetTypeName(rfid.PICC_GetType(rfid.uid.sak));
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();

  unsigned long agora = millis();
  if (uid == ultimaUid && agora - ultimaUidMs < JANELA_REPETICAO_MS) {
    ultimaUidMs = agora;  // tag ainda no campo: mesma passagem
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
  Serial.println("========================");
  piscaLed();
  enfileira(payload);
  drenaFila();
}
