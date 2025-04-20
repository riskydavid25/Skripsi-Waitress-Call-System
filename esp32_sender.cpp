#include <WiFi.h>
#include <PubSubClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <time.h>

// MQTT config
const char* mqtt_server = "18.140.66.74"; // Ganti dengan IP broker MQTT Anda
const int mqtt_port = 1883;
const char* mqtt_client_id = "ESP32_Sender3";
const char* senderID = "ESP32_Sender3";

// Pin assignments
const int callButton = 33;
const int billButton = 25;
const int resetButton = 26;
const int greenLed = 12;
const int blueLed = 13;
const int wifiLed = 14;

int lastCallState = HIGH;
int lastBillState = HIGH;
int lastResetState = HIGH;

int callCount = 0;
int billCount = 0;

WiFiClient espClient;
PubSubClient client(espClient);
WiFiManager wifiManager;

void setup() {
  Serial.begin(115200);

  pinMode(callButton, INPUT_PULLUP);
  pinMode(billButton, INPUT_PULLUP);
  pinMode(resetButton, INPUT_PULLUP);
  pinMode(greenLed, OUTPUT);
  pinMode(blueLed, OUTPUT);
  pinMode(wifiLed, OUTPUT);

  digitalWrite(greenLed, LOW);
  digitalWrite(blueLed, LOW);
  digitalWrite(wifiLed, HIGH);

  setup_wifi();
  client.setServer(mqtt_server, mqtt_port);
  client.setCallback(mqttCallback);

  configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  while (time(nullptr) < 100000) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("\n🕒 Time synchronized!");

  resetSystem();  // Reset state on boot
}

void setup_wifi() {
  wifiManager.setTimeout(180);
  if (!wifiManager.autoConnect("Sender3_AP")) {
    Serial.println("❌ Failed to connect. Restarting...");
    ESP.restart();
  }
  Serial.println("✅ WiFi connected: " + WiFi.localIP().toString());
}

void reconnect() {
  while (!client.connected()) {
    Serial.print("🔄 MQTT reconnect...");
    if (client.connect(mqtt_client_id)) {
      Serial.println("connected");
      client.subscribe("waitress/reset");
    } else {
      Serial.print(" failed, rc=");
      Serial.println(client.state());
      delay(5000);
    }
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.printf("📥 Received on %s: ", topic);
  String message;
  for (unsigned int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  Serial.println(message);

  StaticJsonDocument<256> doc;
  DeserializationError error = deserializeJson(doc, message);
  if (error) {
    Serial.println("❌ JSON Parsing Failed");
    return;
  }

  const char* fromID = doc["id"] | "unknown";
  const char* type = doc["type"];
  const char* target = doc["target"] | "";  // Tidak default ke "all", supaya hanya reset yang benar-benar tertuju
  bool status = doc["status"];

  // Hanya respon jika target sesuai dengan senderID
  if ((String(fromID) == "ESP32_Receiver" || String(fromID) == "NodeRED") && status == false) {
    if (String(target) == senderID && 
        (String(type) == "call" || String(type) == "bill" || String(type) == "all")) {
      Serial.printf("🔁 Reset command received for me (%s)\n", senderID);
      resetSystem();
    } else {
      Serial.printf("⚠️ Reset ignored — target '%s' does not match '%s'\n", target, senderID);
    }
  }
}


void sendMessage(const char* type, bool status, int count, const char* timestamp) {
  int rssi = WiFi.RSSI();
  String topic = "waitress/" + String(senderID) + "/" + String(type);

  String payload = "{";
  payload += "\"id\":\"" + String(senderID) + "\",";  // ID sender
  payload += "\"type\":\"" + String(type) + "\",";  // Tipe (call atau bill)
  payload += "\"status\":" + String(status ? "true" : "false") + ",";  // Status
  payload += "\"count\":" + String(count) + ",";  // Jumlah panggilan/bill
  payload += "\"rssi\":" + String(rssi) + ",";  // RSSI Wi-Fi
  payload += "\"timestamp\":\"" + String(timestamp) + "\"";  // Timestamp
  payload += "}";

  client.publish(topic.c_str(), payload.c_str(), true);
  Serial.printf("📤 Sent to %s: %s\n", topic.c_str(), payload.c_str());
}

void resetSystem() {
  digitalWrite(greenLed, LOW);
  digitalWrite(blueLed, LOW);

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char timeString[40];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);

  sendMessage("call", false, callCount, timeString);
  sendMessage("bill", false, billCount, timeString);
  Serial.println("🔄 System reset");
}

void loop() {
  static unsigned long lastBlinkTime = 0;

  if (WiFi.status() == WL_CONNECTED) {
    if (millis() - lastBlinkTime > 500) {
      digitalWrite(wifiLed, !digitalRead(wifiLed));
      lastBlinkTime = millis();
    }
  } else {
    digitalWrite(wifiLed, HIGH);
  }

  if (!client.connected()) {
    reconnect();
  }
  client.loop();

  int callState = digitalRead(callButton);
  int billState = digitalRead(billButton);
  int resetState = digitalRead(resetButton);

  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  char timeString[40];
  strftime(timeString, sizeof(timeString), "%Y-%m-%d %H:%M:%S", &timeinfo);

  // Deteksi tombol call
  if (callState == LOW && lastCallState == HIGH) {
    callCount++;
    digitalWrite(greenLed, HIGH);
    digitalWrite(blueLed, LOW);
    sendMessage("call", true, callCount, timeString);
    sendMessage("bill", false, billCount, timeString);
    delay(200);
  }

  // Deteksi tombol bill
  if (billState == LOW && lastBillState == HIGH) {
    billCount++;
    digitalWrite(greenLed, LOW);
    digitalWrite(blueLed, HIGH);
    sendMessage("call", false, callCount, timeString);
    sendMessage("bill", true, billCount, timeString);
    delay(200);
  }

  // Deteksi tombol reset
  if (resetState == LOW && lastResetState == HIGH) {
    resetSystem();
    delay(200);
  }

  lastCallState = callState;
  lastBillState = billState;
  lastResetState = resetState;
}
