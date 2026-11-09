#include <Arduino.h>
#include "secret.h"

// กำหนดขาเชื่อมต่อระหว่าง ESP32 และโมดูล A7680C
#define MODEM_TX_PIN      17
#define MODEM_RX_PIN      16
#define MODEM_PWRKEY_PIN  4
#define MODEM_BAUDRATE    115200
#define LED_BLUE_PIN      2

// ── ตัวเลือก (ตั้งใน secret.h หรือ build_flags ได้) ──
// MODEM_APN: ถ้าระบุ จะตั้ง APN ให้ก่อนเปิดเครือข่าย (เช่น "internet")
//            ถ้าไม่ระบุ จะไม่ยุ่งกับ APN เหมือนเดิม
#ifndef MODEM_APN
#define MODEM_APN ""
#endif
// PWRKEY_RELEASE_HIZ: 1 = ปล่อยขา PWRKEY เป็น hi-Z หลังกด (เหมาะกับต่อตรงเข้า PWRKEY active-low)
//                     0 = ดันขาเป็น HIGH ค้างไว้ (พฤติกรรมเดิม)
#ifndef PWRKEY_RELEASE_HIZ
#define PWRKEY_RELEASE_HIZ 0
#endif

HardwareSerial SerialAT(2);

bool mqttReady = false;  // สถานะ MQTT ที่เราจัดการเอง

// ── สถานะสำหรับตรวจจับว่าโมดูลรีบูตเอง (ไฟตก/รีเซ็ต) ──
String  urcBuf        = "";     // เก็บ URC ที่โมดูลส่งมาเอง
uint8_t gpsErrorCount = 0;      // นับ AT+CGNSSINFO ที่ตอบ ERROR ติดกัน

// ───────────────────────── AT helpers ─────────────────────────

// อ่าน byte ที่โมดูลส่งมา: echo ขึ้น Serial + เก็บลง urcBuf กัน URC หาย
static void noteByte(char c, String *resp) {
  if (resp) *resp += c;
  urcBuf += c;
  Serial.write(c);
  if (urcBuf.length() > 1024) urcBuf.remove(0, urcBuf.length() - 1024);
}

static void pumpURC() {
  while (SerialAT.available()) noteByte(SerialAT.read(), nullptr);
}

// ส่งคำสั่ง AT แล้วรอคำตอบ
// - until: ถ้าระบุ จะหยุดรอทันทีเมื่อเจอข้อความนี้ (เช่น "+CMQTTCONNECT: 0,0")
// - ถ้าเจอ "ERROR" ก็หยุดทันทีเช่นกัน (ไม่ต้องรอ timeout)
// - ถ้าไม่ระบุ until จะหยุดเมื่อเจอ "OK"
String sendAT(const String &cmd, uint32_t timeoutMs = 3000, const char *until = nullptr) {
  pumpURC();  // เก็บ URC ที่ค้างอยู่แทนการทิ้ง (สำคัญ: จะได้รู้ว่าโมดูลรีบูต)

  Serial.print("[SEND] >> ");
  Serial.println(cmd);
  SerialAT.println(cmd);

  String resp = "";
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {
    while (SerialAT.available()) {
      noteByte(SerialAT.read(), &resp);
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
      noteByte(SerialAT.read(), &resp);
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

// ปล่อยขา PWRKEY หลังกดเสร็จ
static void pwrkeyRelease() {
#if PWRKEY_RELEASE_HIZ
  pinMode(MODEM_PWRKEY_PIN, INPUT);
#else
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
#endif
}

// กด PWRKEY 1 ครั้ง (ถ้าโมเด็มเปิดอยู่ = สั่งปิด / ถ้าปิดอยู่ = สั่งเปิด)
static void pwrkeyPulse() {
  if (MODEM_PWRKEY_PIN < 0) return;
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(100);
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(1500);
  pwrkeyRelease();
  delay(3000);
}

void powerOnModem() {
  // ถ้าโมเด็มเปิดอยู่แล้ว ห้ามกด PWRKEY ซ้ำ (จะกลายเป็นสั่งปิด)
  if (modemAlive()) {
    Serial.println("[MODEM] เปิดอยู่แล้ว ข้ามการ power on");
    return;
  }

  pwrkeyPulse();

  // รอจนโมเด็มตอบ AT
  uint32_t start = millis();
  while (millis() - start < 15000) {
    if (modemAlive()) return;
    delay(500);
  }
  Serial.println("[MODEM] ไม่ตอบสนองหลัง power on");
}

// โมดูลค้าง (ไม่ตอบ AT) → กด PWRKEY รอบแรกเพื่อปิด แล้วให้ powerOnModem() กดอีกรอบเพื่อเปิด
void hardResetModem() {
  Serial.println("\n[MODEM] โมดูลไม่ตอบสนอง — รีเซ็ตด้วย PWRKEY...");
  pwrkeyPulse();   // รอบแรก = ปิด (ถ้ายังรับคำสั่งฮาร์ดแวร์ได้)
  delay(2000);
  powerOnModem();  // ถ้ายังไม่ตอบ AT จะกดอีกรอบ = เปิด
}

// อ่านค่า stat จาก +CREG: / +CEREG: (รูปแบบ "+CREG: <n>,<stat>[,...]") → 1 = home, 5 = roaming
static bool regOk(const String &r, const char *tag) {
  int i = r.indexOf(tag);
  if (i < 0) return false;
  int c = r.indexOf(',', i);
  if (c < 0) return false;
  int stat = r.substring(c + 1).toInt();
  return stat == 1 || stat == 5;
}

// A7680C เป็น LTE → เช็ก CEREG ก่อน แล้ว fallback เป็น CREG
bool isNetworkConnected() {
  String r = sendAT("AT+CEREG?", 2000);
  if (regOk(r, "+CEREG:")) return true;
  r = sendAT("AT+CREG?", 2000);
  return regOk(r, "+CREG:");
}

static void waitNetwork(uint32_t timeoutMs) {
  uint32_t start = millis();
  while (!isNetworkConnected() && millis() - start < timeoutMs) {
    Serial.println("[NET] รอลงทะเบียนเครือข่าย...");
    delay(2000);
  }
}

// ตั้ง APN (ทำเฉพาะเมื่อกำหนด MODEM_APN)
static void setupAPN() {
  if (strlen(MODEM_APN) == 0) return;
  sendAT(String("AT+CGDCONT=1,\"IP\",\"") + MODEM_APN + "\"", 2000);
}

// ───────────────────────── GPS ─────────────────────────

// เปิด GPS และรอจนโมดูลพร้อม (URC: +CGNSSPWR: READY!)
// ต้องเรียกก่อนใช้ AT+CGNSSINFO ไม่งั้น module ยังไม่ boot เสร็จจะตอบ ERROR
void enableGPS() {
  Serial.println("[GPS] เปิด GPS module...");
  String r = sendAT("AT+CGNSSPWR=1", 3000);

  if (r.indexOf("OK") == -1) {
    Serial.println("[GPS] AT+CGNSSPWR=1 ไม่สำเร็จ (อาจเปิดอยู่แล้ว)");
    return;
  }

  // READY อาจมาติดอยู่ในคำตอบเดียวกับ OK แล้ว → ไม่ต้องรอซ้ำ
  if (r.indexOf("+CGNSSPWR: READY") != -1) {
    Serial.println("[GPS] GPS module พร้อมแล้ว ✓");
    return;
  }

  // รอ URC "+CGNSSPWR: READY!" — บอกว่า GPS module boot เสร็จและพร้อมใช้งาน
  Serial.println("[GPS] รอ GPS module พร้อม (+CGNSSPWR: READY!)...");
  String urc = "";
  uint32_t start = millis();
  while (millis() - start < 15000) {
    while (SerialAT.available()) {
      noteByte(SerialAT.read(), &urc);
    }
    if (urc.indexOf("+CGNSSPWR: READY") != -1) {
      Serial.println("[GPS] GPS module พร้อมแล้ว ✓");
      return;
    }
    delay(10);
  }
  Serial.println("[GPS] หมดเวลารอ READY — จะพยายามใช้งานต่อไป");
}

// GNSS ตอบ ERROR ติดกันหลังโมดูลรีบูต → สั่งเปิด GNSS ใหม่โดยไม่ต้องถอดปลั๊ก
void gpsPowerCycle() {
  Serial.println("[GPS] GNSS ไม่ตอบ — ตรวจสถานะและเปิดใหม่...");

  String pwr = sendAT("AT+CGNSSPWR?", 2000);
  if (pwr.indexOf("+CGNSSPWR: 1") == -1) {
    // GNSS ดับอยู่ (เช่น เพิ่งรีบูต) → เปิดใหม่แล้วรอ READY
    enableGPS();
    return;
  }

  // ยังรายงานว่าเปิดอยู่แต่ใช้งานไม่ได้ (engine ค้าง) → ปิดแล้วเปิดใหม่
  Serial.println("[GPS] GNSS ค้าง — ปิด/เปิดใหม่");
  sendAT("AT+CGNSSPWR=0", 3000);
  delay(1000);
  enableGPS();
}

bool getGPS(double &lat, double &lng) {
  String res = sendAT("AT+CGNSSINFO", 3000);

  int idx = res.indexOf("+CGNSSINFO:");
  if (idx == -1) {
    // ERROR = GNSS ยังไม่ ready / ถูกปิดไปหลังโมดูลรีบูต
    gpsErrorCount++;
    Serial.printf("[GPS] ไม่มี response (+CGNSSINFO ตอบ ERROR) ติดกัน %d ครั้ง\n", (int)gpsErrorCount);
    if (gpsErrorCount >= 3) {  // 3 ครั้งติด (~9 วิ) = GNSS ดับแน่ๆ
      gpsErrorCount = 0;
      gpsPowerCycle();
    }
    return false;
  }
  gpsErrorCount = 0;

  // เอาเฉพาะบรรทัด +CGNSSINFO (ตัด "\r\n\r\nOK" ที่ตามมาออก ไม่งั้นเช็ก "ค่าว่างทั้งหมด" ไม่ทำงาน)
  String data = res.substring(idx + 11);
  int eol = data.indexOf('\r');
  if (eol == -1) eol = data.indexOf('\n');
  if (eol != -1) data = data.substring(0, eol);
  data.trim();

  // กรณีค่าว่างทุกตำแหน่ง เช่น "+CGNSSINFO: ,,,,,,,,"
  // = GPS module พร้อมแล้ว แต่ยังหาดาวเทียมไม่เจอ (ต้องอยู่กลางแจ้ง)
  bool allEmpty = true;
  for (int i = 0; i < (int)data.length(); i++) {
    if (data[i] != ',' && data[i] != ' ') {
      allEmpty = false;
      break;
    }
  }
  if (allEmpty) {
    Serial.println("[GPS] ยังไม่ lock ดาวเทียม (รอสักครู่ / ต้องอยู่กลางแจ้ง)");
    return false;
  }

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
  double la = fields[5].toDouble();
  double lo = fields[7].toDouble();

  if (fields[6] == "S") la = -la;
  if (fields[8] == "W") lo = -lo;

  // กันค่าเพี้ยน (เช่น firmware ส่งเป็น ddmm.mmmm จะเกิน 90 / 180)
  if (fabs(la) > 90.0 || fabs(lo) > 180.0) {
    Serial.printf("[GPS] ค่าพิกัดอยู่นอกช่วง (lat=%s lng=%s) — ตรวจรูปแบบองศาจาก firmware\n",
                  fields[5].c_str(), fields[7].c_str());
    return false;
  }

  lat = la;
  lng = lo;
  Serial.printf("[GPS] mode=%d, lat=%.5f, lng=%.5f\n", mode, lat, lng);
  return true;
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
  // authmode 0 = ไม่ตรวจ certificate (ใช้ตอนพัฒนา — ตอนใช้งานจริงควรโหลด CA แล้วใช้ authmode 1)
  sendAT("AT+CSSLCFG=\"authmode\",0,0", 2000);
  sendAT("AT+CSSLCFG=\"enableSNI\",0,1", 2000);       // จำเป็นสำหรับ EMQX Cloud
  sendAT("AT+CSSLCFG=\"ignorelocaltime\",0,1", 2000);

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

// ───────────────────────── กู้ระบบหลังโมดูลรีบูต ─────────────────────────

// เรียกเมื่อเจอ *ATREADY / SMS DONE = โมดูลบูตใหม่ (ไฟตก, brownout, รีเซ็ต)
// หลังรีบูต: GNSS ดับ, MQTT session หาย, network ต้อง attach ใหม่
void recoverModem() {
  Serial.println("\n[MODEM] ตรวจพบโมดูลรีบูตเอง — กำลัง init ใหม่ทั้งหมด...");
  mqttReady = false;
  gpsErrorCount = 0;

  // 1) รอ SIM พร้อมก่อน (หลังบูต CPIN จะยังไม่ READY ทันที)
  bool simReady = false;
  for (int i = 0; i < 20; i++) {
    if (sendAT("AT+CPIN?", 1500).indexOf("READY") != -1) { simReady = true; break; }
    delay(1000);
  }
  Serial.println(simReady ? "[MODEM] SIM พร้อม" : "[MODEM] SIM ยังไม่พร้อม จะลองต่อ");

  setupAPN();

  // 2) รอลงทะเบียนเครือข่าย (สูงสุด 60 วินาที)
  waitNetwork(60000);

  // 3) เปิด GNSS ใหม่ + ต่อ MQTT ใหม่
  enableGPS();
  connectMQTT();

  urcBuf = "";  // กัน *ATREADY ค้างใน buffer แล้วสั่งกู้ซ้ำ
  Serial.println("[MODEM] กู้ระบบเสร็จ\n");
}

// ───────────────────────── Publish ─────────────────────────

// รูปแบบ topic / payload / QoS ไม่เปลี่ยนจากเดิม
bool publishLocation(double lat, double lng, const char *status) {
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
  SerialAT.setRxBufferSize(2048);  // กัน URC burst ตอนโมดูลรีบูตทำ buffer เต็ม
  SerialAT.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN);

  pinMode(LED_BLUE_PIN, OUTPUT);
  digitalWrite(LED_BLUE_PIN, LOW);

  Serial.println("\n========================================");
  Serial.println("   A7680C GPS + MQTT (EMQX Cloud)");
  Serial.println("========================================");

  powerOnModem();

  sendAT("ATE1", 1000);
  sendAT("AT+CPIN?", 2000);
  setupAPN();

  // รอจนลงทะเบียนเครือข่ายได้ (สูงสุด 60 วินาที)
  waitNetwork(60000);

  // ทิ้ง URC จากการบูตปกติ (*ATREADY / SMS DONE / PB DONE) ก่อนเริ่มช่วงที่ต้องจับการรีบูตกลางทาง
  pumpURC();
  urcBuf = "";

  // เปิด GPS และรอ module พร้อม (+CGNSSPWR: READY!) ก่อนทำอย่างอื่น
  enableGPS();

  connectMQTT();

  // ถ้าระหว่าง init มี *ATREADY โผล่ = โมดูลรีบูตกลางทาง (เช่น ตอน TLS connect)
  // ต้องกู้ใหม่ ไม่งั้น GNSS ที่เพิ่งเปิดจะดับและ CGNSSINFO ตอบ ERROR ตลอด
  if (urcBuf.indexOf("*ATREADY") != -1 || urcBuf.indexOf("SMS DONE") != -1) {
    urcBuf = "";
    recoverModem();
  }
  urcBuf = "";
}

void loop() {
  static uint32_t lastPub      = 0;
  static uint32_t lastCheck    = 0;
  static uint32_t nextMqttTry  = 0;
  static uint8_t  noRespCount  = 0;   // นับครั้งที่โมดูลไม่ตอบ AT ติดกัน

  // ── ตรวจสอบการเชื่อมต่อทุก 10 วินาที ──
  if (millis() - lastCheck > 10000) {
    lastCheck = millis();

    if (!modemAlive()) {
      // โมดูลไม่ตอบ AT เลย → ถ้าเป็นติดกัน 3 รอบ ให้รีเซ็ตด้วย PWRKEY แล้วกู้ระบบ
      mqttReady = false;
      noRespCount++;
      Serial.printf("[MODEM] ไม่ตอบ AT ติดกัน %d ครั้ง\n", (int)noRespCount);
      if (noRespCount >= 3) {
        noRespCount = 0;
        hardResetModem();
        urcBuf = "";
        recoverModem();
      }
    } else {
      noRespCount = 0;

      if (!isNetworkConnected()) {
        Serial.println("[NET] หลุดเครือข่าย! รอสัญญาณกลับมา...");
        mqttReady = false;
      } else if (!isMQTTConnected()) {
        // backoff: อย่า reconnect รัวๆ ถ้าล้มเหลว (กินเวลา 30 วิ/รอบ)
        if (millis() >= nextMqttTry) {
          Serial.println("[MQTT] หลุด Broker! กำลัง reconnect...");
          if (connectMQTT()) nextMqttTry = 0;
          else nextMqttTry = millis() + 15000;
        }
      } else {
        Serial.println("[OK] Network และ MQTT พร้อม");
      }
    }
  }

  // ── อ่าน GPS และ Publish ทุก 3 วินาที ──
  if (millis() - lastPub > 3000) {
    lastPub = millis();

    if (!mqttReady) {
      Serial.println("[MQTT] ยังไม่ได้ connect, ข้าม publish รอบนี้");
    } else {
      double lat, lng;
      if (getGPS(lat, lng)) {
        digitalWrite(LED_BLUE_PIN, HIGH);
        bool ok = publishLocation(lat, lng, "active");
        Serial.println(ok ? "[MQTT] Publish สำเร็จ" : "[MQTT] Publish ล้มเหลว");
      } else {
        digitalWrite(LED_BLUE_PIN, LOW);
      }
    }
  }

  // อ่าน URC ที่โมเด็มส่งมาเอง (เก็บใน urcBuf ไม่ให้หาย)
  pumpURC();

  // ── ตรวจจับว่าโมดูลรีบูตเองไหม ──
  // *ATREADY / SMS DONE = โมดูลเพิ่งบูตใหม่ → GNSS ดับ + MQTT session หาย
  if (urcBuf.indexOf("*ATREADY") != -1 || urcBuf.indexOf("SMS DONE") != -1) {
    urcBuf = "";
    recoverModem();
  } else if (urcBuf.indexOf("+CMQTTCONNLOST") != -1 || urcBuf.indexOf("+CMQTTNONET") != -1) {
    urcBuf = "";       // consume แล้วทิ้ง ไม่งั้นข้อความเก่าค้างแล้วสั่ง mqttReady=false ซ้ำหลัง reconnect
    mqttReady = false;
  }
}