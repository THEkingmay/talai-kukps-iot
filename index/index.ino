#include <Arduino.h>
#include "secret.h"

// กำหนดขาเชื่อมต่อระหว่าง ESP32 และโมดูล A7680C
#define MODEM_TX_PIN      17
#define MODEM_RX_PIN      16
#define MODEM_PWRKEY_PIN  4
#define MODEM_BAUDRATE    115200
#define LED_BLUE_PIN      2

HardwareSerial SerialAT(2);

bool mqttReady = false;  // สถานะ MQTT ที่เราจัดการเอง

// ───────────────────────── AT helpers ─────────────────────────

// ส่งคำสั่ง AT แล้วรอคำตอบ
// - until: ถ้าระบุ จะหยุดรอทันทีเมื่อเจอข้อความนี้ (เช่น "+CMQTTCONNECT: 0,0")
// - ถ้าเจอ "ERROR" ก็หยุดทันทีเช่นกัน (ไม่ต้องรอ timeout)
// - ถ้าไม่ระบุ until จะหยุดเมื่อเจอ "OK"
String sendAT(const String &cmd, uint32_t timeoutMs = 3000, const char *until = nullptr) {
  while (SerialAT.available()) SerialAT.read();

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
    }
    if (until) {
      if (resp.indexOf(until) != -1) break;
    } else {
      if (resp.indexOf("OK\r\n") != -1) break;
    }
    if (resp.indexOf("ERROR") != -1) break;
    delay(5);
  }
  return resp;
}

// ส่งข้อมูลดิบ (topic / payload) หลังโมเด็มตอบ ">"
void sendRaw(const String &data, uint32_t timeoutMs = 3000) {
  Serial.print("[DATA] >> ");
  Serial.println(data);
  SerialAT.print(data);

  String resp = "";
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      char c = SerialAT.read();
      resp += c;
      Serial.write(c);
    }
    if (resp.indexOf("OK\r\n") != -1 || resp.indexOf("ERROR") != -1) break;
    delay(5);
  }
}

// ───────────────────────── Modem ─────────────────────────

bool modemAlive() {
  for (int i = 0; i < 3; i++) {
    String r = sendAT("AT", 1000);
    if (r.indexOf("OK") != -1) return true;
  }
  return false;
}

void powerOnModem() {
  // ถ้าโมเด็มเปิดอยู่แล้ว ห้ามกด PWRKEY ซ้ำ (จะกลายเป็นสั่งปิด)
  if (modemAlive()) {
    Serial.println("[MODEM] เปิดอยู่แล้ว ข้ามการ power on");
    return;
  }

  if (MODEM_PWRKEY_PIN >= 0) {
    pinMode(MODEM_PWRKEY_PIN, OUTPUT);
    digitalWrite(MODEM_PWRKEY_PIN, HIGH);
    delay(100);
    digitalWrite(MODEM_PWRKEY_PIN, LOW);
    delay(1500);
    digitalWrite(MODEM_PWRKEY_PIN, HIGH);
    delay(3000);
  }

  // รอจนโมเด็มตอบ AT
  uint32_t start = millis();
  while (millis() - start < 15000) {
    if (modemAlive()) return;
    delay(500);
  }
  Serial.println("[MODEM] ไม่ตอบสนองหลัง power on");
}

bool isNetworkConnected() {
  String res = sendAT("AT+CREG?", 2000);
  return res.indexOf(",1") != -1 || res.indexOf(",5") != -1;
}

// ───────────────────────── MQTT ─────────────────────────

// เคลียร์สถานะ MQTT ทั้งหมดให้เริ่มใหม่ได้สะอาด
void cleanupMQTT() {
  sendAT("AT+CMQTTDISC=0,60", 3000);  // ไม่เชื่อมอยู่จะ ERROR ได้ ไม่เป็นไร
  sendAT("AT+CMQTTREL=0", 2000);
  sendAT("AT+CMQTTSTOP", 3000);
  mqttReady = false;
}

bool isMQTTConnected() {
  if (!mqttReady) return false;

  // ถ้าเชื่อมอยู่ โมเด็มจะตอบ +CMQTTCONNECT: 0,"tcp://host:port",...
  String res = sendAT("AT+CMQTTCONNECT?", 2000);
  bool connected = res.indexOf("+CMQTTCONNECT: 0,\"") != -1;
  if (!connected) mqttReady = false;
  return connected;
}

bool connectMQTT() {
  Serial.println("\n[MQTT] กำลังตั้งค่าและเชื่อมต่อ EMQX Cloud...");

  // เคลียร์ของเก่าก่อนเริ่มทุกครั้ง
  cleanupMQTT();

  // เปิด network (ถ้าเปิดอยู่แล้วจะตอบ ERROR ได้ ไม่เป็นไร)
  sendAT("AT+NETOPEN", 8000, "+NETOPEN:");

  // 1. ตั้งค่า SSL Context 0
  // sslversion: 3 = TLS1.2
  sendAT("AT+CSSLCFG=\"sslversion\",0,3", 2000);
  sendAT("AT+CSSLCFG=\"authmode\",0,0", 2000);        // ไม่ตรวจ certificate
  sendAT("AT+CSSLCFG=\"enableSNI\",0,1", 2000);       // จำเป็นสำหรับ EMQX Cloud
  sendAT("AT+CSSLCFG=\"ignorelocaltime\",0,1", 2000);
  // (ลบ ciphersuite ออก เพราะ firmware นี้ไม่รองรับ และตอบ ERROR)

  // 2. เริ่มระบบ MQTT
  String r = sendAT("AT+CMQTTSTART", 5000, "+CMQTTSTART:");
  if (r.indexOf("+CMQTTSTART: 0") == -1) {
    Serial.println("[MQTT] CMQTTSTART ล้มเหลว");
    cleanupMQTT();
    return false;
  }

  // 3. จอง Client ID (พารามิเตอร์ตัวที่ 3 = 1 → ใช้ SSL)
  String accqCmd = "AT+CMQTTACCQ=0,\"" + String(MQTT_CLIENT_ID) + "\",1";
  r = sendAT(accqCmd, 3000);
  if (r.indexOf("OK") == -1) {
    Serial.println("[MQTT] CMQTTACCQ ล้มเหลว");
    cleanupMQTT();
    return false;
  }

  // 4. เชื่อม SSL Context 0 เข้ากับ MQTT Client 0
  r = sendAT("AT+CMQTTSSLCFG=0,0", 2000);
  if (r.indexOf("OK") == -1) {
    Serial.println("[MQTT] CMQTTSSLCFG ล้มเหลว");
    cleanupMQTT();
    return false;
  }

  // 5. เชื่อมต่อ Broker — ต้องมี tcp:// นำหน้า (SSL ถูกกำหนดจาก ACCQ/SSLCFG แล้ว)
  String connCmd = "AT+CMQTTCONNECT=0,\"tcp://" + String(MQTT_URL) + ":" + String(MQTT_PORT) +
                   "\",60,1,\"" + String(MQTT_USERNAME) + "\",\"" + String(MQTT_PASSWORD) + "\"";
  String res = sendAT(connCmd, 30000, "+CMQTTCONNECT: 0,");

  if (res.indexOf("+CMQTTCONNECT: 0,0") != -1) {
    Serial.println("[MQTT] เชื่อมต่อสำเร็จ!");
    mqttReady = true;
    return true;
  }

  Serial.println("[MQTT] เชื่อมต่อล้มเหลว! (ดูเลข error ใน +CMQTTCONNECT: 0,<เลข>)");
  cleanupMQTT();
  return false;
}

// ───────────────────────── GPS ─────────────────────────

bool getGPS(float &lat, float &lng) {
  String res = sendAT("AT+CGNSSINFO", 3000);

  int idx = res.indexOf("+CGNSSINFO:");
  if (idx == -1) {
    Serial.println("[GPS] ไม่มี response");
    return false;
  }

  String data = res.substring(idx + 11);
  data.trim();

  // รูปแบบ: mode,gps_sat,glo_sat,bd_sat,gal_sat,lat,N/S,lng,E/W,...
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

  int mode = fields[0].toInt();
  if (mode == 0) {
    Serial.println("[GPS] ยังไม่ได้ lock สัญญาณ...");
    return false;
  }

  if (fields[5].length() == 0 || fields[7].length() == 0) {
    Serial.println("[GPS] lat/lng ว่างเปล่า");
    return false;
  }

  // เป็น Decimal Degrees อยู่แล้ว ไม่ต้องหาร 60
  lat = fields[5].toFloat();
  lng = fields[7].toFloat();

  if (fields[6] == "S") lat = -lat;
  if (fields[8] == "W") lng = -lng;

  Serial.printf("[GPS] mode=%d, lat=%.5f, lng=%.5f\n", mode, lat, lng);
  return true;
}

// ───────────────────────── Publish ─────────────────────────

bool publishLocation(float lat, float lng, const char *status) {
  char payload[128];
  snprintf(payload, sizeof(payload),
           "{\"lat\":%.5f,\"lng\":%.5f,\"status\":\"%s\"}", lat, lng, status);

  String topic      = String(MQTT_TOPIC);
  String payloadStr = String(payload);

  Serial.println("\n[MQTT] กำลัง Publish ข้อมูล...");
  Serial.print("[MQTT] Payload: ");
  Serial.println(payloadStr);

  // 1. Topic
  String r = sendAT("AT+CMQTTTOPIC=0," + String(topic.length()), 2000, ">");
  if (r.indexOf(">") == -1) return false;
  sendRaw(topic, 2000);

  // 2. Payload
  r = sendAT("AT+CMQTTPAYLOAD=0," + String(payloadStr.length()), 2000, ">");
  if (r.indexOf(">") == -1) return false;
  sendRaw(payloadStr, 2000);

  // 3. Publish (QoS 1, timeout 60 วินาที) — รอ URC +CMQTTPUB: 0,0
  String pubRes = sendAT("AT+CMQTTPUB=0,1,60", 10000, "+CMQTTPUB:");
  bool ok = pubRes.indexOf("+CMQTTPUB: 0,0") != -1;
  if (!ok) mqttReady = false;  // ให้ loop ตรวจและ reconnect
  return ok;
}

// ───────────────────────── Arduino ─────────────────────────

void setup() {
  Serial.begin(115200);
  SerialAT.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  pinMode(LED_BLUE_PIN, OUTPUT);
  digitalWrite(LED_BLUE_PIN, LOW);

  Serial.println("\n========================================");
  Serial.println("   A7680C GPS + MQTT Test (EMQX Cloud)");
  Serial.println("========================================");

  powerOnModem();

  sendAT("ATE1", 1000);
  sendAT("AT+CPIN?", 2000);

  // รอจนลงทะเบียนเครือข่ายได้ (สูงสุด 60 วินาที)
  uint32_t start = millis();
  while (!isNetworkConnected() && millis() - start < 60000) {
    Serial.println("[NET] รอลงทะเบียนเครือข่าย...");
    delay(2000);
  }

  // เปิด GPS ก่อน เพื่อให้เริ่มค้นหาดาวเทียมระหว่างต่อ MQTT
  sendAT("AT+CGNSSPWR=1", 3000);
  Serial.println("[GPS] เปิด GPS แล้ว รอ lock สัญญาณ...");

  connectMQTT();
}

void loop() {
  static uint32_t lastPub   = 0;
  static uint32_t lastCheck = 0;

  // ── ตรวจสอบการเชื่อมต่อทุก 10 วินาที ──
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();

    if (!isNetworkConnected()) {
      Serial.println("[NET] หลุดเครือข่าย! รอสัญญาณกลับมา...");
      mqttReady = false;
    } else if (!isMQTTConnected()) {
      Serial.println("[MQTT] หลุด Broker! กำลัง reconnect...");
      connectMQTT();
    } else {
      Serial.println("[OK] Network และ MQTT พร้อม");
    }
  }

  // ── อ่าน GPS และ Publish ทุก 3 วินาที ──
  if (millis() - lastPub > 3000) {
    lastPub = millis();

    if (!mqttReady) {
      Serial.println("[MQTT] ยังไม่ได้ connect, ข้าม publish รอบนี้");
    } else {
      float lat, lng;
      if (getGPS(lat, lng)) {
        digitalWrite(LED_BLUE_PIN, HIGH);
        bool ok = publishLocation(lat, lng, "active");
        Serial.println(ok ? "[MQTT] Publish สำเร็จ" : "[MQTT] Publish ล้มเหลว");
      } else {
        digitalWrite(LED_BLUE_PIN, LOW);
      }
    }
  }

  // แสดง URC ที่โมเด็มส่งมาเอง เช่น +CMQTTCONNLOST
  String urc = "";
  while (SerialAT.available()) {
    char c = SerialAT.read();
    urc += c;
    Serial.write(c);
  }
  if (urc.indexOf("+CMQTTCONNLOST") != -1 || urc.indexOf("+CMQTTNONET") != -1) {
    mqttReady = false;
  }
}