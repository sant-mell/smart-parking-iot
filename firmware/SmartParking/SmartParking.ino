#include <Wire.h>
#include <LCD_I2C.h>
#include <DHT.h>
#include <ESP32Servo.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <time.h>
#include <HTTPClient.h>

//                           PINES                          
#define TRIG_PIN   4
#define ECHO_PIN   5

#define LDR_PIN    34
#define SERVO_PIN  32

#define DHTPIN     25
#define DHTTYPE    DHT11

#define LED_VERDE  2
#define LED_ROJO   15
#define LUZ_PIN    12

#define LCD_ADDR   0x27

//                           UMBRALES Y CONSTANTES                          
const float DISTANCIA_NORMAL_CM = 9.60;    // distancia normal sin coche
const float DELTA_ALERTA_CM     = 1.0;     // cuando se reduce 1 cm
const float UMBRAL_COCHE_CM     = DISTANCIA_NORMAL_CM - DELTA_ALERTA_CM; // 8.60 cm

const int   LDR_UMBRAL          = 2000;   // menor = más oscuro
const float TEMP_ALTA           = 35.0;
const float HUM_ALTA            = 80.0;

// Servo: ajusta si tu mecánica es al revés
const int ANGULO_CERRADO = 90;  // barrera abajo
const int ANGULO_ABIERTO = 0;   // barrera arriba

// NTP / hora local (México)
const char* ntpServer           = "pool.ntp.org";
const long  gmtOffset_sec       = -6 * 3600;
const int   daylightOffset_sec  = 3600;

// Tarifa
const float TARIFA_BASE_30      = 10.0;   // base por cada 30 min
const unsigned long ESPERA_CIERRE_MS    = 2000;   // 2 s
const unsigned long TIMEOUT_SALIDA_MS   = 10000;  // seguridad salida
const unsigned long SENSOR_INTERVAL_MS  = 1000;   // cada 1 s
const unsigned long PUBLISH_INTERVAL_MS = 60000;  // cada 60 s (para no chocar rate limit)
const unsigned long LCD_INTERVAL_MS     = 2000;   // cada 2 s

//                           OBJETOS                          
DHT dht(DHTPIN, DHTTYPE);
LCD_I2C lcd(LCD_ADDR, 16, 2);
Servo barrera;
WiFiClient espClient;
PubSubClient client_mqtt(espClient);

//                           WIFI
// NOTE: move these to a separate (git-ignored) secrets header before flashing.
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

//                           MQTT THINGSPEAK                          
// Fields:
//  field1 = tiempo ocupado (min simulados)
//  field2 = temperatura (°C)
//  field3 = humedad (%)
//  field4 = estado de barrera (0=cerrada, 1=abierta)
//  field5 = ocupado (0=libre, 1=ocupado)
//  field6 = tarifa por 30 min
//  field7 = comando (1=entrada, 2=salida) [recibido]
//  field8 = tarifa total
const char* broker       = "mqtt3.thingspeak.com";
int         puerto       = 1883;

const char* id_cliente   = "YOUR_MQTT_CLIENT_ID";
const char* mqttUser     = "YOUR_MQTT_USERNAME";
const char* mqttPass     = "YOUR_MQTT_PASSWORD";

const char* mqttTopicPub = "channels/YOUR_CHANNEL_ID/publish";                // publicar datos
const char* mqttTopicSub = "channels/YOUR_CHANNEL_ID/subscribe/fields/field7"; // comandos

// --- HTTP para leer comandos (field7) desde ThingSpeak ---
const char* TS_READ_API_KEY = "YOUR_THINGSPEAK_READ_KEY";   // Read API Key
const char* TS_CHANNEL_ID   = "YOUR_CHANNEL_ID";

unsigned long lastCmdCheck = 0;
const unsigned long CMD_CHECK_INTERVAL_MS = 5000;   // revisar comando cada 5 s
long lastCommandEntryId = -1;                       // para no repetir mismo comando

//                           ESTADO GLOBAL                          
volatile int comandoBarrera = 0;   // 0=nada, 1=entrada, 2=salida

bool cochePresente = false;
bool gateOpen      = false;

// 0 = idle, 1 = entrada, 2 = salida
int  gateMode = 0;

unsigned long entryDetectTime = 0;
bool          entryDetectionStarted = false;

unsigned long exitOpenTime = 0;

float lastDist   = -1.0;
float lastTemp   = NAN;
float lastHum    = NAN;
int   lastLdr    = 0;
int   ocupado    = 0;          // 0 libre, 1 ocupado
int   estadoAnterior = 0;      // transición 1->0

float       currentMultiplicador = 1.0;
float       currentTarifa30      = TARIFA_BASE_30;
char        tarifaEtiqueta       = 'N';
float       tarifaTotal          = 0.0;

// NUEVO: tiempo ocupado en minutos simulados
float       tiempoOcupadoMin     = 0.0;

bool alarmaClima = false;

// tiempos generales
unsigned long lastSensorRead      = 0;
unsigned long lastPublishTime     = 0;
unsigned long lastScreenUpdate    = 0;

// reloj simulado: 1 s real = 30 min simulados
time_t        baseSimEpoch   = 0;
unsigned long baseSimMillis  = 0;
bool          relojListo     = false;


//                           FUNCIONES                          
float leerDistanciaCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);

  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long dur = pulseIn(ECHO_PIN, HIGH, 30000);
  if (dur == 0) return -1.0;

  return (dur * 0.0343f) / 2.0f;
}

float calcularMultiplicador(const struct tm &timeinfo) {
  int h = timeinfo.tm_hour;

  if ((h >= 7 && h < 10) || (h >= 17 && h < 20)) {
    tarifaEtiqueta = 'P';
    return 1.5f;
  } else if (h >= 0 && h < 6) {
    tarifaEtiqueta = 'B';
    return 0.5f;
  } else {
    tarifaEtiqueta = 'N';
    return 1.0f;
  }
}

bool getSimTime(struct tm *timeinfo) {
  if (!relojListo) return false;

  unsigned long elapsedMs = millis() - baseSimMillis;
  time_t simEpoch = baseSimEpoch + (elapsedMs / 1000) * 1800; // 30 min por s

  return (localtime_r(&simEpoch, timeinfo) != nullptr);
}

//                           BARRERA                          
void abrirBarrera() {
  barrera.write(ANGULO_ABIERTO);
  Serial.println("Barrera ABIERTA");
}

void cerrarBarrera() {
  barrera.write(ANGULO_CERRADO);
  Serial.println("Barrera CERRADA");
}

void manejarBarrera(unsigned long now) {
  // Procesar comando recibido
  if (comandoBarrera != 0 && gateMode == 0 && !gateOpen) {
    if (comandoBarrera == 1) {
      Serial.println("Comando: ENTRADA -> abrir barrera");
      gateMode = 1;
      gateOpen = true;
      entryDetectionStarted = false;
      abrirBarrera();
    } else if (comandoBarrera == 2) {
      Serial.println("Comando: SALIDA -> abrir barrera");
      gateMode = 2;
      gateOpen = true;
      exitOpenTime = now;
      abrirBarrera();
    }
    comandoBarrera = 0;
  }

  if (!gateOpen || gateMode == 0) return;

  if (gateMode == 1) {
    // ENTRADA: al detectar coche, esperar 2 s y cerrar
    if (cochePresente && !entryDetectionStarted) {
      entryDetectionStarted = true;
      entryDetectTime = now;
      Serial.println("Coche detectado (entrada), contando 2s...");
    }
    if (entryDetectionStarted && (now - entryDetectTime >= ESPERA_CIERRE_MS)) {
      Serial.println("Cerrando barrera después de 2s (entrada).");
      cerrarBarrera();
      gateOpen = false;
      gateMode = 0;
      entryDetectionStarted = false;
    }

  } else if (gateMode == 2) {
    // SALIDA: al dejar de detectar coche, esperar 2 s y cerrar
    if (!cochePresente && (now - exitOpenTime >= ESPERA_CIERRE_MS)) {
      Serial.println("Cerrando barrera 2s después de que se fue el coche (salida).");
      cerrarBarrera();
      gateOpen = false;
      gateMode = 0;
    } else if (now - exitOpenTime > TIMEOUT_SALIDA_MS) {
      Serial.println("Timeout salida: cerrando barrera por seguridad.");
      cerrarBarrera();
      gateOpen = false;
      gateMode = 0;
    }
  }
}

//                           MQTT CALLBACK                          
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg;
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  Serial.print("MQTT mensaje en [");
  Serial.print(topic);
  Serial.print("]: ");
  Serial.println(msg);

  if (String(topic) == mqttTopicSub) {
    int valor = msg.toInt();   // 1 = entrada, 2 = salida
    comandoBarrera = valor;
    Serial.print("comandoBarrera (MQTT) = ");
    Serial.println(comandoBarrera);
  }
}

//                           WIFI / MQTT                          
void conectarWiFi() {
  Serial.print("Conectando a WiFi ");
  Serial.println(ssid);

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi conectado.");
}

void conectarMQTT() {
  client_mqtt.setServer(broker, puerto);
  client_mqtt.setCallback(mqttCallback);

  while (!client_mqtt.connected()) {
    Serial.print("Conectando a MQTT...");
    if (client_mqtt.connect(id_cliente, mqttUser, mqttPass)) {
      Serial.println(" conectado.");
      if (client_mqtt.subscribe(mqttTopicSub)) {
        Serial.print("Suscrito a ");
        Serial.println(mqttTopicSub);
      } else {
        Serial.println("Fallo al suscribirse al topic");
      }
    } else {
      Serial.print(" fallo, rc=");
      Serial.print(client_mqtt.state());
      Serial.println(" reintentando en 2s...");
      delay(2000);
    }
  }
}

//                           THINGSPEAK PUBLISH                          
void enviarThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (!client_mqtt.connected())    return;

  // 0 = cerrada, 1 = abierta
  int estadoBarrera = gateOpen ? 1 : 0;

  // field1 ahora es tiempoOcupadoMin
  String datos = "field1=" + String(tiempoOcupadoMin, 1) +
                 "&field2=" + String(lastTemp, 1) +
                 "&field3=" + String(lastHum, 1) +
                 "&field4=" + String(estadoBarrera) +
                 "&field5=" + String(ocupado) +
                 "&field6=" + String(currentTarifa30, 2) +
                 "&field8=" + String(tarifaTotal, 2);

  bool ok = client_mqtt.publish(mqttTopicPub, datos.c_str());

  Serial.print("Publicando: ");
  Serial.println(datos);
  Serial.println(ok ? "MQTT publish OK" : "MQTT publish FAIL");
}

//                           THINGSPEAK HTTP COMMAND POLLING                          
void checarComandoHTTP(unsigned long now) {
  if (WiFi.status() != WL_CONNECTED) return;
  if (now - lastCmdCheck < CMD_CHECK_INTERVAL_MS) return;
  lastCmdCheck = now;

  HTTPClient http;
  String url = "http://api.thingspeak.com/channels/";
  url += TS_CHANNEL_ID;
  url += "/fields/7/last.json?api_key=";
  url += TS_READ_API_KEY;

  http.begin(url);
  int httpCode = http.GET();
  if (httpCode != 200) {
    Serial.print("HTTP comando error: ");
    Serial.println(httpCode);
    http.end();
    return;
  }

  String payload = http.getString();
  http.end();

  // Sacar entry_id para saber si es un comando nuevo
  int idxEntry = payload.indexOf("\"entry_id\":");
  if (idxEntry < 0) return;
  idxEntry += 11;
  int endEntry = payload.indexOf(",", idxEntry);
  long entryId = payload.substring(idxEntry, endEntry).toInt();

  if (entryId == lastCommandEntryId) return;
  lastCommandEntryId = entryId;

  // Sacar valor de field7
  int idxField = payload.indexOf("\"field7\":\"");
  if (idxField < 0) return;
  idxField += 10;
  int endField = payload.indexOf("\"", idxField);
  String valStr = payload.substring(idxField, endField);
  int v = valStr.toInt();

  if (v == 1 || v == 2) {
    comandoBarrera = v;
    Serial.print("Nuevo comandoBarrera (HTTP) = ");
    Serial.println(comandoBarrera);
  }
}

//                           LCD                          
void actualizarLCD() {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("T:");
  if (!isnan(lastTemp)) lcd.print((int)lastTemp);
  else lcd.print("--");
  lcd.print(" H:");
  if (!isnan(lastHum)) lcd.print((int)lastHum);
  else lcd.print("--");
  lcd.print(" O:");
  lcd.print(ocupado);

  lcd.setCursor(0, 1);
  lcd.print("T30:");
  lcd.print(currentTarifa30, 1);
  lcd.print(" ");
  lcd.print(tarifaEtiqueta);
  lcd.print(" ");

  struct tm timeinfo;
  if (getSimTime(&timeinfo)) {
    if (timeinfo.tm_hour < 10) lcd.print("0");
    lcd.print(timeinfo.tm_hour);
    lcd.print(":");
    if (timeinfo.tm_min < 10) lcd.print("0");
    lcd.print(timeinfo.tm_min);
  } else {
    lcd.print("--:--");
  }
}

//                           SETUP                          
void setup() {
  Serial.begin(115200);

  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  pinMode(LUZ_PIN, OUTPUT);
  pinMode(LED_VERDE, OUTPUT);
  pinMode(LED_ROJO, OUTPUT);

  digitalWrite(LUZ_PIN, LOW);
  digitalWrite(LED_VERDE, LOW);
  digitalWrite(LED_ROJO, LOW);

  dht.begin();

  Wire.begin(21, 22);
  lcd.begin();
  lcd.backlight();

  barrera.attach(SERVO_PIN);
  cerrarBarrera();
  gateOpen = false;
  gateMode = 0;

  conectarWiFi();
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    baseSimEpoch  = mktime(&timeinfo);
    baseSimMillis = millis();
    relojListo    = true;
  } else {
    relojListo = false;
  }

  conectarMQTT();

  lcd.clear();
  lcd.print("Estacionamiento");
  lcd.setCursor(0, 1);
  lcd.print("Inicializando...");
  delay(1500);
}

//                           LOOP                          
void loop() {
  if (!client_mqtt.connected()) {
    conectarMQTT();
  }
  client_mqtt.loop();

  unsigned long now = millis();

  // Lecturas de sensores cada 1 s
  if (now - lastSensorRead >= SENSOR_INTERVAL_MS) {
    lastSensorRead = now;

    // Ultrasonico (solo para lógica local)
    lastDist = leerDistanciaCm();
    cochePresente = (lastDist > 0 && lastDist <= UMBRAL_COCHE_CM);
    ocupado = cochePresente ? 1 : 0;

    // transición ocupado -> libre => reset tarifaTotal, tiempoOcupadoMin y field8
    if (estadoAnterior == 1 && ocupado == 0) {
      Serial.println("Vehiculo salió: Reiniciando tarifaTotal y tiempoOcupadoMin.");
      tarifaTotal = 0.0;
      tiempoOcupadoMin = 0.0;

      if (client_mqtt.connected()) {
        String datosReset = "field8=0&field1=0";
        client_mqtt.publish(mqttTopicPub, datosReset.c_str());
      }
    }
    estadoAnterior = ocupado;

    // LDR
    lastLdr = analogRead(LDR_PIN);
    bool oscuro = (lastLdr < LDR_UMBRAL);
    digitalWrite(LUZ_PIN, oscuro ? HIGH : LOW);

    // LEDs según ocupación
    if (cochePresente) {
      digitalWrite(LED_VERDE, LOW);
      digitalWrite(LED_ROJO, HIGH);
    } else {
      digitalWrite(LED_VERDE, HIGH);
      digitalWrite(LED_ROJO, LOW);
    }

    // DHT
    float t = dht.readTemperature();
    float h = dht.readHumidity();
    if (!isnan(t)) lastTemp = t;
    if (!isnan(h)) lastHum  = h;

    alarmaClima = (!isnan(lastTemp) && lastTemp > TEMP_ALTA) ||
                  (!isnan(lastHum)  && lastHum  > HUM_ALTA);
    if (alarmaClima) {
      Serial.println("ALERTA: Temperatura/Humedad anormal.");
    }

    // Tarifa dinámica
    struct tm simTime;
    if (getSimTime(&simTime)) {
      currentMultiplicador = calcularMultiplicador(simTime);
      currentTarifa30 = TARIFA_BASE_30 * currentMultiplicador;
    } else {
      currentMultiplicador = 1.0f;
      currentTarifa30 = TARIFA_BASE_30;
      tarifaEtiqueta = 'N';
    }

    // Cobro y tiempo ocupado: cada segundo = 30 min simulados
    if (ocupado && relojListo) {
      tarifaTotal += currentTarifa30;
      tiempoOcupadoMin += 30.0f;  // sumamos 30 minutos simulados
      Serial.print("tarifaTotal: ");
      Serial.println(tarifaTotal, 2);
      Serial.print("tiempoOcupadoMin: ");
      Serial.println(tiempoOcupadoMin, 1);
    }
  }

  // Leer comandos (field7) por HTTP en ThingSpeak
  checarComandoHTTP(now);

  // Lógica de la barrera
  manejarBarrera(now);

  // Publicar a ThingSpeak (datos) cada 60 s
  if (now - lastPublishTime >= PUBLISH_INTERVAL_MS) {
    lastPublishTime = now;
    enviarThingSpeak();
  }

  // Actualizar LCD cada 2 s
  if (now - lastScreenUpdate >= LCD_INTERVAL_MS) {
    lastScreenUpdate = now;
    actualizarLCD();
  }
}
