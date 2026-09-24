#!/usr/bin/env node
/*
  Simulador do leitor RFID: faz no PC exatamente o que o ESP32 faria.
  Mesmos tópicos, mesmo formato de payload, mesmo Last Will.

  Instalação:
    npm install mqtt
  Uso:
    node simulador_esp32.js
    node simulador_esp32.js --auto 3      (publica sozinho a cada 3 s)
    node simulador_esp32.js --device esp32-bancada2
*/

const mqtt = require("mqtt");

// ============ CONFIGURAÇÃO ============
const CONFIG = {
  // HiveMQ Cloud (mesma porta que o ESP32 usa):
  broker: "mqtts://c115e6f71aae4b3eae2d554b93ea0250.s1.eu.hivemq.cloud:8883",
  // Mosquitto local:  "mqtt://localhost:1883"
  // HiveMQ público:   "mqtt://broker.hivemq.com:1883"

  // Credencial de Access Management. Melhor vir de variável de ambiente
  // do que ficar escrita aqui:  MQTT_USUARIO=... MQTT_SENHA=... node simulador_esp32.js
  usuario: "Alisson",
  senha: "Alisson_111",
  topicoBase: "cafe/teste/leitor",
  deviceId: "esp32-simulado",

  // Tags de mentira. UID de 4 bytes, como um cartão MIFARE.
  tags: [
    { uid: "04A1B2C3", tipo: "MIFARE 1KB" },
    { uid: "93F10A7E", tipo: "MIFARE 1KB" },
    { uid: "5D22C08B", tipo: "MIFARE 1KB" },
    { uid: "A743E902", tipo: "MIFARE Ultralight" },
    { uid: "1CF8B640", tipo: "MIFARE 1KB" },
  ],
};
// ======================================

const args = process.argv.slice(2);
const opcao = (nome, padrao) => {
  const i = args.indexOf("--" + nome);
  return i >= 0 && args[i + 1] ? args[i + 1] : padrao;
};
const intervaloAuto = Number(opcao("auto", 0));
CONFIG.deviceId = opcao("device", CONFIG.deviceId);

const topicoLeitura = `${CONFIG.topicoBase}/${CONFIG.deviceId}/leitura`;
const topicoStatus = `${CONFIG.topicoBase}/${CONFIG.deviceId}/status`;

let seq = 0;
let timerAuto = null;

const cliente = mqtt.connect(CONFIG.broker, {
  clientId: CONFIG.deviceId,
  username: CONFIG.usuario || undefined,
  password: CONFIG.senha || undefined,
  clean: true,
  reconnectPeriod: 2000,
  // Last Will: se este processo morrer, o broker publica "offline" sozinho
  will: { topic: topicoStatus, payload: JSON.stringify({ estado: "offline" }), qos: 0, retain: true },
});

cliente.on("connect", () => {
  cliente.publish(topicoStatus, JSON.stringify({ estado: "online", ip: "127.0.0.1", fw: "sim-0.1.0" }), { retain: true });
  console.log(`conectado em ${CONFIG.broker}`);
  console.log(`publicando em ${topicoLeitura}\n`);
  mostraAjuda();
  if (intervaloAuto > 0) ligaAuto(intervaloAuto);
});

cliente.on("error", (e) => {
  console.error("erro:", e.message);
  if (/Not authorized|Bad username/i.test(e.message)) {
    console.error("Preencha CONFIG.usuario e CONFIG.senha com a credencial de Access Management.");
    process.exit(1);
  }
});

function publicaLeitura(indice) {
  const tag = CONFIG.tags[indice % CONFIG.tags.length];
  const payload = {
    device_id: CONFIG.deviceId,
    uid: tag.uid,
    tipo: tag.tipo,
    ts: Math.floor(Date.now() / 1000),
    seq: ++seq,
    rssi_wifi: -45 - Math.floor(Math.random() * 25),
  };
  cliente.publish(topicoLeitura, JSON.stringify(payload));
  console.log(`tag ${tag.uid}  seq ${payload.seq}`);
}

function ligaAuto(segundos) {
  if (timerAuto) { clearInterval(timerAuto); timerAuto = null; console.log("automático desligado"); return; }
  console.log(`automático ligado: uma tag a cada ${segundos} s`);
  let i = 0;
  timerAuto = setInterval(() => publicaLeitura(i++), segundos * 1000);
}

function simulaReinicio() {
  console.log("reiniciando o leitor: seq volta para 1");
  cliente.publish(topicoStatus, JSON.stringify({ estado: "offline" }), { retain: true });
  seq = 0;
  setTimeout(() => {
    cliente.publish(topicoStatus, JSON.stringify({ estado: "online", ip: "127.0.0.1", fw: "sim-0.1.0" }), { retain: true });
    console.log("leitor de volta");
  }, 1500);
}

function simulaPerda() {
  seq += 3;   // avança o contador sem publicar: o painel mostra a lacuna
  console.log("3 leituras perdidas (buffer estourou)");
}

function mostraAjuda() {
  console.log("1 a 5  passar uma tag no leitor");
  console.log("a      liga ou desliga o envio automático");
  console.log("r      simular reinício do ESP32");
  console.log("p      simular perda de leituras");
  console.log("q      sair\n");
}

function encerra() {
  cliente.publish(topicoStatus, JSON.stringify({ estado: "offline" }), { retain: true }, () => {
    cliente.end(false, () => process.exit(0));
  });
}

if (process.stdin.isTTY) {
  process.stdin.setRawMode(true);
  process.stdin.resume();
  process.stdin.setEncoding("utf8");
  process.stdin.on("data", (tecla) => {
    if (tecla === "\u0003" || tecla === "q") return encerra();
    if (tecla >= "1" && tecla <= "9") return publicaLeitura(Number(tecla) - 1);
    if (tecla === "a") return ligaAuto(intervaloAuto > 0 ? intervaloAuto : 3);
    if (tecla === "r") return simulaReinicio();
    if (tecla === "p") return simulaPerda();
    if (tecla === "h") return mostraAjuda();
  });
} else if (intervaloAuto === 0) {
  console.log("sem teclado disponível: ligando o modo automático a cada 3 s");
  cliente.on("connect", () => ligaAuto(3));
}

process.on("SIGINT", encerra);
