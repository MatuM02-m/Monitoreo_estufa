// Caracterización térmica de una estufa: DS18B20 -> flash + MQTT (TLS)
// loop(): mide y guarda. tareaRed(): WiFi/MQTT, para que una caída de red
// no frene las mediciones.
#include <Arduino.h>
#include <DallasTemperature.h>
#include <LittleFS.h>
#include <OneWire.h>
#include <PubSubClient.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <time.h>
#include "ca_cert.h"
#include "secrets.h"

const int PIN_DQ = 25;                // 4,7 kohm entre DQ y 3,3 V
const uint32_t PERIODO_MS = 10000;
const float OFFSET_C = 0.0f;          // termómetro patrón - sensor
const char* TOPICO = "estufa/temperatura";
const char* TOPICO_ESTADO = "estufa/estado";
const int MAX_ENSAYOS = 1000;
const size_t LARGO_COLA = 360;        // 1 h sin red

// Historial en la nube: un mensaje retenido por hora, con todas las lecturas.
// Tópico: estufa/hist/<ensayo % RANURAS>/<hora>; así un reinicio no pisa el
// ensayo anterior.
const int POR_BLOQUE = 360;
const int MAX_BLOQUES = 36;           // 36 h; después sigue solo en flash
const int RANURAS = 4;
const int16_t SIN_DATO = INT16_MIN;
int16_t hist[POR_BLOQUE * MAX_BLOQUES];  // centésimas de °C
volatile uint32_t nHist = 0;
uint32_t inicioMs = 0, inicioS = 0;
char idEnsayo[9];

struct Muestra {
  uint32_t s;                         // segundos desde el arranque
  float t;                            // NAN si la lectura es inválida
};

OneWire oneWire(PIN_DQ);
DallasTemperature sensor(&oneWire);
WiFiClientSecure red;
PubSubClient mqtt(red);
QueueHandle_t cola;
int ensayo = -1;
char archivo[24];
uint32_t proxima = 0;

void nombreEnsayo(int n, char* buf, size_t largo) {
  snprintf(buf, largo, "/ensayo%03d.csv", n);
}

uint32_t segundos() { return (uint32_t)(esp_timer_get_time() / 1000000LL); }

// Hora Unix de una muestra; 0 si todavía no hubo NTP
uint32_t epochDe(uint32_t s) {
  time_t ahora = time(nullptr);
  if (ahora < 1700000000) return 0;
  return (uint32_t)ahora - (segundos() - s);
}

const char* causaReset() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON: return "encendido/EN";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "wdt_int";
    case ESP_RST_TASK_WDT: return "wdt_tarea";
    case ESP_RST_WDT: return "wdt_otro";
    case ESP_RST_BROWNOUT: return "baja_tension";
    default: return "otra";
  }
}

// Diagnóstico retenido: estufa/diag/<ensayo % 8>
void publicarDiag() {
  char json[230], topico[32];
  snprintf(json, sizeof(json),
           "{\"ensayo\":%d,\"reset\":\"%s\",\"codigo\":%d,\"s\":%u,\"rssi\":%d,"
           "\"heap\":%u,\"heap_min\":%u,\"pila_red\":%u,\"epoch\":%u}",
           ensayo, causaReset(), (int)esp_reset_reason(), (unsigned)segundos(),
           WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
           (unsigned)uxTaskGetStackHighWaterMark(nullptr), (unsigned)epochDe(segundos()));
  snprintf(topico, sizeof(topico), "estufa/diag/%d", ensayo % 8);
  mqtt.publish(topico, json, true);
}

// Devuelve false si falló la publicación
bool publicarBloque(int b, int n) {
  static char json[2300];
  uint32_t epoch0 = epochDe(inicioS + (uint32_t)b * POR_BLOQUE * (PERIODO_MS / 1000));
  int largo = snprintf(json, sizeof(json),
      "{\"id\":\"%s\",\"ensayo\":%d,\"bloque\":%d,\"epoch0\":%u,\"p\":%u,\"t\":[",
      idEnsayo, ensayo, b, (unsigned)epoch0, (unsigned)(PERIODO_MS / 1000));
  for (int i = 0; i < n; i++) {
    int16_t v = hist[b * POR_BLOQUE + i];
    if (i) json[largo++] = ',';
    if (v == SIN_DATO) largo += snprintf(json + largo, sizeof(json) - largo, "null");
    else largo += snprintf(json + largo, sizeof(json) - largo, "%d", v);
  }
  snprintf(json + largo, sizeof(json) - largo, "]}");
  char topico[40];
  snprintf(topico, sizeof(topico), "estufa/hist/%d/%d", ensayo % RANURAS, b);
  return mqtt.publish(topico, json, true);
}

// Publica los bloques con lecturas nuevas: el actual, cada minuto
void publicarHistorial(uint16_t* publicado) {
  if (epochDe(segundos()) == 0) return;   // sin hora no se puede ubicar
  int total = nHist;
  for (int b = 0; b * POR_BLOQUE < total; b++) {
    int n = min(total - b * POR_BLOQUE, POR_BLOQUE);
    bool toca = publicado[b] == 0 || n == POR_BLOQUE || n - publicado[b] >= 6;
    if (n > publicado[b] && toca) {
      if (publicarBloque(b, n)) publicado[b] = n;
      return;                             // uno por vuelta
    }
  }
}

void tareaRed(void*) {
  static uint16_t publicado[MAX_BLOQUES] = {0};
  mqtt.setBufferSize(2600);
  red.setCACert(CA_RAIZ);
  red.setHandshakeTimeout(15);
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setSocketTimeout(10);
  mqtt.setKeepAlive(30);
  char id[32];
  snprintf(id, sizeof(id), "estufa-%012llx", ESP.getEfuseMac());
  uint32_t ultimo = 0, ultimoDiag = 0;
  bool primero = true;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED && !mqtt.connected() &&
        (primero || millis() - ultimo > 15000)) {
      primero = false;
      bool ok = mqtt.connect(id, MQTT_USER, MQTT_PASS, TOPICO_ESTADO, 1, true, "offline");
      ultimo = millis();
      if (ok) {
        mqtt.publish(TOPICO_ESTADO, "online", true);
        memset(publicado, 0, sizeof(publicado));   // reenviar todo
        publicarDiag();
        ultimoDiag = millis();
        Serial.println("# MQTT conectado");
      } else {
        char err[100] = "";
        red.lastError(err, sizeof(err));
        Serial.printf("# MQTT error rc=%d %s\n", mqtt.state(), err);
      }
    }
    if (mqtt.connected()) {
      mqtt.loop();
      Muestra m;
      while (xQueuePeek(cola, &m, 0) == pdTRUE) {
        char temp[12] = "null";
        if (!isnan(m.t)) snprintf(temp, sizeof(temp), "%.4f", m.t);
        char json[120];
        snprintf(json, sizeof(json), "{\"id\":\"%s\",\"ensayo\":%d,\"epoch\":%u,\"s\":%u,\"t\":%s}",
                 idEnsayo, ensayo, (unsigned)epochDe(m.s), (unsigned)m.s, temp);
        if (!mqtt.publish(TOPICO, json)) break;
        xQueueReceive(cola, &m, 0);
      }
      publicarHistorial(publicado);
      if (millis() - ultimoDiag > 60000) {
        ultimoDiag = millis();
        publicarDiag();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void volcar() {
  Serial.println("# INICIO");
  char n[24];
  for (int i = 0; i < MAX_ENSAYOS; i++) {
    nombreEnsayo(i, n, sizeof(n));
    if (!LittleFS.exists(n)) break;
    File f = LittleFS.open(n, "r");
    uint8_t buf[256];
    size_t leidos;
    while ((leidos = f.read(buf, sizeof(buf))) > 0) Serial.write(buf, leidos);
    f.close();
  }
  Serial.println("# FIN");
}

void comandos() {
  static String cmd;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') {
      if (cmd.length() < 16) cmd += c;
      if (cmd != "d") continue;       // 'd' no necesita Enter
    }
    if (cmd == "d") volcar();
    else if (cmd == "BORRAR") {
      Serial.println("# Borrando todos los ensayos y reiniciando");
      LittleFS.format();
      ESP.restart();
    }
    cmd = "";
  }
}

void setup() {
  Serial.begin(115200);
  if (!LittleFS.begin(true)) Serial.println("# ERROR: LittleFS");
  for (int i = 0; i < MAX_ENSAYOS; i++) {
    nombreEnsayo(i, archivo, sizeof(archivo));
    if (!LittleFS.exists(archivo)) { ensayo = i; break; }
  }
  File f = LittleFS.open(archivo, "a");
  if (f) {
    f.printf("# ensayo %d reset=%s (%d)\n", ensayo, causaReset(), (int)esp_reset_reason());
    f.close();
  }
  Serial.printf("# Ensayo %d, reset=%s, flash libre=%u kB\n", ensayo, causaReset(),
                (unsigned)((LittleFS.totalBytes() - LittleFS.usedBytes()) / 1024));

  sensor.begin();
  sensor.setResolution(12);
  Serial.printf("# Sensores encontrados: %d\n", sensor.getDeviceCount());

  for (auto& v : hist) v = SIN_DATO;
  snprintf(idEnsayo, sizeof(idEnsayo), "%08x", (unsigned)esp_random());
  inicioMs = proxima = millis();
  inicioS = segundos();

  cola = xQueueCreate(LARGO_COLA, sizeof(Muestra));
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
#ifdef WIFI_EAP_USER
#ifndef WIFI_EAP_IDENTITY
#define WIFI_EAP_IDENTITY WIFI_EAP_USER
#endif
#ifndef WIFI_EAP_CA
#error "Definir WIFI_EAP_CA (certificado CA de la red, PEM): sin él, un punto de acceso falso puede robar la clave"
#endif
  WiFi.begin(WIFI_SSID, WPA2_AUTH_PEAP, WIFI_EAP_IDENTITY, WIFI_EAP_USER,
             WIFI_EAP_PASS, WIFI_EAP_CA);
#else
  WiFi.begin(WIFI_SSID, WIFI_PASS);
#endif
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  // Núcleo 1: PubSubClient espera sin ceder CPU y en el 0 dispara el watchdog
  xTaskCreatePinnedToCore(tareaRed, "red", 12288, nullptr, 1, nullptr, 1);
}

void loop() {
  comandos();
  if ((int32_t)(millis() - proxima) < 0) { delay(10); return; }
  // Si hubo una demora (p. ej. el volcado), saltear los turnos perdidos
  proxima += (millis() - proxima) / PERIODO_MS * PERIODO_MS;
  uint32_t indice = (proxima - inicioMs) / PERIODO_MS;
  proxima += PERIODO_MS;

  Muestra m;
  sensor.requestTemperatures();       // bloquea hasta 750 ms
  m.s = segundos();
  float t = sensor.getTempCByIndex(0);
  // -127: desconectado. 85: valor de encendido, no esperable en incubación
  m.t = (t <= -55.0f || t == 85.0f) ? NAN : t + OFFSET_C;

  char temp[12] = "nan";
  if (!isnan(m.t)) snprintf(temp, sizeof(temp), "%.4f", m.t);
  char linea[64];
  snprintf(linea, sizeof(linea), "%d,%u,%u,%s", ensayo,
           (unsigned)epochDe(m.s), (unsigned)m.s, temp);
  Serial.println(linea);

  File f = LittleFS.open(archivo, "a");
  if (!f || f.println(linea) == 0) Serial.println("# ERROR: no se pudo guardar");
  if (f) f.close();

  if (indice < (uint32_t)(POR_BLOQUE * MAX_BLOQUES)) {
    hist[indice] = isnan(m.t) ? SIN_DATO : (int16_t)lroundf(m.t * 100);
    nHist = indice + 1;
  }

  if (xQueueSend(cola, &m, 0) != pdTRUE) {   // llena: descarta la más vieja
    Muestra vieja;
    xQueueReceive(cola, &vieja, 0);
    xQueueSend(cola, &m, 0);
  }
}
