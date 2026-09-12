#include <Arduino.h>
#include "secret.h"

// กำหนดขาเชื่อมต่อระหว่าง ESP32 และโมดูล A7680C
#define MODEM_TX_PIN      17
#define MODEM_RX_PIN      16
#define MODEM_PWRKEY_PIN  4
#define MODEM_BAUDRATE    115200
#define LED_BLUE_PIN      2     // Built-in blue LED บน ESP32

HardwareSerial SerialAT(2);

// ฟังก์ชันส่งคำสั่ง AT และรอรับผลการตอบกลับ
String sendAT(const String &cmd, uint32_t timeoutMs = 3000) {
  while (SerialAT.available()) {
    SerialAT.read();
  }

  Serial.print("[SEND] >> ");
  Serial.println(cmd);
  SerialAT.println(cmd);

  String resp = "";
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      char c = SerialAT.read();
      resp += c;
      Serial.write(c);
      start = millis();
    }
  }
  return resp;
}

// ฟังก์ชันส่งข้อมูลดิบ (เช่น หลังเครื่องหมาย '>')
void sendRaw(const String &data, uint32_t timeoutMs = 3000) {
  Serial.print("[DATA] >> ");
  Serial.println(data);
  SerialAT.print(data);

  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      Serial.write(SerialAT.read());
      start = millis();
    }
  }
}

// ฟังก์ชันเปิดโมดูลผ่านขา PWRKEY
void powerOnModem() {
  if (MODEM_PWRKEY_PIN >= 0) {
    pinMode(MODEM_PWRKEY_PIN, OUTPUT);
    digitalWrite(MODEM_PWRKEY_PIN, HIGH);
    delay(100);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
    delay(1500);
    digitalWrite(MODEM_PWRKEY_PIN, HIGH);
    delay(3000);
  }
}

// ตรวจสอบว่าโมดูลติดเครือข่ายมือถืออยู่ไหม
// +CREG: 0,1 = ติด  |  +CREG: 0,5 = roaming
bool isNetworkConnected() {
  String res = sendAT("AT+CREG?", 2000);
  return res.indexOf(",1") != -1 || res.indexOf(",5") != -1;
}

// ตรวจสอบว่า MQTT ยังเชื่อมต่อ Broker อยู่ไหม
// +CMQTTCONNECT: 0,1 = connected
bool isMQTTConnected() {
  String res = sendAT("AT+CMQTTCONNECT?", 2000);
  return res.indexOf("0,1") != -1;
}

// ฟังก์ชันตั้งค่า SSL และเชื่อมต่อไปยัง HiveMQ Cloud Broker
bool connectMQTT() {
  Serial.println("\n[MQTT] กำลังตั้งค่าและเชื่อมต่อ HiveMQ Cloud...");

  sendAT("AT+NETOPEN", 5000);

  // 1. ตั้งค่า SSL Context 0 สำหรับ HiveMQ Cloud
  sendAT("AT+CSSLCFG=\"sslversion\",0,4", 2000);
  sendAT("AT+CSSLCFG=\"authmode\",0,0", 2000);
  sendAT("AT+CSSLCFG=\"enableSNI\",0,1", 2000);  // จำเป็นมากสำหรับ HiveMQ Cloud
  sendAT("AT+CSSLCFG=\"ignorelocaltime\",0,1", 2000);

  // 2. เริ่มต้นระบบ MQTT
  sendAT("AT+CMQTTSTART", 3000);

  // 3. จอง Client ID (parameter ตัวที่สามคือ 1 = SSL)
  String accqCmd = "AT+CMQTTACCQ=0,\"" + String(MQTT_CLIENT_ID) + "\",1";
  sendAT(accqCmd, 2000);

  // 4. เชื่อม SSL Context 0 เข้ากับ MQTT Client 0
  sendAT("AT+CMQTTSSLCFG=0,0", 2000);

  // 5. เชื่อมต่อไปยัง HiveMQ Broker
  String connCmd = "AT+CMQTTCONNECT=0,\"tcp://" + String(MQTT_URL) + ":" + String(MQTT_PORT) +
                   "\",60,1,\"" + String(MQTT_USERNAME) + "\",\"" + String(MQTT_PASSWORD) + "\"";
  String res = sendAT(connCmd, 15000);

  if (res.indexOf("+CMQTTCONNECT: 0,0") != -1 || res.indexOf("OK") != -1) {
    Serial.println("[MQTT] เชื่อมต่อสำเร็จ!");
    return true;
  }

  Serial.println("[MQTT] เชื่อมต่อล้มเหลว!");
  return false;
}
bool getGPS(float &lat, float &lng) {
  String res = sendAT("AT+CGNSSINFO", 3000);

  // หาตำแหน่ง header
  int idx = res.indexOf("+CGNSSINFO:");
  if (idx == -1) {
    Serial.println("[GPS] ไม่มี response");
    return false;
  }

  // ข้าม "+CGNSSINFO:" แล้วตัด whitespace
  String data = res.substring(idx + 11);
  data.trim();

  // แยก fields ด้วย comma
  // จาก Log รูปแบบคือ: 3,12,08,01,,14.0251942,N,99.9747162,E,...
  // [0]=mode [1]=gps_sat [2]=glo_sat [3]=bd_sat [4]=gal_sat (มักจะว่าง)
  // [5]=lat  [6]=N/S     [7]=lng      [8]=E/W ...
  String fields[12];
  int fieldCount = 0;
  int start = 0;
  for (int i = 0; i <= (int)data.length() && fieldCount < 12; i++) {
    if (i == (int)data.length() || data[i] == ',') {
      String f = data.substring(start, i);
      f.trim();
      fields[fieldCount++] = f;
      start = i + 1;
    }
  }

  if (fieldCount < 9) {
    Serial.println("[GPS] parse ล้มเหลว — fields ไม่ครบ");
    return false;
  }

  // ตรวจ mode — mode != 0 คือ lock ได้
  int mode = fields[0].toInt();
  if (mode == 0) {
    Serial.println("[GPS] ยังไม่ได้ lock สัญญาณ...");
    return false;
  }

  // ตำแหน่งจริงของ lat/lng คือ index 5 และ 7
  if (fields[5].length() == 0 || fields[7].length() == 0) {
    Serial.println("[GPS] lat/lng ว่างเปล่า");
    return false;
  }

  // ข้อมูลที่ส่งมาเป็น Decimal Degrees (DD.DDDDDD) อยู่แล้ว ไม่ต้องหาร 60
  lat = fields[5].toFloat();
  lng = fields[7].toFloat();

  if (fields[6] == "S") lat = -lat;
  if (fields[8] == "W") lng = -lng;

  Serial.printf("[GPS] mode=%d, lat=%.5f, lng=%.5f\n", mode, lat, lng);
  return true;
}

// ฟังก์ชัน Publish ข้อมูลพิกัดและสถานะในรูปแบบ JSON
bool publishLocation(float lat, float lng, const char *status) {
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"lat\":%.5f,\"lng\":%.5f,\"status\":\"%s\"}", lat, lng, status);

  String topic      = String(MQTT_TOPIC);
  String payloadStr = String(payload);

  Serial.println("\n[MQTT] กำลัง Publish ข้อมูล...");
  Serial.print("[MQTT] Payload: ");
  Serial.println(payloadStr);

  // 1. ส่งคำสั่งระบุความยาว Topic แล้วตามด้วยข้อความ Topic
  String topicCmd = "AT+CMQTTTOPIC=0," + String(topic.length());
  sendAT(topicCmd, 2000);
  sendRaw(topic, 2000);

  // 2. ส่งคำสั่งระบุความยาว Payload แล้วตามด้วยข้อความ Payload
  String payloadCmd = "AT+CMQTTPAYLOAD=0," + String(payloadStr.length());
  sendAT(payloadCmd, 2000);
  sendRaw(payloadStr, 2000);

  // 3. สั่ง Publish ข้อมูล (QoS 1, timeout 60 วินาที)
  String pubRes = sendAT("AT+CMQTTPUB=0,1,60", 5000);
  return pubRes.indexOf("OK") != -1;
}

void setup() {
  Serial.begin(115200);
  SerialAT.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  // ตั้งค่าขา LED
  pinMode(LED_BLUE_PIN, OUTPUT);
  digitalWrite(LED_BLUE_PIN, LOW);  // เริ่มต้นดับไฟ

  Serial.println("\n========================================");
  Serial.println("   A7680C GPS + MQTT Test");
  Serial.println("========================================");

  powerOnModem();
  delay(3000);

  sendAT("ATE1", 1000);
  sendAT("AT+CPIN?", 2000);
  sendAT("AT+CREG?", 2000);

  connectMQTT();

  // เปิด GPS
  sendAT("AT+CGNSSPWR=1", 3000);
  Serial.println("[GPS] เปิด GPS แล้ว รอ lock สัญญาณ...");
}

void loop() {
  static uint32_t lastPub   = 0;
  static uint32_t lastCheck = 0;

  // ── ตรวจสอบการเชื่อมต่อทุก 10 วินาที ──────────────────
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();

    if (!isNetworkConnected()) {
      Serial.println("[NET] หลุดเครือข่าย! รอสัญญาณกลับมา...");
    } else if (!isMQTTConnected()) {
      Serial.println("[MQTT] หลุด Broker! กำลัง reconnect...");
      connectMQTT();
    } else {
      Serial.println("[OK] Network และ MQTT พร้อม");
    }
  }

  // ── อ่าน GPS และ Publish ทุก 3 วินาที ──────────────────
  if (millis() - lastPub > 3000) {
    lastPub = millis();

    if (!isMQTTConnected()) {
      Serial.println("[MQTT] ยังไม่ได้ connect, ข้าม publish รอบนี้");
    } else {
      float lat, lng;
      if (getGPS(lat, lng)) {
        digitalWrite(LED_BLUE_PIN, HIGH);  // lock ได้ → เปิดไฟสีฟ้า
        bool ok = publishLocation(lat, lng, "active");
        Serial.println(ok ? "[MQTT] Publish สำเร็จ" : "[MQTT] Publish ล้มเหลว");
      } else {
        digitalWrite(LED_BLUE_PIN, LOW);   // lock ไม่ได้ → ดับไฟ
      }
    }
  }

  // ── Mirror ข้อความ URC จากโมดูลออก Serial Monitor ─────
  while (SerialAT.available()) {
    Serial.write(SerialAT.read());
  }
}
