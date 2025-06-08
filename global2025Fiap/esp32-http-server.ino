#include <WiFi.h>
#include <PubSubClient.h>

#define TRIG_PIN       18     // trig do ultrassom
#define ECHO_PIN       19     // echo do ultrassom
#define BUZZER_PIN     21     // saída digital para buzzer
#define POT_PIN        34     // entrada analógica para o pot (simula boia)

const float LIMIAR_CHEIO    = 20.0;      // cm: ultrassom
const int   LIMIAR_BOIA_ADC = 2000;      // ADC (0–4095) acima do qual consideramos “boia acionada”
const unsigned long INTERVALO_MEDIR = 5000;  // 5 s entre cada leitura/MQTT

// Buzzer: 2 s ON / 4 s OFF quando “nível cheio”
const unsigned long DURA_ON  = 2000;
const unsigned long DURA_OFF = 4000;

const char* ssid       = "Wokwi-GUEST";
const char* password   = "";

// Configuração CloudAMQP (RabbitMQ/MQTT)
const char* mqttServer = "albatross.rmq.cloudamqp.com";
const int   mqttPort   = 1883;
const char* mqttUser   = "iuyaxzit:iuyaxzit";
const char* mqttPass   = "H3sN2i4mQZFp_7dGVj9OepJpkZm16DpW";

WiFiClient   espClient;
PubSubClient client(espClient);

unsigned long ultimoMeasureMillis = 0;
unsigned long ultimoBeepMillis    = 0;
bool nivelCheio                   = false;
bool buzzerLigado                 = false;

void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);

  pinMode(POT_PIN, INPUT);

  // Conecta Wi-Fi
  Serial.print("[Wi-Fi] Conectando a ");
  Serial.println(ssid);
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("[Wi-Fi] Conectado!");
  Serial.print("         IP: ");
  Serial.println(WiFi.localIP());
  Serial.println("--------------------------------");

  // Conecta MQTT/RabbitMQ
  client.setServer(mqttServer, mqttPort);
  Serial.print("[MQTT] Conectando ao broker ");
  Serial.println(mqttServer);
  while (!client.connected()) {
    Serial.print("[MQTT] Tentando conexão");
    if (client.connect("ralo01", mqttUser, mqttPass)) {
      Serial.println(" → Conectado!");
    } else {
      Serial.print(" → Falha, rc=");
      Serial.print(client.state());
      Serial.println(" tentando de novo...");
      delay(500);
    }
  }
  Serial.println("[MQTT] Pronto para publicar mensagens");
  Serial.println("================================");
}

long measureDistance() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return 999;
  return (duration * 0.034) / 2;
}

void loop() {
  unsigned long agora = millis();

  // 1) Garante conexão MQTT
  if (!client.connected()) {
    Serial.println("[MQTT] Reconectando...");
    if (client.connect("esp32_ralo", mqttUser, mqttPass)) {
      Serial.println("[MQTT] Reconectado!");
    } else {
      Serial.print("[MQTT] Erro reconexão, rc=");
      Serial.println(client.state());
      delay(500);
      return;
    }
  }

  // 2) A cada INTERVALO, lê ultrassom + pot e publica no MQTT
  if (agora - ultimoMeasureMillis >= INTERVALO_MEDIR) {
    ultimoMeasureMillis = agora;

    Serial.println();
    Serial.print("[Tempo] ");
    Serial.print(agora / 1000.0, 2);
    Serial.println(" s desde o boot");
    Serial.println("---- Medição ----");

    // 2.1) Leitura do ultrassom
    long dist = measureDistance();
    if (dist == 999) {
      Serial.println("[Ultrassom] Leitura inválida (sem eco)");
    } else {
      Serial.print("[Ultrassom] Distância: ");
      Serial.print(dist);
      Serial.println(" cm");
    }

    // 2.2) Leitura do potenciômetro (simula float switch)
    int adcValue = analogRead(POT_PIN);  // 0 .. 4095
    bool boiaAcionada = (adcValue >= LIMIAR_BOIA_ADC);
    Serial.print("[Pot] ADC: ");
    Serial.print(adcValue);
    Serial.print(" → Boia: ");
    Serial.println(boiaAcionada ? "SIM" : "NÃO");

    // 2.3) Define nivelCheio
    if ((dist != 999 && dist <= LIMIAR_CHEIO) || boiaAcionada) {
      if (!nivelCheio) {
        Serial.println("[Alerta] Nível atingiu o limiar (CHEIO)!");
      }
      nivelCheio = true;
    } else {
      if (nivelCheio) {
        Serial.println("[Info] Nível abaixo do limiar (normal)");
      }
      nivelCheio = false;
    }

    // 2.4) Se nível não cheio, garante buzzer OFF e reseta timer
    if (!nivelCheio) {
      digitalWrite(BUZZER_PIN, LOW);
      buzzerLigado = false;
      ultimoBeepMillis = agora;
    }

    // 2.5) Monta o JSON incluindo “boia” no payload
    String payload = String("{"
      "\"id\":\"ralo01\","
      "\"nivel\":") + String(dist) + "," +
      "\"boia\":" + String(boiaAcionada ? "true" : "false") + "," +
      "\"timestamp\":\"" + String(agora) + "\"}";

    client.publish("/ralo/ralo01/nivel", payload.c_str());
    Serial.print("[MQTT] Publicado: ");
    Serial.println(payload);
    Serial.println("------------------");
  }

  // 3) Controle do buzzer: somente se nivelCheio == true
  if (nivelCheio) {
    if (buzzerLigado) {
      // Se buzzer está ON, aguarda DURA_ON para desligar
      if (agora - ultimoBeepMillis >= DURA_ON) {
        digitalWrite(BUZZER_PIN, LOW);
        buzzerLigado = false;
        ultimoBeepMillis = agora;
        Serial.println("[Buzzer] OFF (pausa de 4 s)");
      }
    } else {
      // Se buzzer está OFF, aguarda DURA_OFF para ligar
      if (agora - ultimoBeepMillis >= DURA_OFF) {
        digitalWrite(BUZZER_PIN, HIGH);
        buzzerLigado = true;
        ultimoBeepMillis = agora;
        Serial.println("[Buzzer] ON (tocando 2 s)");
      }
    }
  }

  // Sem delay() para não bloquear o millis() do buzzer
}