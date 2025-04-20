#include <WiFiManager.h>
#include <PubSubClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <DFRobotDFPlayerMini.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>
#include <BlynkSimpleEsp32.h>
#include <time.h>

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// DFPlayer
HardwareSerial mySerial(2);  // RX = 16, TX = 17
DFRobotDFPlayerMini dfPlayer;

// MQTT
WiFiClient espClient;
PubSubClient client(espClient);
const char *mqtt_server = "18.140.66.74";

// Blynk
char auth[] = "cjqSPyXSlzm32sMLG0JOfH7ANlSvqe8M";

// LED WiFi
#define LED_WIFI 14
bool wifiConnected = false;
unsigned long lastBlink = 0;
bool ledState = false;

// Status Sender
String statusSender[6] = {"OFF", "OFF", "OFF", "OFF", "OFF", "OFF"};
unsigned long lastMessageTime[6] = {0, 0, 0, 0, 0, 0};
const unsigned long debounceDelay = 1000;
int lastCount[6] = {0};
int lastRSSI[6] = {0};

// Reset Control
bool resetRequested[3] = {false, false, false};
unsigned long lastResetTime[3] = {0, 0, 0};
const unsigned long resetCooldown = 1000; // 1 second cooldown

void setupDisplay() {
  Wire.begin(21, 22);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("OLED Gagal Inisialisasi");
    while (1);
  }
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(WHITE);
  display.setCursor(20, 0);
  display.println("WAITRESS");
  display.setCursor(40, 20);
  display.println("CALL");
  display.setCursor(34, 40);
  display.println("SYSTEM");
  display.display();
  delay(3000);
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(20, 20);
  display.println("RISKY DAVID K");
  display.setCursor(30, 35);
  display.println("2212101134");
  display.display();
  delay(2000);
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  for (int i = 0; i < 6; i++) {
    display.setCursor(0, i * 10);
    display.printf("M%d (%s): %s", (i / 2) + 1, (i % 2 == 0 ? "Call" : "Bill"), statusSender[i].c_str());
  }
  display.display();
}

void setBlynkLED(int senderIndex, bool call, bool bill) {
  int basePin = senderIndex * 3;
  Blynk.virtualWrite(V1 + basePin, call ? 1 : 0);
  Blynk.virtualWrite(V2 + basePin, bill ? 1 : 0);
  Blynk.virtualWrite(V3 + basePin, (!call && !bill) ? 1 : 0);
}

String getTimeString() {
  time_t now;
  struct tm timeinfo;
  time(&now);
  localtime_r(&now, &timeinfo);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &timeinfo);
  return String(buf);
}

void sendNodeRedReset(int senderIndex) {
  if (senderIndex < 0 || senderIndex > 2) return;

  const char* senderIds[] = {"ESP32_Sender1", "ESP32_Sender2", "ESP32_Sender3"};
  
  DynamicJsonDocument doc(256);
  doc["id"] = "ESP32_Receiver";
  doc["type"] = "all";
  doc["status"] = false;
  doc["count"] = 0;
  doc["rssi"] = 0;
  doc["timestamp"] = getTimeString();
  doc["target"] = senderIds[senderIndex]; // Target specific sender

  String payload;
  serializeJson(doc, payload);
  
  client.publish("waitress/reset", payload.c_str());
  Serial.printf("[RESET] Payload untuk %s: %s\n", senderIds[senderIndex], payload.c_str());
}

void performReset(int senderIndex) {
  if (millis() - lastResetTime[senderIndex] < resetCooldown) return;
  
  lastResetTime[senderIndex] = millis();
  
  // Reset local status (both call and bill)
  int callIdx = senderIndex * 2;
  int billIdx = senderIndex * 2 + 1;
  
  statusSender[callIdx] = "OFF";
  statusSender[billIdx] = "OFF";
  lastMessageTime[callIdx] = millis();
  lastMessageTime[billIdx] = millis();

  // Update display and Blynk
  setBlynkLED(senderIndex, false, false);
  updateDisplay();

  // Send Node-RED compatible reset command
  sendNodeRedReset(senderIndex);
  
  Serial.printf("Sender %d di-reset (call & bill)\n", senderIndex + 1);
}

// Blynk button handlers - modified
BLYNK_WRITE(V10) { 
  resetRequested[0] = true; // Meja 1
}
BLYNK_WRITE(V11) { 
  resetRequested[1] = true; // Meja 2
}
BLYNK_WRITE(V12) { 
  resetRequested[2] = true; // Meja 3
}

void processResetRequests() {
  for (int i = 0; i < 3; i++) {
    if (resetRequested[i]) {
      performReset(i);
      resetRequested[i] = false;
    }
  }
}

void callback(char* topic, byte* payload, unsigned int length) {
  if (String(topic) == "waitress/reset") {
    // Handle targeted reset commands
    StaticJsonDocument<256> doc;
    DeserializationError error = deserializeJson(doc, payload, length);
    
    if (!error && doc.containsKey("target")) {
      String target = doc["target"];
      int senderIndex = -1;
      
      if (target == "ESP32_Sender1") senderIndex = 0;
      else if (target == "ESP32_Sender2") senderIndex = 1;
      else if (target == "ESP32_Sender3") senderIndex = 2;
      
      if (senderIndex != -1) {
        performReset(senderIndex);
      }
    }
    return;
  }

  // Original message handling
  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error) {
    Serial.print("[ERROR] JSON: ");
    Serial.println(error.c_str());
    return;
  }

  String senderId = doc["id"];
  String type = doc["type"];
  bool status = doc["status"];
  int count = doc["count"] | 0;
  int rssi = doc["rssi"] | 0;
  unsigned long currentMillis = millis();

  int senderIndex = -1;
  if (senderId == "ESP32_Sender1") senderIndex = 0;
  else if (senderId == "ESP32_Sender2") senderIndex = 1;
  else if (senderId == "ESP32_Sender3") senderIndex = 2;
  if (senderIndex == -1) return;

  int index = (type == "call") ? senderIndex * 2 : senderIndex * 2 + 1;
  if (index < 0 || index >= 6) return;

  if (currentMillis - lastMessageTime[index] > debounceDelay) {
    statusSender[index] = status ? "ON" : "OFF";
    if (status) {
      if (type == "call") statusSender[senderIndex * 2 + 1] = "OFF";
      else if (type == "bill") statusSender[senderIndex * 2] = "OFF";
    }

    lastCount[index] = count;
    lastRSSI[index] = rssi;

    updateDisplay();
    bool callOn = statusSender[senderIndex * 2] == "ON";
    bool billOn = statusSender[senderIndex * 2 + 1] == "ON";
    setBlynkLED(senderIndex, callOn, billOn);

    String timeString = getTimeString();
    if (senderIndex == 0) Blynk.virtualWrite(V13, timeString);
    if (senderIndex == 1) Blynk.virtualWrite(V14, timeString);
    if (senderIndex == 2) Blynk.virtualWrite(V15, timeString);

    String timestampTopic = "node-red/timestamp/" + senderId;
    client.publish(timestampTopic.c_str(), timeString.c_str());

    if (status) dfPlayer.play(index + 1);
    lastMessageTime[index] = currentMillis;
  }
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("[MQTT] Menghubungkan...");
    if (client.connect("ESP32_Receiver")) {
      Serial.println("Tersambung!");
      client.subscribe("waitress/+/call");
      client.subscribe("waitress/+/bill");
      client.subscribe("waitress/reset");
    } else {
      Serial.print("Gagal, rc=");
      Serial.print(client.state());
      Serial.println(" coba lagi 5 detik...");
      delay(5000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(LED_WIFI, OUTPUT);
  digitalWrite(LED_WIFI, HIGH);

  mySerial.begin(9600, SERIAL_8N1, 16, 17);
  if (!dfPlayer.begin(mySerial)) {
    Serial.println("[DFPlayer] Gagal inisialisasi!");
    while (1);
  }
  dfPlayer.volume(30);

  setupDisplay();
  configTime(25200, 0, "pool.ntp.org");

  WiFiManager wm;
  wm.setConfigPortalTimeout(60);
  if (!wm.autoConnect("WaitressReceiver")) {
    Serial.println("Gagal connect WiFi. Restart...");
    ESP.restart();
  }

  Blynk.begin(auth, WiFi.SSID().c_str(), WiFi.psk().c_str());
  client.setServer(mqtt_server, 1883);
  client.setCallback(callback);
}

void loop() {
  Blynk.run();
  processResetRequests();
  
  wifiConnected = WiFi.status() == WL_CONNECTED;
  if (wifiConnected) {
    if (millis() - lastBlink >= 500) {
      lastBlink = millis();
      ledState = !ledState;
      digitalWrite(LED_WIFI, ledState);
    }
    if (!client.connected()) reconnect();
    client.loop();
  }
}
