#include "esp_camera.h"
#include <WiFi.h>
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"

#define CAMERA_MODEL_AI_THINKER

#define LED   4
#define RXD2 14
#define TXD2 13

void CameraWebServer_init();

WiFiServer server(100);


extern int gpLb = 14; // Left 1
extern int gpLf = 13; // Left 2
extern int gpRb = 33; // Right 1
extern int gpRf = 15; // Right 2
extern int gpLed = 4; // Light
extern int ENR = 2;
extern int ENL = 12;

void initMotors()
{
  pinMode(gpLb, OUTPUT); //Left Backward
  pinMode(gpLf, OUTPUT); //Left Forward
  pinMode(gpRb, OUTPUT); //Right Forward
  pinMode(gpRf, OUTPUT); //Right Backward
  pinMode(gpLed, OUTPUT); //Light
  pinMode(ENR, OUTPUT);
  pinMode(ENL, OUTPUT);

  ledcSetup(2,5000,8);
  ledcSetup(12,5000,8);
  ledcAttachPin(ENR,2);
  ledcAttachPin(ENL,12);
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

  ledcSetup(7, 5000, 8);
  ledcAttachPin(gpLed, 7);  //pin4 is LED

  server.begin();

  for (int i = 0; i < 5; i++) 
  {
    ledcWrite(7, 10); // flash led
    delay(50);
    ledcWrite(7, 0);
    delay(50);
  }
}

void loop() 
{
  //delay(1000);
  //Serial.printf("RSSi: %ld dBm\n", WiFi.RSSI());
}
