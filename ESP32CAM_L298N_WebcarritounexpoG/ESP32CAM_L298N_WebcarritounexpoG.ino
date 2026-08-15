#include "esp_camera.h"
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define CAMERA_MODEL_AI_THINKER

// ============== CONFIGURACION INTERNET / WEBSOCKET ==============
// WiFi de tu casa: el carrito se conecta a esta red para salir a Internet
const char* WIFI_STA_SSID = "Gabriel";
const char* WIFI_STA_PASS = "pianoigbt";

// Servidor WebSocket en Internet
const char* WS_HOST = "arduino.libardo-apps.es";
const uint16_t WS_PORT = 443;   // puerto TLS estandar de wss:// (el backend Node corre en 3000 detras del proxy)
const char* WS_PATH = "/";

// 1 = tambien enviar frames de la camara por WebSocket (~2 fps, consume RAM/CPU)
#define WS_ENVIAR_VIDEO 0

WebSocketsClient webSocket;
// =================================================================

// Definidas en app_httpd.cpp (deben declararse ANTES de usarse en este archivo)
extern int speed;
void WheelAct(int speed_R, int speed_L, int nLf, int nLb, int nRf, int nRb);

// Pines globales (definidos aqui, usados tambien por app_httpd.cpp)
extern int gpLb = 14; // Left 1
extern int gpLf = 13; // Left 2
extern int gpRb = 33; // Right 1
extern int gpRf = 15; // Right 2
extern int gpLed = 4; // Light
extern int ENR = 2;
extern int ENL = 12;

#define LED   4
#define RXD2 14
#define TXD2 13
#define TRIG_PIN 1   // Disparo (Salida)
#define ECHO_PIN 16  // Eco (Entrada) - Ahora seguro sin PSRAM

int distanciaActual = 0; // Variable global para guardar la lectura

// 2. Función para medir 
int leerDistancia() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long tiempo = pulseIn(ECHO_PIN, HIGH, 10000); // 20ms de espera
  int d = tiempo * 0.034 / 2;
  return (d <= 0) ? 400 : d; 
}
void CameraWebServer_init();

// ============== FUNCIONES WEBSOCKET ==============
void enviarSaludo() {
  webSocket.sendTXT("{\"tipo\":\"hola\",\"nombre\":\"carro1\"}");
}

void procesarComandoWS(char* payload, size_t length) {
  Serial.printf("[WS] Comando recibido: %s\n", payload);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, length);
  if (err) {
    Serial.print("[WS] JSON invalido: ");
    Serial.println(err.c_str());
    return;
  }

  const char* cmd = doc["cmd"] | "";
  int val = doc["val"] | 0;

  // Compatibilidad con el formato de la web local (?var=car&val=1)
  if (strcmp(cmd, "") == 0 && doc.containsKey("var")) {
    const char* var = doc["var"] | "";
    if (strcmp(var, "car") == 0) {
      if      (val == 1) cmd = "adelante";
      else if (val == 2) cmd = "derecha";
      else if (val == 3) cmd = "parar";
      else if (val == 4) cmd = "izquierda";
      else if (val == 5) cmd = "atras";
    }
    else if (strcmp(var, "speed") == 0) cmd = "velocidad";
    else if (strcmp(var, "flash") == 0) cmd = "luz";
  }

  if      (strcmp(cmd, "adelante")  == 0) {
    Serial.println("[WS] Ejecutando: ADELANTE (freno automatico <15cm)");
    int dist = leerDistancia();
    if (dist > 15) WheelAct(speed, speed, 1, 0, 1, 0);  // frena si hay obstaculo
    else           WheelAct(0, 0, 0, 0, 0, 0);
  }
  else if (strcmp(cmd, "atras")     == 0) { Serial.println("[WS] Ejecutando: ATRAS");     WheelAct(speed, speed, 0, 1, 0, 1); }
  else if (strcmp(cmd, "izquierda") == 0) { Serial.println("[WS] Ejecutando: IZQUIERDA"); WheelAct(speed, speed, 1, 0, 0, 1); }
  else if (strcmp(cmd, "derecha")   == 0) { Serial.println("[WS] Ejecutando: DERECHA");   WheelAct(speed, speed, 0, 1, 1, 0); }
  else if (strcmp(cmd, "parar")     == 0) { Serial.println("[WS] Ejecutando: PARAR");     WheelAct(0, 0, 0, 0, 0, 0); }
  else if (strcmp(cmd, "velocidad") == 0) { speed = constrain(val, 0, 255); Serial.printf("[WS] Velocidad ajustada a %d\n", speed); }
  else if (strcmp(cmd, "luz")       == 0) { ledcWrite(gpLed, constrain(val, 0, 255)); Serial.printf("[WS] Luz LED: %d\n", constrain(val, 0, 255)); }
  else {
    Serial.print("[WS] Comando desconocido: ");
    Serial.println(cmd);
  }
}

void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_ERROR:
      Serial.print("[WS] Error de conexion");
      if (payload && length) {
        Serial.print(": ");
        Serial.print((const char*)payload);
      }
      Serial.println(" (reintentando en 5s...)");
      break;
    case WStype_DISCONNECTED:
      Serial.println("[WS] Desconectado del servidor (reintentando en 5s...)");
      WheelAct(0, 0, 0, 0, 0, 0);   // seguridad: frenar al perder conexion
      break;
    case WStype_CONNECTED:
      Serial.println("[WS] Conectado correctamente al servidor");
      enviarSaludo();
      break;
    case WStype_TEXT:
      procesarComandoWS((char*)payload, length);
      break;
    default:
      break;
  }
}
// ==================================================

WiFiServer server(100);


void initMotors()
{
  pinMode(gpLb, OUTPUT); //Left Backward
  pinMode(gpLf, OUTPUT); //Left Forward
  pinMode(gpRb, OUTPUT); //Right Forward
  pinMode(gpRf, OUTPUT); //Right Backward
  pinMode(gpLed, OUTPUT); //Light
  pinMode(ENR, OUTPUT);
  pinMode(ENL, OUTPUT);

  // API nueva de esp32 core 3.x: ledcAttachChannel(pin, freq, resolucion, canal)
  ledcAttachChannel(ENR, 5000, 8, 2);   // motor derecho (canal 2)
  ledcAttachChannel(ENL, 5000, 8, 12);  // motor izquierdo (canal 12)
  ledcWrite(ENR, 0);
  ledcWrite(ENL, 0);
  digitalWrite(gpLf, LOW);
  digitalWrite(gpRb, LOW);
  digitalWrite(gpRf, LOW);
}

void setup()
{
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // prevent brownouts by silencing them

  Serial.begin(115200);
  Serial2.begin(115200, SERIAL_8N1, RXD2, TXD2);
  Serial.setDebugOutput(true);
  Serial.println();
    
  CameraWebServer_init(); 
  

  // Remote Control Car
  initMotors();

  ledcAttachChannel(gpLed, 5000, 8, 7);  //pin4 is LED (canal 7)

  server.begin();

  for (int i = 0; i < 5; i++) 
  {
    ledcWrite(gpLed, 10); // flash led
    delay(50);
    ledcWrite(gpLed, 0);
    delay(50);
  }
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  digitalWrite(TRIG_PIN, LOW);

  // ---- Cliente WebSocket hacia Internet ----
  Serial.printf("[WS] Intentando conectar a wss://%s:%d%s ...\n", WS_HOST, WS_PORT, WS_PATH);
  webSocket.beginSSL(WS_HOST, WS_PORT, WS_PATH);
  // Sin fingerprint/CA, la libreria WebSockets ya desactiva la verificacion del certificado TLS
  webSocket.onEvent(webSocketEvent);
  webSocket.setReconnectInterval(5000);   // reintenta cada 5 s
  webSocket.enableHeartbeat(15000, 3000, 2);
}

unsigned long lastTele = 0;
#if WS_ENVIAR_VIDEO
unsigned long lastFrame = 0;
#endif

void loop() 
{
  webSocket.loop();

  // Telemetria: distancia cada 1 s
  if (millis() - lastTele >= 1000) {
    lastTele = millis();
    if (webSocket.isConnected()) {
      char buf[64];
      snprintf(buf, sizeof(buf), "{\"distancia\":%d}", leerDistancia());
      webSocket.sendTXT(buf);
    }
  }

#if WS_ENVIAR_VIDEO
  // Video: un frame JPEG cada ~500 ms hacia el servidor
  if (webSocket.isConnected() && millis() - lastFrame >= 500) {
    lastFrame = millis();
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      webSocket.sendBIN(fb->buf, fb->len);
      esp_camera_fb_return(fb);
    }
  }
#endif
}
