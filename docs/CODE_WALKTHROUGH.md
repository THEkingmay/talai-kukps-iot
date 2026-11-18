# อธิบายโค้ด `index/index.ino` แบบละเอียดรายฟังก์ชัน + โฟลวการทำงาน

> **โปรเจกต์:** `talai-kukps-iot` — อุปกรณ์ติดตามรถ (GPS Tracker) บน ESP32 + โมดูล SIMCom **A7680C**
> **ทำอะไร:** อ่านพิกัด GNSS จากโมดูล → ส่งขึ้น MQTT Broker (EMQX Cloud) ผ่าน TLS port 8883
> **ภาษา/เฟรมเวิร์ก:** Arduino (ESP32 core) — ไฟล์เดียว `index.ino` + ไฟล์ตั้งค่า `secret.h`

---

## สารบัญ

1. [ภาพรวมและสถาปัตยกรรม](#1-ภาพรวมและสถาปัตยกรรม)
2. [ฮาร์ดแวร์ / ค่าคงที่ / ตัวแปร global](#2-ฮาร์ดแวร์--ค่าคงที่--ตัวแปร-global)
3. [ชั้น AT I/O — หัวใจของทั้งโปรแกรม](#3-ชั้น-at-io--หัวใจของทั้งโปรแกรม)
4. [จัดการโมเด็มและไฟเลี้ยง (PWRKEY)](#4-จัดการโมเด็มและไฟเลี้ยง-pwrkey)
5. [เครือข่าย (Network / APN / Registration)](#5-เครือข่าย-network--apn--registration)
6. [GPS / GNSS](#6-gps--gnss)
7. [MQTT](#7-mqtt)
8. [กู้ระบบหลังโมดูลรีบูต](#8-กู้ระบบหลังโมดูลรีบูต)
9. [Publish ข้อมูล](#9-publish-ข้อมูล)
10. [`setup()` / `loop()`](#10-setup--loop)
11. [ตาราง AT Command ทั้งหมด](#11-ตาราง-at-command-ทั้งหมด)
12. [จังหวะเวลา / Timeout / Effective rate](#12-จังหวะเวลา--timeout--effective-rate)
13. [ข้อสังเกต จุดเสี่ยง และสิ่งที่ควรปรับปรุง](#13-ข้อสังเกต-จุดเสี่ยง-และสิ่งที่ควรปรับปรุง)

---

## 1. ภาพรวมและสถาปัตยกรรม

### 1.1 ไฟล์ในโปรเจกต์

| ไฟล์ | บทบาท |
|---|---|
| `index/index.ino` | โปรแกรมหลักทั้งหมด (570 บรรทัด) — AT helper, จัดการโมเด็ม, GPS, MQTT, กู้ระบบ, loop หลัก |
| `index/secret.h` | ข้อมูลลับ: Client ID, Broker URL, Port, Username, Password, Topic (ถูก `.gitignore` ไว้ด้วย `*secret.h` — **ไม่ถูก commit** ✅) |

### 1.2 แนวคิดการออกแบบ (Design)

โปรแกรมนี้ **ไม่ใช้ library MQTT / TinyGSM** แต่คุยกับโมดูลด้วย **AT command ดิบ** เองทั้งหมด เพราะ:

1. ต้องการคุมค่า TLS (`AT+CSSLCFG`) เอง เช่น SNI, ไม่ตรวจ certificate, ไม่สนเวลาเครื่อง
2. ต้องการดักจับ **URC** (Unsolicited Result Code — ข้อความที่โมดูลส่งมาเองโดยไม่ถูกถาม) เช่น `*ATREADY`, `+CMQTTCONNLOST`, `+CGNSSPWR: READY!`
3. ต้องการรู้ว่า "โมดูลรีบูตเอง" แล้วกู้ระบบเองได้ (ข้อดีหลักเมื่อเทียบกับ library สำเร็จรูป)

### 1.3 เลเยอร์ของโค้ด (call graph)

```
┌──────────────────────────────────────────────────────────────────────┐
│  setup()                                  loop()                     │
│    │                                        │                        │
│    ├── powerOnModem()                       ├── modemAlive()  ทุก 10s │
│    ├── setupAPN()                           ├── isNetworkConnected() │
│    ├── waitNetwork()                        ├── isMQTTConnected()    │
│    ├── enableGPS()                          ├── connectMQTT()        │
│    ├── connectMQTT()                        ├── getGPS()     ทุก 1s  │
│    └── recoverModem() (ถ้าพบ *ATREADY)      ├── publishLocation()    │
│                                             ├── pumpURC()            │
│  ┌──────────────────────────────────────────┴─────────────────────┐  │
│  │  ชั้นโดเมน:  GPS(3) · NET(4) · MQTT(3) · RECOVERY(1) · PUB(1)   │  │
│  └───────────────────────────┬────────────────────────────────────┘  │
│  ┌───────────────────────────┴────────────────────────────────────┐  │
│  │  ชั้น AT I/O:   sendAT() · sendRaw() · pumpURC() · noteByte()  │  │
│  └───────────────────────────┬────────────────────────────────────┘  │
│  ┌───────────────────────────┴────────────────────────────────────┐  │
│  │  HardwareSerial SerialAT(2)  =  UART2  <── 150 ms ──>  A7680C  │  │
│  └────────────────────────────────────────────────────────────────┘  │
└──────────────────────────────────────────────────────────────────────┘
```

**หลักการสำคัญ 2 ข้อที่ต้องเข้าใจก่อนอ่านฟังก์ชันอื่น:**

1. **ทุกฟังก์ชันเป็น blocking** — `sendAT()` จะนั่งรอจนกว่าจะเจอคำตอบหรือ timeout ไม่มี state machine แบบ non-blocking ทั้งโปรแกรมจึงเดินเป็นลำดับ (sequential)
2. **ทุก byte ที่อ่านจากโมดูลจะถูกเก็บลง `urcBuf` เสมอ** ไม่ว่าจะเป็นการตอบคำสั่งหรือ URC — ทำให้จับสัญญาณ "โมดูลรีบูต" ที่อาจโผล่มาระหว่างรอคำสั่งอื่นได้

---

## 2. ฮาร์ดแวร์ / ค่าคงที่ / ตัวแปร global

### 2.1 การต่อขา (`index.ino:4-9`)

| มาโคร | ค่า | GPIO | หน้าที่ |
|---|---|---|---|
| `MODEM_TX_PIN` | 17 | GPIO17 | ESP32 **TX** → เข้า **RX** ของ A7680C |
| `MODEM_RX_PIN` | 16 | GPIO16 | ESP32 **RX** ← ออกมาจาก **TX** ของ A7680C |
| `MODEM_PWRKEY_PIN` | 4 | GPIO4 | กดปุ่มเปิด/ปิดโมดูล (active-low pulse) |
| `MODEM_BAUDRATE` | 115200 | — | ความเร็ว UART (8N1) |
| `LED_BLUE_PIN` | 2 | GPIO2 | LED สถานะบนบอร์ด |

> **จุดสังเกต:** `SerialAT.begin(MODEM_BAUDRATE, SERIAL_8N1, MODEM_RX_PIN, MODEM_TX_PIN)` — พารามิเตอร์ของ ESP32 คือ `(baud, config, RX, TX)` คือ **สลับลำดับจากชื่อมาโคร** โดยตั้งใจ (RX ของ ESP32 ต้องต่อกับ TX ของโมดูล) ถูกต้องแล้ว

### 2.2 มาโครที่ปรับแต่งได้ (`index.ino:11-21`)

| มาโคร | ค่าเริ่มต้น | ผล |
|---|---|---|
| `MODEM_APN` | `""` (ว่าง) | ถ้ามีค่า → เรียก `AT+CGDCONT=1,"IP","<APN>"` ก่อนรอเครือข่าย; ถ้าว่าง → **ไม่แตะ APN เลย** (ใช้ค่าเดิมในโมดูล/SIM) |
| `PWRKEY_RELEASE_HIZ` | `0` | `0` = ดันขา PWRKEY เป็น **HIGH ค้าง** หลังกด; `1` = ปล่อยเป็น **hi-Z (INPUT)** — เลือกตามวงจรฮาร์ดแวร์จริง |

### 2.3 ตัวแปร global (`index.ino:23-29`)

| ตัวแปร | ชนิด | ความหมาย |
|---|---|---|
| `SerialAT(2)` | `HardwareSerial` | ผูกกับ UART2 ของ ESP32 คุยกับโมดูล |
| `mqttReady` | `bool` | **ธงสถานะ MQTT ที่โปรแกรมจัดการเอง** — `true` = เชื่อมต่อแล้ว (ไม่ใช่ค่าที่อ่านจากโมดูลโดยตรง) ใช้ตัดสินว่าจะ publish ได้หรือไม่ |
| `urcBuf` | `String` | **หน้าต่างเลื่อน (sliding window) 1024 ตัวอักษร** เก็บทุก byte ที่โมดูลส่งมา ใช้ตรวจ `*ATREADY` / `SMS DONE` / `+CMQTTCONNLOST` |
| `gpsErrorCount` | `uint8_t` | นับจำนวนครั้งติดกันที่ `AT+CGNSSINFO` ตอบ ERROR (≥3 → GNSS น่าจะดับ → `gpsPowerCycle()`) |

> **ทำไมต้องมี `urcBuf` แยกจากค่าที่ `sendAT()` คืน?** เพราะ URC อาจโผล่มา **ระหว่าง** ที่เรากำลังรอคำตอบของคำสั่งอื่นก็ได้ (เช่น รอ `AT+CPIN?` อยู่แล้วโมดูลแทรก `*ATREADY` มา) ถ้าเก็บเฉพาะตอนที่ caller สนใจ URC นั้นจะหลุดหาย

---

## 3. ชั้น AT I/O — หัวใจของทั้งโปรแกรม

ทั้ง 4 ฟังก์ชันนี้คือรากฐาน ทุกอย่างที่เหลือถูกสร้างบนนี้

### 3.1 `noteByte(char c, String *resp)` — `index.ino:34-39`

**ทำอะไร:** "บันทึก 1 byte ที่เพิ่งอ่านมาจากโมดูล" ลง 3 ที่พร้อมกัน

```
byte ที่อ่านจาก SerialAT
   │
   ├─► ถ้า resp != nullptr → ต่อท้าย resp       (คำตอบของคำสั่งที่กำลังรอ)
   ├─► ต่อท้าย urcBuf เสมอ                       (กัน URC หาย)
   ├─► Serial.write(c)                           (echo ออก USB ให้ debug)
   └─► ถ้า urcBuf ยาว > 1024 → ตัดหัวทิ้งให้เหลือ 1024 ตัวท้าย
```

| | |
|---|---|
| **พารามิเตอร์** | `c` = ตัวอักษรที่อ่านมา, `resp` = ปลายทางคำตอบของคำสั่ง (`nullptr` ได้) |
| **คืนค่า** | ไม่มี (`void`) |
| **ผลข้างเคียง** | เขียน `Serial` (USB) ทุก byte, แก้ `urcBuf` global |

**โฟลว**

```c
static void noteByte(char c, String *resp) {
  if (resp) *resp += c;                 // 1) เก็บลงคำตอบของคำสั่ง (ถ้ามี)
  urcBuf += c;                          // 2) เก็บลง buffer URC เสมอ
  Serial.write(c);                      // 3) สะท้อนออกพอร์ต debug
  if (urcBuf.length() > 1024)           // 4) จำกัดขนาด กันหน่วยความจำบวม
    urcBuf.remove(0, urcBuf.length() - 1024);
}
```

> **ทำไมต้องจำกัด 1024?** ถ้าไม่จำกัด `urcBuf` จะโตไม่หยุดทั้งวัน (ทุก byte ของทุกคำสั่งสะสมลงไป) → RAM เต็ม / heap fragmentation / String realloc บ่อยจนแฮงก์ การเก็บแค่ 1024 ตัวท้ายก็เพียงพอเพราะข้อความที่สนใจ (`*ATREADY`, `SMS DONE`) สั้นมาก

### 3.2 `pumpURC()` — `index.ino:41-43`

**ทำอะไร:** "รีด" byte ที่ค้างอยู่ใน RX buffer ของ UART ออกมาให้หมด แล้วส่งเข้า `noteByte()` โดยไม่เก็บลง `resp` ใด ๆ

```c
static void pumpURC() {
  while (SerialAT.available()) noteByte(SerialAT.read(), nullptr);
}
```

| | |
|---|---|
| **ที่เรียกจาก** | ต้นของ `sendAT()` (ก่อนส่งคำสั่ง), ท้ายของ `loop()`, `setup()` หลังรอเครือข่าย |
| **คืนค่า** | ไม่มี |

**ทำไมสำคัญ:** ถ้าปล่อย byte ค้างไว้ พอ `sendAT()` ตัวถัดไปเริ่มอ่าน มันจะอ่านของเก่าปนกับของใหม่ → ตรวจ pattern ผิด (เช่นคิดว่า `*ATREADY` เพิ่งเกิด ทั้งที่เกิดนานแล้ว) และ buffer 2048 byte ที่ `setRxBufferSize()` ตั้งไว้ก็จะเต็ม

### 3.3 `sendAT(cmd, timeoutMs = 3000, until = nullptr)` — `index.ino:49-71`

**ทำอะไร:** ส่งคำสั่ง AT 1 บรรทัด แล้ว **รอคำตอบ** จนเข้าเงื่อนไขหยุด (หรือหมดเวลา) แล้วคืนคำตอบทั้งก้อนเป็น `String`

#### พารามิเตอร์

| ชื่อ | ชนิด | ความหมาย |
|---|---|---|
| `cmd` | `const String&` | คำสั่งที่จะส่ง (จะเติม `\r\n` ให้อัตโนมัติด้วย `println`) |
| `timeoutMs` | `uint32_t` | เวลารอสูงสุด (default 3000 ms) |
| `until` | `const char*` | สตริงที่ถือว่า "ได้คำตอบแล้ว" → ใช้เมื่อคำตอบ **ไม่ใช่ `OK`** เช่นรอ `">"` หรือ `"+CMQTTCONNECT: 0,"` |

#### เงื่อนไขหยุด (เรียงตามลำดับที่โค้ดเช็ก)

| ลำดับ | เงื่อนไข | ความหมาย |
|---|---|---|
| 1 | เจอ `until` (ถ้าระบุมา) | คำตอบที่ต้องการมาแล้ว → ออกทันที |
| 2 | เจอ `OK\r\n` (ถ้า**ไม่**ระบุ `until`) | คำสั่งสำเร็จ |
| 3 | เจอ `"ERROR"` (ทั้งสองกรณี) | คำสั่งล้มเหลว → **ออกทันทีไม่ต้องรอ timeout** (ประหยัดเวลา 3 วิ/ครั้ง) |
| 4 | ครบ `timeoutMs` | หมดเวลา → คืนสิ่งที่ได้มา (อาจเป็น `""`) |

> ⚠️ **หมายเหตุ:** ถ้าระบุ `until` แล้วโมดูลตอบแค่ `OK` (ไม่มี `until`) → จะรอจน**ครบ timeout** เสมอ เช่น `AT+NETOPEN` ที่เปิดอยู่แล้ว จะกินเวลาเต็ม 8 วินาที

#### โฟลว

```
sendAT(cmd, timeout, until)
 │
 ├─(1) pumpURC()              ← รีด URC ค้างก่อน (ไม่ทิ้ง!)
 ├─(2) Serial.print "[SEND] >> cmd" + SerialAT.println(cmd)
 │
 ├─(3) loop  until (millis()-start < timeout):
 │        ├─ อ่านทุก byte ที่มี → noteByte(c, &resp)
 │        ├─ if (until)      : resp มี until?  → break
 │        │  else            : resp มี "OK\r\n"?→ break
 │        ├─ if resp มี "ERROR"?               → break
 │        └─ delay(5)                          ← ผ่อน CPU + ให้ WDT ได้พัก
 │
 └─(4) return resp              ← คำตอบดิบทั้งก้อน
```

```c
String sendAT(const String &cmd, uint32_t timeoutMs = 3000, const char *until = nullptr) {
  pumpURC();                                   // 1) เก็บ URC ค้าง
  Serial.print("[SEND] >> "); Serial.println(cmd);
  SerialAT.println(cmd);                       // 2) ส่งคำสั่ง (+ \r\n)
  String resp = "";
  uint32_t start = millis();
  while (millis() - start < timeoutMs) {       // 3) รอคำตอบ
    while (SerialAT.available()) noteByte(SerialAT.read(), &resp);
    if (until) { if (resp.indexOf(until) != -1) break; }
    else       { if (resp.indexOf("OK\r\n") != -1) break; }
    if (resp.indexOf("ERROR") != -1) break;
    delay(5);
  }
  return resp;                                 // 4)
}
```

**ตัวอย่างการใช้งานจริงในโค้ด**

| การเรียก | ทำไม | คำตอบที่คาดหวัง |
|---|---|---|
| `sendAT("AT", 1000)` | เช็กมีชีวิต | `OK` |
| `sendAT("AT+CGNSSINFO", 3000)` | ขอพิกัด | `+CGNSSINFO: ...` + `OK` |
| `sendAT("AT+CMQTTTOPIC=0,25", 2000, ">")` | ขอส่ง topic | `\r\n>` (prompt) |
| `sendAT(connCmd, 30000, "+CMQTTCONNECT: 0,")` | ต่อ broker | `+CMQTTCONNECT: 0,0` |

**ข้อจำกัดที่ควรรู้**
- เป็น **blocking**: ระหว่างรอ `loop()` ทั้งรอบหยุดชะงัก (ดูข้อ 12)
- `resp` โตได้ไม่จำกัด ถ้าโมดูลพ่นข้อมูลยาว ๆ (ในทางปฏิบัติไม่เกิด)
- ตรวจ `ERROR` แบบ substring — ถ้า payload/echo บังเอิญมีคำว่า `ERROR` จะหยุดก่อนเวลาอันควร (ในโปรเจกต์นี้ไม่เกิด)

### 3.4 `sendRaw(data, timeoutMs = 3000)` — `index.ino:74-88`

**ทำอะไร:** ส่ง **ข้อมูลดิบ** (ไม่ใช่คำสั่ง AT) หลังโมดูลตอบ `">"` แล้วรอ `OK`/`ERROR`
ใช้กับขั้นตอน `AT+CMQTTTOPIC` / `AT+CMQTTPAYLOAD` ที่ต้องส่งข้อมูลตามหลัง prompt

```
publishLocation()
   ├─ sendAT("AT+CMQTTTOPIC=0,<len>", 2000, ">")   ← ได้ prompt '>' แล้ว
   ├─ sendRaw(topic, 2000)                         ← ส่งข้อมูลจริง (ไม่ต้อง \r\n นำหน้า)
   ├─ sendAT("AT+CMQTTPAYLOAD=0,<len>", 2000, ">")
   ├─ sendRaw(payload, 2000)
   └─ sendAT("AT+CMQTTPUB=0,1,60", 10000, "+CMQTTPUB:")
```

| | |
|---|---|
| **ต่างจาก `sendAT`** | ไม่เติม `\r\n` ท้ายข้อมูล (`print` ไม่ใช่ `println`) เพราะ payload ต้องตรง byte ตามที่ประกาศความยาวไว้ |
| **ไม่ตรวจว่าได้ `>` ก่อน** | ผู้เรียก (`publishLocation`) ต้องเช็กเอง |
| **ผลข้างเคียง** | ระหว่างรอ ถ้าเจอ `+CMQTTPUB:` หรือ URC อื่น มันจะถูกกลืนเข้า `resp` ทิ้ง (แต่ยังอยู่ใน `urcBuf` ✅) |

---

## 4. จัดการโมเด็มและไฟเลี้ยง (PWRKEY)

> A7680C **ไม่มีขา RESET แยก** — การรีเซ็ตทำโดย "กด PWRKEY" ซึ่งเป็นปุ่ม toggle: ถ้าเปิดอยู่ = ปิด, ถ้าปิดอยู่ = เปิด นี่คือเหตุผลที่โค้ดต้องเช็กก่อนกดทุกครั้ง

### 4.1 `modemAlive()` — `index.ino:92-98`

**ทำอะไร:** ยิง `AT` ซ้ำได้ถึง 3 ครั้ง (timeout 1 วิ/ครั้ง) ถ้ามีครั้งใดได้ `OK` → `true`

```c
bool modemAlive() {
  for (int i = 0; i < 3; i++) {
    String r = sendAT("AT", 1000);
    if (r.indexOf("OK") != -1) return true;
  }
  return false;
}
```

| | |
|---|---|
| **คืนค่า** | `true` = โมดูลตอบสนอง, `false` = เงียบ (ลองแล้ว 3 ครั้ง) |
| **เวลาที่ใช้** | เร็วสุด ~10 ms, ช้าสุด 3 × 1 วิ = **3 วินาที** |
| **ทำไมต้อง 3 ครั้ง** | UART อาจมี noise/baud เพี้ยนชั่วขณะ หรือโมดูลกำลัง busy (เช่นตอนเขียน NVM) — ยิงครั้งเดียวง่ายต่อ false negative แล้วไปกด PWRKEY ปิดโมเดลทั้งที่ยังทำงานดี |

### 4.2 `pwrkeyRelease()` — `index.ino:101-107`

**ทำอะไร:** คืนสภาพขา PWRKEY หลังกดเสร็จ ตามค่า `PWRKEY_RELEASE_HIZ`

```c
#if PWRKEY_RELEASE_HIZ
  pinMode(MODEM_PWRKEY_PIN, INPUT);      // ปล่อยลอย hi-Z
#else
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);  // ดัน HIGH ค้าง (default)
#endif
```

| ค่า | พฤติกรรม | เหมาะกับ |
|---|---|---|
| `0` (default) | ดัน HIGH ค้าง | วงจรที่มี pull-up อยู่แล้ว / ต้องการ level ชัดเจน |
| `1` | ปล่อยเป็น INPUT (hi-Z) | ต่อตรงเข้า PWRKEY ของโมดูล (active-low, มี pull-up ภายในอยู่แล้ว) — **ปลอดภัยกว่าถ้าไม่แน่ใจ** เพราะไม่มีกระแสไหลค้างเข้าโมดูล |

### 4.3 `pwrkeyPulse()` — `index.ino:110-119`

**ทำอะไร:** กด PWRKEY 1 ครั้งตามจังหวะที่ datasheet กำหนด (HIGH → LOW ค้าง 1.5 วิ → ปล่อย → รอ 3 วิ)

```
pinMode(OUTPUT) ─ HIGH ──100ms── LOW ──1500ms── release ──3000ms── จบ
                  idle          ┌── กดจริง ──┐        (รอโมดูล boot/ปิด)
```

```c
static void pwrkeyPulse() {
  if (MODEM_PWRKEY_PIN < 0) return;   // เผื่อกรณีไม่ได้ต่อขานี้
  pinMode(MODEM_PWRKEY_PIN, OUTPUT);
  digitalWrite(MODEM_PWRKEY_PIN, HIGH);
  delay(100);                          // ให้ระดับ HIGH นิ่งก่อนกด
  digitalWrite(MODEM_PWRKEY_PIN, LOW);
  delay(1500);                         // ความยาวการกด (ต้อง > ~500ms ตามสเปก)
  pwrkeyRelease();
  delay(3000);                         // รอโมดูลทำงานให้เสร็จ
}
```

| | |
|---|---|
| **เวลารวม** | 100 + 1500 + 3000 = **4.6 วินาที** |
| **ผล** | ถ้าโมดูล**เปิดอยู่ → ปิด**, ถ้า**ปิดอยู่ → เปิด** (toggle) |
| **จุดที่ควรรู้** | `if (MODEM_PWRKEY_PIN < 0)` เป็น dead code เพราะ define เป็น `4` — เป็น defensive check เผื่ออนาคตเปลี่ยนเป็น `-1` (แต่ต้องเปลี่ยน `#define` เป็น `-1` ก่อน ไม่งั้น compiler มองเป็นค่าคงที่) |

### 4.4 `powerOnModem()` — `index.ino:121-137`

**ทำอะไร:** "เปิดโมดูล" อย่างปลอดภัย — **เช็กก่อนว่าเปิดอยู่แล้วหรือยัง** ถ้าเปิดอยู่จะไม่กด PWRKEY (เพราะจะกลายเป็นสั่งปิด!)

```
powerOnModem()
 │
 ├─ modemAlive()? ──yes──► log "เปิดอยู่แล้ว ข้ามการ power on" ──► return  ✅
 │        no
 ├─ pwrkeyPulse()                      ← กดเปิด (4.6 วิ)
 │
 └─ รอสูงสุด 15 วิ:  ทุก 500ms → modemAlive()?  ──yes──► return ✅
                      │
                      └─ ครบ 15 วิ ──► log "ไม่ตอบสนองหลัง power on"
```

| | |
|---|---|
| **เวลาที่ใช้** | 0 วิ (ถ้าเปิดอยู่) หรือ ~5-20 วิ (ถ้าต้องเปิด) |
| **ทำไมต้องวนรอ 15 วิ** | หลังปล่อย PWRKEY โมดูลใช้เวลา 3-10 วิในการ boot แล้วเริ่มตอบ AT — รอครั้งเดียว 3 วิไม่พอ |

### 4.5 `hardResetModem()` — `index.ino:140-145`

**ทำอะไร:** ใช้เมื่อโมดูล **ค้างสนิท** (ไม่ตอบ AT) แต่ยังอาจรับคำสั่งระดับฮาร์ดแวร์ได้ (PWRKEY ทำงานที่ระดับขา ไม่ต้องพึ่ง UART)

```
pwrkeyPulse()   ← รอบ 1 = สั่งปิด (ถ้ายังเปิดอยู่)
   │
   delay(2000)  ← ให้โมดูลปิดสนิทจริง ๆ ก่อน
   │
powerOnModem()  ← ถ้ายังไม่ตอบ AT จะกดอีกรอบ = เปิด
```

| | |
|---|---|
| **ทำไมต้องกด 2 รอบ** | เพราะ PWRKEY เป็น toggle ถ้าไม่รู้สถานะแน่ชัด กด 1 ครั้งอาจ = ปิดซ้ำ (ไม่เกิดอะไร) การกด "ปิด → รอ → เปิด" จึงเป็นลำดับที่แน่นอนที่สุด |
| **ถูกเรียกจาก** | `loop()` เมื่อ `noRespCount >= 3` (ไม่ตอบ AT ติดกัน 3 รอบ × 10 วิ = ~30 วิ) |

---

## 5. เครือข่าย (Network / APN / Registration)

### 5.1 `setupAPN()` — `index.ino:174-177`

**ทำอะไร:** ตั้ง APN ให้ PDP Context 1 **เฉพาะเมื่อกำหนด `MODEM_APN` ไว้** (ถ้าว่าง = no-op = ไม่ยุ่งกับค่าเดิมของ SIM)

```c
static void setupAPN() {
  if (strlen(MODEM_APN) == 0) return;                                  // ไม่ได้ตั้ง → ออก
  sendAT(String("AT+CGDCONT=1,\"IP\",\"") + MODEM_APN + "\"", 2000);   // ตั้ง APN
}
```

| | |
|---|---|
| **เรียกจาก** | `setup()` และ `recoverModem()` (เพราะหลังรีบูต PDP context อาจรีเซ็ต) |
| **ผลลัพธ์** | `+CGDCONT: 1,"IP","<apn>",...` ถูกบันทึก → โมดูลจะใช้ค่านี้ตอน `AT+NETOPEN` |

### 5.2 `regOk(const String& r, const char* tag)` — `index.ino:148-155`

**ทำอะไร:** **parser** อ่านค่า `stat` จากบรรทัด `+CREG: <n>,<stat>[,<lac>,<ci>]` หรือ `+CEREG: ...` แล้วตัดสินว่า "ลงทะเบียนแล้วหรือยัง"

```
"+CEREG: 0,1"  →  tag="+CEREG:"  →  หา ',' ตัวแรก  →  stat = 1  →  true  (home network)
       ↑   ↑
     n=0   stat=1
```

```c
static bool regOk(const String &r, const char *tag) {
  int i = r.indexOf(tag);
  if (i < 0) return false;              // ไม่มี tag นี้ในคำตอบ
  int c = r.indexOf(',', i);            // หา comma ตัวแรกหลัง tag
  if (c < 0) return false;              // ไม่มีรูปแบบที่คาด
  int stat = r.substring(c + 1).toInt();// แปลงส่วนที่ตามมาเป็นตัวเลข
  return stat == 1 || stat == 5;        // 1 = home, 5 = roaming
}
```

| `stat` | ความหมาย |
|---|---|
| 0 | not registered, ไม่ได้ค้นหา |
| **1** | **registered, home network** ✅ |
| 2 | กำลังค้นหา |
| 3 | registration denied |
| **5** | **registered, roaming** ✅ |
| 4 | unknown |

> `toInt()` จะหยุดที่ตัวอักษรที่ไม่ใช่ตัวเลข เช่น `"1,\"00A0\""` → ได้ `1` ถูกต้องตามต้องการ

### 5.3 `isNetworkConnected()` — `index.ino:158-163`

**ทำอะไร:** เช็กการลงทะเบียนเครือข่าย โดย **ลอง `CEREG` ก่อน** แล้ว fallback ไป `CREG`

```
AT+CEREG?  ──► regOk(r,"+CEREG:")? ──yes──► true ✅
                    │ no
                    ▼
AT+CREG?   ──► regOk(r,"+CREG:")?  ──yes──► true ✅
                    │ no
                    ▼
                  false
```

**ทำไมต้อง CEREG ก่อน:** A7680C เป็น **LTE** ซึ่งใช้ EPS registration (`CEREG`) เป็นหลัก — บางเฟิร์มแวร์ `CREG` จะค้างที่ `stat=0` ตลอดทั้งที่เน็ตใช้ได้แล้ว

| | |
|---|---|
| **เวลาที่ใช้** | ~20 ms (สำเร็จที่ CEREG) ถึง **~4 วินาที** (ทั้งคู่ timeout) |
| **ถูกเรียกจาก** | `waitNetwork()` (ใน setup/recover), `loop()` ทุก 10 วิ |

### 5.4 `waitNetwork(timeoutMs)` — `index.ino:165-171`

**ทำอะไร:** วนเช็ก `isNetworkConnected()` ทุก 2 วิ จนกว่าจะติดหรือครบเวลา

```
start = millis()
  │
  └─ while (!isNetworkConnected() && millis()-start < timeout):
        log "[NET] รอลงทะเบียนเครือข่าย..."
        delay(2000)
```

| | |
|---|---|
| **เรียกจาก** | `setup()` ด้วย 60000 ms, `recoverModem()` ด้วย 60000 ms |
| **คืนค่า** | ไม่มี (void) — **ไม่บอกว่าสำเร็จหรือไม่** ผู้เรียกไม่รู้ผล |

---

## 6. GPS / GNSS

> **บริบทสำคัญ:** GPS ของ A7680C เป็นชิปแยกที่ต้องสั่ง **"เปิด"** เอง (`AT+CGNSSPWR=1`) และใช้เวลา boot หลายวินาทีจนส่ง URC `+CGNSSPWR: READY!` — ถ้าเรียก `AT+CGNSSINFO` ก่อน READY จะได้ **ERROR** ทุกครั้ง นี่คือที่มาของทั้ง 3 ฟังก์ชันในหมวดนี้

### 6.1 `enableGPS()` — `index.ino:183-213`

**ทำอะไร:** เปิด GPS module แล้ว **รอจนพร้อมจริง** (READY) ก่อนคืนการทำงาน

```
enableGPS()
 │
 ├─ log "[GPS] เปิด GPS module..."
 ├─ r = sendAT("AT+CGNSSPWR=1", 3000)
 │
 ├─ r ไม่มี "OK"? ──yes──► log "ไม่สำเร็จ (อาจเปิดอยู่แล้ว)" ──► return   ⚠️ ไม่รอ READY
 │        no
 ├─ r มี "+CGNSSPWR: READY" อยู่แล้ว? ──yes──► log "พร้อมแล้ว ✓" ──► return ✅ (เร็วสุด)
 │        no
 └─ รอ URC "+CGNSSPWR: READY" สูงสุด 15 วิ (loop อ่าน SerialAT → noteByte)
        ├─ เจอ READY ──► log "พร้อมแล้ว ✓" ──► return ✅
        └─ ครบ 15 วิ ──► log "หมดเวลารอ READY — จะพยายามใช้งานต่อไป" (ไม่ทิ้งงาน)
```

| | |
|---|---|
| **เวลาที่ใช้** | เร็วสุด ~50 ms (เปิดอยู่แล้ว+READY มาติดกับ OK) / ปกติ 2-10 วิ / ช้าสุด ~18 วิ |
| **เรียกจาก** | `setup()`, `recoverModem()`, `gpsPowerCycle()` |
| **ทำไมไม่ return หลัง OK** | `OK` แปลว่า "รับคำสั่งแล้ว" ไม่ได้แปลว่า GNSS engine พร้อม — ต้องรอ READY |
| **จุดอ่อน** | ถ้า `AT+CGNSSPWR=1` ตอบ ERROR (เช่น GNSS เปิดค้างอยู่) จะ `return` ทันทีโดยไม่รอ READY → `getGPS()` จะ ERROR 3 ครั้งแล้วค่อย `gpsPowerCycle()` กู้เอง |

### 6.2 `gpsPowerCycle()` — `index.ino:216-231`

**ทำอะไร:** ซ่อม GNSS ที่ "ตาย" หลังโมดูลรีบูต โดยตัดสินจากสถานะจริงก่อนว่าจะเปิดใหม่หรือปิด-เปิดใหม่

```
gpsPowerCycle()
 │
 ├─ pwr = sendAT("AT+CGNSSPWR?", 2000)
 │
 ├─ pwr ไม่มี "+CGNSSPWR: 1"  (คือตอบ 0 = ดับอยู่)
 │     └─► enableGPS()                    ← เปิดใหม่ตรง ๆ แล้วรอ READY
 │
 └─ pwr มี "+CGNSSPWR: 1"  (รายงานว่าเปิด แต่ใช้งานไม่ได้ = engine ค้าง)
       ├─ sendAT("AT+CGNSSPWR=0", 3000)   ← สั่งปิด
       ├─ delay(1000)                     ← ให้ดับสนิท
       └─ enableGPS()                     ← เปิดใหม่
```

| | |
|---|---|
| **เรียกจาก** | `getGPS()` เมื่อ `gpsErrorCount >= 3` |
| **ทำไมต้องดู `CGNSSPWR?` ก่อน** | ถ้า GNSS ดับอยู่ การสั่ง `=0` ซ้ำไม่ได้ช่วยอะไร และถ้าเปิดอยู่แล้วสั่ง `=1` ซ้ำอาจไม่เกิดผล (ฟิร์มแวร์บางรุ่นไม่ตอบสนอง) → ต้องแยกสองเส้นทาง |

### 6.3 `getGPS(double &lat, double &lng)` — `index.ino:233-317`

**ทำอะไร:** ขอพิกัดจาก GNSS, parse ข้อความ `+CGNSSINFO`, แปลงเป็นทศนิยม และส่งค่าออกทาง reference

#### ลำดับการทำงาน (7 ด่าน)

```
getGPS(&lat, &lng)
 │
 │ res = sendAT("AT+CGNSSINFO", 3000)
 │
 ├─【ด่าน 1】res ไม่มี "+CGNSSINFO:" ──► gpsErrorCount++
 │                                       └─ ถ้า >= 3 → gpsPowerCycle() (รีเซ็ตนับเป็น 0)
 │                                       return false      ❌ (GNSS ดับ/ไม่พร้อม)
 │
 ├─ gpsErrorCount = 0                  ← มีคำตอบแล้ว รีเซ็ตนับ
 ├─ ตัดเอาเฉพาะบรรทัดข้อมูล (หลัง ": ")
 │     res.substring(idx+11) แล้วตัดที่ '\r' หรือ '\n' แรก → data.trim()
 │
 ├─【ด่าน 2】data เป็น "," หรือ " " ล้วน? (เช่น ",,,,,,,,") 
 │     ──yes──► log "ยังไม่ lock ดาวเทียม (รอสักครู่ / ต้องอยู่กลางแจ้ง)"   return false ❌
 │
 ├─【ด่าน 3】แยกด้วย ',' เป็น fields[] สูงสุด 12 ช่อง
 │     ถ้าได้ < 9 ช่อง ──► log "parse ล้มเหลว — fields ไม่ครบ"  return false ❌
 │
 ├─【ด่าน 4】mode = fields[0].toInt() ; ถ้า mode == 0 
 │     ──► log "ยังไม่ได้ lock สัญญาณ..."  return false ❌
 │
 ├─【ด่าน 5】fields[5] (lat) หรือ fields[7] (lng) ว่าง?
 │     ──yes──► log "lat/lng ว่างเปล่า"      return false ❌
 │
 ├─【ด่าน 6】la = fields[5].toDouble(), lo = fields[7].toDouble()
 │           fields[6]=="S" → la = -la ;  fields[8]=="W" → lo = -lo
 │           |la|>90 หรือ |lo|>180 ──► log "ค่าพิกัดอยู่นอกช่วง"  return false ❌
 │
 └─【ด่าน 7】lat = la ; lng = lo ; log "[GPS] mode=, lat=, lng="  ✅ return true
```

#### ตาราง index ของ `+CGNSSINFO`

| idx | ความหมาย | ที่โค้ดใช้ |
|---|---|---|
| 0 | `mode` (0=no fix, 1=2D, 2=3D, ...) | ด่าน 4 |
| 1 | จำนวนดาว GPS | — |
| 2 | จำนวนดาว GLONASS | — |
| 3 | จำนวนดาว BEIDOU | — |
| 4 | จำนวนดาว Galileo | — |
| **5** | **latitude (decimal degrees)** | **lat** |
| **6** | **N/S** | **เครื่องหมาย lat** |
| **7** | **longitude (decimal degrees)** | **lng** |
| **8** | **E/W** | **เครื่องหมาย lng** |
| 9 | วันที่ (ddmmyy) | — (โค้ดไม่ใช้) |
| 10 | เวลา UTC (hhmmss.s) | — |
| 11 | altitude | — |

> **จุดสำคัญ:** A7680C ส่ง lat/lng มาเป็น **decimal degrees เต็ม** แล้ว *ไม่ใช่* ddmm.mmmm แบบโมดูล GPS รุ่นเก่า — โค้ดจึง**ไม่หาร 60** (มีคอมเมนต์กำกับไว้ที่ `index.ino:299`) และมี guard `fabs > 90/180` ไว้ดักกรณีเฟิร์มแวร์เปลี่ยนรูปแบบ

| | |
|---|---|
| **คืนค่า** | `true` = ได้พิกัดสมบูรณ์, `false` = ยังใช้ไม่ได้ (หลายสาเหตุ) |
| **ผลข้างเคียง** | แก้ `lat`/`lng` ผ่าน reference **เฉพาะเมื่อสำเร็จ**, แก้ `gpsErrorCount`, อาจเรียก `gpsPowerCycle()` (blocking ~5-20 วิ) |
| **คืนค่า doubling** | ใช้ reference เพื่อให้แยก "สำเร็จ/ไม่สำเร็จ + ค่า" ในการเรียกครั้งเดียว — `if (getGPS(lat, lng))` |

---

## 7. MQTT

### 7.1 `cleanupMQTT()` — `index.ino:322-327`

**ทำอะไร:** "ล้างท่อ" ให้สะอาดก่อนเริ่มใหม่ — ปลด connection, ปลด client, หยุด service ตามลำดับ

```
AT+CMQTTDISC=0,60   ← disconnect client 0 (timeout 60)   [ERROR ได้ ถ้าไม่ได้ต่อ]
AT+CMQTTREL=0       ← release client 0
AT+CMQTTSTOP        ← stop MQTT service
mqttReady = false
```

| | |
|---|---|
| **เรียกจาก** | `connectMQTT()` (ต้นทางเสมอ), เมื่อ `CMQTTSTART`/`ACCQ`/`SSLCFG`/`CONNECT` ล้มเหลว |
| **ทำไมต้อง DISC ก่อน REL** | โมดูลต้องปิด connection ให้เรียบร้อยก่อนคืน client slot ไม่งั้น slot ค้าง (session ค้าง) → ต่อใหม่ไม่ติด |
| **เวลาที่ใช้** | ~1-8 วิ (แต่ละคำสั่งมี timeout ของตัวเอง) |

### 7.2 `isMQTTConnected()` — `index.ino:329-337`

**ทำอะไร:** ถามโมดูลว่า MQTT client 0 ยังต่ออยู่ไหม **โดยอิง `mqttReady` เป็นประตูแรก** (ประหยัดเวลา)

```
mqttReady == false? ──yes──► return false     (ไม่ต้องยิง AT เลย)
        no
        ▼
AT+CMQTTCONNECT?  →  พบ "+CMQTTCONNECT: 0,\"" ? ──yes──► true  ✅
        │ no
        └─ mqttReady = false ; return false  ❌
```

| | |
|---|---|
| **ทำไมต้องดู `mqttReady` ก่อน** | ถ้าเราไม่เคยต่อสำเร็จ การถามโมดูลก็เสียเวลาเปล่า 2 วิ |
| **ทำไมต้องเซ็ต `mqttReady=false` เมื่อล้มเหลว** | ให้ `loop()` รู้ทันทีว่าต้อง reconnect (ไม่ต้องรอ detect ซ้ำ) |
| **เวลาที่ใช้** | 0 ms (ถ้า `mqttReady=false`) / ~20 ms (ปกติ) / 2 วิ (timeout) |

### 7.3 `connectMQTT()` — `index.ino:339-395`

**ทำอะไร:** ตั้งค่า TLS → เริ่ม MQTT service → จอง client → ผูก SSL → ต่อ broker (ทั้งหมด 9 ขั้น)

#### โฟลวเต็ม

```
connectMQTT()
 │
 ① cleanupMQTT()                                  ← ล้างของเก่าเสมอ
 │
 ② sendAT("AT+NETOPEN", 8000, "+NETOPEN:")        ← เปิด PDP/network (ซ้ำได้ ERROR ไม่เป็นไร)
 │
 ③ ── ตั้ง SSL context 0 (4 คำสั่ง) ──
 │    AT+CSSLCFG="sslversion",0,3                 ← TLS 1.2
 │    AT+CSSLCFG="authmode",0,0                   ← ไม่ตรวจ certificate ⚠️ (dev เท่านั้น)
 │    AT+CSSLCFG="enableSNI",0,1                  ← ต้องมี! EMQX Cloud ใช้ SNI เลือก cert
 │    AT+CSSLCFG="ignorelocaltime",0,1            ← ไม่เช็กเวลาเครื่อง (ESP32 ไม่มี RTC ที่แม่น)
 │
 ④ r = AT+CMQTTSTART (5s, until "+CMQTTSTART:")
 │    r ไม่มี "+CMQTTSTART: 0"? ──► log + cleanupMQTT() + return false ❌
 │
 ⑤ r = AT+CMQTTACCQ=0,"<MQTT_CLIENT_ID>",1        ← 1 = ใช้ SSL
 │    r ไม่มี "OK"? ──► log + cleanupMQTT() + return false ❌
 │
 ⑥ r = AT+CMQTTSSLCFG=0,0                         ← ผูก SSL ctx 0 เข้า client 0
 │    r ไม่มี "OK"? ──► log + cleanupMQTT() + return false ❌
 │
 ⑦ res = AT+CMQTTCONNECT=0,"tcp://<URL>:<PORT>",60,1,"<user>","<pass>"
 │        (timeout 30 วิ, until "+CMQTTCONNECT: 0,")
 │    ├─ มี "+CMQTTCONNECT: 0,0" ──► mqttReady=true ; log "เชื่อมต่อสำเร็จ!" ✅ return true
 │    └─ อื่น ๆ ──► log "เชื่อมต่อล้มเหลว!" + cleanupMQTT() + return false ❌
```

#### รายละเอียดพารามิเตอร์ที่สำคัญ

| คำสั่ง | พารามิเตอร์ | ความหมาย |
|---|---|---|
| `AT+CMQTTACCQ=0,"id",1` | `0` = client index, `1` = **ใช้ SSL** | ต้องเป็น 1 ไม่งั้นจะต่อ 8883 ไม่ได้ |
| `AT+CMQTTSSLCFG=0,0` | `<client>,<ssl_ctx>` | ผูก context 0 (ที่ตั้งในขั้น ③) เข้า client 0 |
| `AT+CMQTTCONNECT=0,"tcp://h:p",60,1,"u","p"` | `60` = keepalive (วินาที), `1` = clean session | **URL ต้องขึ้นต้น `tcp://` แม้ใช้ TLS** — เพราะ SSL ถูกกำหนดจาก ACCQ/SSLCFG แล้ว ถ้าใส่ `ssl://` หรือ `mqtts://` โมดูลจะปฏิเสธ |

#### ตารางเวลาและ error

| ขั้น | timeout | หมายเหตุ |
|---|---|---|
| NETOPEN | 8 วิ | ถ้าเปิดอยู่แล้วตอบ ERROR → ผ่านไปได้ |
| CSSLCFG × 4 | 2 วิ/คำสั่ง | เงียบ ๆ ไม่เช็กผล |
| CMQTTSTART | 5 วิ | ต้องได้ `+CMQTTSTART: 0` |
| CMQTTACCQ | 3 วิ | ต้องได้ `OK` |
| CMQTTSSLCFG | 2 วิ | ต้องได้ `OK` |
| **CMQTTCONNECT** | **30 วิ** | ต้องได้ `+CMQTTCONNECT: 0,0` (0 ตัวหลัง = สำเร็จ) |
| **รวม worst case** | **~55 วินาที** | loop จะค้างนานขนาดนี้ |

> `+CMQTTCONNECT: 0,<err>` — ตัวเลขหลังคือ error code (ไม่ใช่ HTTP) ความหมายอ้างอิงตารางใน SIMCom *A76XX Series_MQTT_Application_Note* ซึ่งรวมถึงข้อผิดพลาดด้าน network/DNS/TLS/auth โค้ดแค่ log ให้ดู ไม่ได้แปลงความหมาย

### 7.4 `publishLocation(lat, lng, status)` — `index.ino:430-457`

**ทำอะไร:** สร้าง JSON → ส่ง topic → ส่ง payload → สั่ง publish (QoS 1) → รอ confirmation

#### โฟลว

```
publishLocation(lat, lng, "active")
 │
 ① payload = snprintf("{...}", "{\"lat\":%.5f,\"lng\":%.5f,\"status\":\"%s\"}")
 │     ตัวอย่าง: {"lat":13.75630,"lng":100.50180,"status":"active"}
 │
 ② sendAT("AT+CMQTTTOPIC=0,<ความยาว topic>", 2000, ">")
 │     ไม่มี '>' ──► return false ❌
 ├  sendRaw(topic, 2000)                       ← ส่ง "/cars/<client-id>"
 │
 ③ sendAT("AT+CMQTTPAYLOAD=0,<ความยาว payload>", 2000, ">")
 │     ไม่มี '>' ──► return false ❌
 ├  sendRaw(payloadStr, 2000)                  ← ส่ง JSON
 │
 ④ pubRes = sendAT("AT+CMQTTPUB=0,1,60", 10000, "+CMQTTPUB:")
 │            │      │ │  │
 │            │      │ │  └─ publish timeout 60 วิ (ของโมดูล)
 │            │      │ └──── QoS 1 (at least once + PUBACK)
 │            │      └────── client 0
 │            └───────────── คำสั่ง publish
 │
 ⑤ ok = pubRes มี "+CMQTTPUB: 0,0" ?          ← 0 ตัวหลัง = สำเร็จ
 │     ├─ ok  ──► return true  ✅
 │     └─ !ok ──► mqttReady = false ; return false ❌  (ให้ loop reconnect)
```

| | |
|---|---|
| **ความยาว topic/payload** | ส่งเป็น **จำนวน byte** (ASCII ทั้งคู่จึงเท่ากับจำนวนตัวอักษร) — ต้องตรงเป๊ะ ไม่งั้นโมดูลรอต่อไม่ออก |
| **ความยาวสูงสุด** ของ `payload` | 128 byte (`char payload[128]`) — ปัจจุบันใช้ ~55 byte เหลือเผื่อ |
| **ผลข้างเคียงเมื่อล้มเหลว** | เซ็ต `mqttReady = false` → `loop()` จะเข้าเส้นทาง reconnect ในรอบตรวจถัดไป (ไม่เกิน 10 วิ) |
| **เวลาที่ใช้** | สำเร็จ ~200-500 ms / ล้มเหลว ~8-10 วิ |

---

## 8. กู้ระบบหลังโมดูลรีบูต

### 8.1 `recoverModem()` — `index.ino:401-425`

**ทำอะไร:** จัดการสถานะ "โมดูลบูตใหม่" (ไฟตก / brownout / watchdog ของโมดูลเอง) ซึ่งทำให้ **ทุกอย่างหายหมด**: PDP context, GNSS (ดับ), MQTT session

#### วิธีตรวจจับว่า "รีบูต"

โมดูล A7680C จะพ่น URC หลัง boot ทุกครั้ง:

| URC | ความหมาย |
|---|---|
| `*ATREADY` | **โมดูลพร้อมแล้ว (เพิ่งบูต)** ← สัญญาณหลัก |
| `SMS DONE` | อ่าน SIM/SMS เสร็จ (มาหลัง boot) |
| `PB DONE` | อ่าน phonebook เสร็จ (มาหลัง boot) |

#### โฟลว

```
recoverModem()
 │
 ├─ log "ตรวจพบโมดูลรีบูตเอง — กำลัง init ใหม่ทั้งหมด..."
 ├─ mqttReady = false ; gpsErrorCount = 0
 │
 ├─【1】รอ SIM พร้อม — AT+CPIN? วนสูงสุด 20 ครั้ง × (1.5s + 1s) ≈ 50 วิ
 │        indexOf("READY") เจอ → simReady = true, break
 │        log "[MODEM] SIM พร้อม" / "SIM ยังไม่พร้อม จะลองต่อ"
 │
 ├─【2】setupAPN()                      ← ตั้ง APN ใหม่ (ถ้ากำหนดไว้)
 │
 ├─【3】waitNetwork(60000)              ← รอลงทะเบียนเครือข่ายสูงสุด 60 วิ
 │
 ├─【4】enableGPS()                     ← เปิด GNSS ใหม่ + รอ READY
 │
 ├─【5】connectMQTT()                   ← ต่อ broker ใหม่ทั้งหมด
 │
 ├─ urcBuf = ""                         ← ⚠️ สำคัญ: ล้าง *ATREADY ที่ค้าง
 └─ log "กู้ระบบเสร็จ"
```

| | |
|---|---|
| **ถูกเรียกจาก** | ① `setup()` — ถ้าพบ `*ATREADY`/`SMS DONE` ใน `urcBuf` ระหว่าง init<br>② `loop()` — ทุกครั้งที่เจอ URC ใน `urcBuf` (นี่คือ path หลัก)<br>③ `loop()` — หลัง `hardResetModem()` (โมดูลค้าง) |
| **เวลาที่ใช้** | ~15-120 วินาที (ขึ้นกับว่าเน็ตกลับมาเร็วแค่ไหน) |
| **ทำไมต้องล้าง `urcBuf` ท้ายฟังก์ชัน** | ระหว่างกู้ระบบ โมดูลอาจส่ง `*ATREADY` มาอีก/หรือตัวเก่ายังค้าง → ถ้าไม่ล้าง `loop()` จะเห็นแล้วเรียก `recoverModem()` ซ้ำไม่รู้จบ (**recovery loop**) |

---

## 9. `setup()` / `loop()`

### 9.1 `setup()` — `index.ino:461-498`

**ทำอะไร:** ตั้ง hardware → เปิดโมดูล → รอเน็ต → เปิด GPS → ต่อ MQTT → ตรวจว่ามีรีบูตระหว่างทางไหม

```
setup()
 │
 ① Serial.begin(115200)                        ← พอร์ต debug (USB)
 ② SerialAT.setRxBufferSize(2048)              ← ★ ตั้งก่อน begin() เท่านั้น
 ③ SerialAT.begin(115200, 8N1, RX=16, TX=17)
 ④ LED = OUTPUT, LOW ; พิมพ์ banner
 │
 ⑤ powerOnModem()                              ← 0-20 วิ
 ⑥ ATE1 (echo on), AT+CPIN?
 ⑦ setupAPN()
 ⑧ waitNetwork(60000)                          ← สูงสุด 60 วิ
 │
 ⑨ pumpURC() ; urcBuf = ""                     ← ★ ทิ้ง URC ของ "การบูตปกติ"
 │                                                 (*ATREADY/SMS DONE ที่เกิดตอนเปิดเครื่อง
 │                                                  ไม่ใช่การรีบูตกลางทาง — ต้องล้างก่อน)
 ⑩ enableGPS()                                 ← รอ +CGNSSPWR: READY!
 ⑪ connectMQTT()                               ← สูงสุด ~55 วิ
 │
 ⑫ urcBuf มี "*ATREADY" หรือ "SMS DONE"?
 │     ──yes──► urcBuf="" ; recoverModem()      ← ★ รีบูตกลางทาง (เช่นตอน TLS handshake
 │                                                  กินไฟกระชากจน brownout)
 ⑬ urcBuf = ""
```

**ลำดับเวลาจริง (worst case)**

```
0s ─── powerOnModem ─────────────────────────── ~20s
      │
      ├─ CPIN/APN ─── 2s
      ├─ waitNetwork ──────────────────────────── 60s
      ├─ enableGPS ────────────────────────────── 18s
      ├─ connectMQTT ──────────────────────────── 55s
      └─ recoverModem (ถ้าจำเป็น) ─────────────── 120s
                                              รวมได้ถึง ~4 นาที
```

> **ทำไมขั้น ⑨ ต้องล้าง `urcBuf`:** ตอนเปิดเครื่องปกติ A7680C ก็ส่ง `*ATREADY` มาเหมือนกัน ถ้าไม่ล้าง ณ จุดนี้ `loop()` จะเข้าใจผิดว่า "เพิ่งรีบูต" แล้วเรียก `recoverModem()` ทันทีทั้งที่เพิ่ง init เสร็จ (= ทำงานซ้ำซ้อน 2-4 นาที)

### 9.2 `loop()` — `index.ino:500-570`

**ทำอะไร:** 3 งาน อิสระกัน ทำงานตามจังหวะเวลาของตัวเอง (ใช้ `static` + `millis()` แทน `delay()`)

```
loop()
 │
 ├──【งาน A】ทุก 10 วินาที : health check
 │   ├─ modemAlive()?
 │   │    no  → mqttReady=false ; noRespCount++
 │   │          └─ noRespCount >= 3 ? ──► noRespCount=0 ;
 │   │                                     hardResetModem() ; urcBuf="" ; recoverModem()
 │   │    yes → noRespCount = 0
 │   │          ├─ !isNetworkConnected() ──► log "หลุดเครือข่าย!" ; mqttReady=false
 │   │          └─ !isMQTTConnected()    ──► ถ้า millis() >= nextMqttTry:
 │   │                                        connectMQTT()
 │   │                                          ├─ สำเร็จ ──► nextMqttTry = 0
 │   │                                          └─ ล้มเหลว ─► nextMqttTry = millis()+15000 (backoff)
 │   │          └─ (ทั้งคู่ผ่าน) ──────────► log "[OK] Network และ MQTT พร้อม"
 │   │
 ├──【งาน B】ทุก 1 วินาที : อ่าน GPS + publish
 │   ├─ !mqttReady ? ──► log "ยังไม่ได้ connect, ข้าม publish รอบนี้"
 │   └─ mqttReady ──► getGPS(lat,lng)?
 │                     ├─ true  ──► LED HIGH ; publishLocation(lat,lng,"active") ; log ok/fail
 │                     └─ false ──► LED LOW
 │
 ├──【งาน C】ทุก loop iteration : URC handler
 │   ├─ pumpURC()                                 ← ดึง byte ค้างเข้า urcBuf
 │   ├─ urcBuf มี "*ATREADY" หรือ "SMS DONE" ?
 │   │     └─ yes ──► urcBuf="" ; recoverModem()  (โมดูลรีบูต → กู้ทั้งระบบ)
 │   └─ urcBuf มี "+CMQTTCONNLOST" หรือ "+CMQTTNONET" ?
 │         └─ yes ──► urcBuf="" (consume!) ; mqttReady = false
 │
 └─ (วนกลับ)
```

#### 4 ตัวแปร `static` ที่จำสถานะข้ามรอบ

| ตัวแปร | ค่าเริ่ม | ทำหน้าที่ |
|---|---|---|
| `lastPub` | 0 | timestamp ของ publish ล่าสุด → ทุก 1 วิ |
| `lastCheck` | 0 | timestamp ของ health check ล่าสุด → ทุก 10 วิ |
| `nextMqttTry` | 0 | เวลาที่ยอมให้ลอง reconnect MQTT ครั้งถัดไป (**backoff 15 วิ**) |
| `noRespCount` | 0 | นับรอบ health check ที่โมดูลไม่ตอบติดกัน (≥3 → hard reset) |

#### ตรรกะของ LED (GPIO2)

| สถานะ | LED |
|---|---|
| `getGPS()` สำเร็จ (ไม่ว่าจะ publish สำเร็จหรือไม่) | **HIGH (ติด)** |
| `getGPS()` ล้มเหลว / `mqttReady == false` | **LOW (ดับ)** |

> หมายเหตุ: LED เป็นแบบ **latching** — ติดค้างไว้จนกว่าจะมีรอบที่ GPS ล้มเหลว จึงไม่ใช่ "LED กะพริบตอนส่งข้อมูล" แต่เป็น "ตัวชี้ว่าล่าสุดได้พิกัดไหม"

#### State machine ของทั้งระบบ

```
                    ┌────────────┐
       boot ───────►│  STARTING  │  setup(): modem / net / gps / mqtt
                    └─────┬──────┘
                          │
                    ┌─────▼──────────────┐
       ┌───────────►│      NORMAL        │  publish ทุก ~1s, LED ตาม GPS
       │            └──┬──────────┬──────┘
       │               │          │
       │  MQTT ต่อติด  │          │ +CMQTTCONNLOST / +CMQTTNONET  หรือ CONNECT? ล้ม
       │   (backoff)   │          ▼
       │            ┌──▼───────────────┐
       │            │   MQTT_DOWN      │  mqttReady=false, reconnect ทุก 15s
       │            └──┬───────────────┘
       │               │ CEREG/CREG หลุด
       │            ┌──▼───────────────┐
       │            │   NET_DOWN       │  mqttReady=false, รอสัญญาณกลับ
       │            └──┬───────────────┘
       │               │ AT ไม่ตอบ 3 รอบ (30 วิ)
       │            ┌──▼───────────────┐
       │            │  MODEM_HUNG      │  hardResetModem() → PWRKEY ปิด+เปิด
       │            └──┬───────────────┘
       │               │
       │            ┌──▼───────────────┐
       ├────────────│  MODEM_REBOOTED  │  พบ *ATREADY/SMS DONE
       │            │ (recoverModem)   │  CPIN → APN → NET → GPS → MQTT
       │            └──────────────────┘
       │
       └─── กลับ NORMAL

  แขนงแยก: GNSS ERROR 3 ครั้งติด ──► gpsPowerCycle() ──► กลับ loop ปกติ
```

---

## 10. ตาราง AT Command ทั้งหมด

| # | คำสั่ง | เรียกจาก | Timeout | หยุดเมื่อ | หมายเหตุ |
|---|---|---|---|---|---|
| 1 | `AT` | `modemAlive` | 1 s × 3 | `OK` | ping |
| 2 | `ATE1` | `setup` | 1 s | `OK` | เปิด echo |
| 3 | `AT+CPIN?` | `setup`, `recoverModem` | 2 s / 1.5 s | `OK` | ต้องมี `READY` |
| 4 | `AT+CGDCONT=1,"IP","<apn>"` | `setupAPN` | 2 s | `OK` | เฉพาะเมื่อมี `MODEM_APN` |
| 5 | `AT+CEREG?` | `isNetworkConnected` | 2 s | `OK` | LTE registration |
| 6 | `AT+CREG?` | `isNetworkConnected` | 2 s | `OK` | fallback |
| 7 | `AT+CGNSSPWR=1` | `enableGPS` | 3 s | `OK` | เปิด GNSS |
| 8 | `AT+CGNSSPWR?` | `gpsPowerCycle` | 2 s | `OK` | 0=ดับ 1=เปิด |
| 9 | `AT+CGNSSPWR=0` | `gpsPowerCycle` | 3 s | `OK` | ปิด GNSS |
| 10 | `AT+CGNSSINFO` | `getGPS` | 3 s | `OK` | ขอพิกัด |
| 11 | `AT+NETOPEN` | `connectMQTT` | 8 s | `+NETOPEN:` | เปิด PDP |
| 12 | `AT+CSSLCFG="sslversion",0,3` | `connectMQTT` | 2 s | `OK` | TLS 1.2 |
| 13 | `AT+CSSLCFG="authmode",0,0` | `connectMQTT` | 2 s | `OK` | **ไม่ตรวจ cert ⚠️** |
| 14 | `AT+CSSLCFG="enableSNI",0,1` | `connectMQTT` | 2 s | `OK` | จำเป็นกับ EMQX |
| 15 | `AT+CSSLCFG="ignorelocaltime",0,1` | `connectMQTT` | 2 s | `OK` | ข้ามการเช็กเวลา |
| 16 | `AT+CMQTTSTART` | `connectMQTT` | 5 s | `+CMQTTSTART:` | ต้องได้ `: 0` |
| 17 | `AT+CMQTTACCQ=0,"<id>",1` | `connectMQTT` | 3 s | `OK` | 1 = SSL |
| 18 | `AT+CMQTTSSLCFG=0,0` | `connectMQTT` | 2 s | `OK` | ผูก ctx→client |
| 19 | `AT+CMQTTCONNECT=0,"tcp://...",60,1,"u","p"` | `connectMQTT` | **30 s** | `+CMQTTCONNECT: 0,` | `,0` = สำเร็จ |
| 20 | `AT+CMQTTCONNECT?` | `isMQTTConnected` | 2 s | `OK` | สถานะ client |
| 21 | `AT+CMQTTTOPIC=0,<len>` | `publishLocation` | 2 s | `>` | ตามด้วย `sendRaw` |
| 22 | `AT+CMQTTPAYLOAD=0,<len>` | `publishLocation` | 2 s | `>` | ตามด้วย `sendRaw` |
| 23 | `AT+CMQTTPUB=0,1,60` | `publishLocation` | 10 s | `+CMQTTPUB:` | QoS 1 |
| 24 | `AT+CMQTTDISC=0,60` | `cleanupMQTT` | 3 s | `OK` | ERROR ได้ |
| 25 | `AT+CMQTTREL=0` | `cleanupMQTT` | 2 s | `OK` | คืน client |
| 26 | `AT+CMQTTSTOP` | `cleanupMQTT` | 3 s | `OK` | หยุด service |

### URC ที่โปรแกรมตรวจ

| URC | ตรวจจุด | แปลว่า | การตอบสนอง |
|---|---|---|---|
| `*ATREADY` | `urcBuf` (setup + loop) | โมดูลเพิ่งบูต | `recoverModem()` |
| `SMS DONE` | `urcBuf` (setup + loop) | บูตเสร็จขั้นอ่าน SIM | `recoverModem()` |
| `PB DONE` | ไม่ได้ตรวจ (ถูกทิ้งใน `urcBuf`) | อ่าน phonebook เสร็จ | — |
| `+CGNSSPWR: READY!` | `enableGPS` | GNSS พร้อม | ทำงานต่อ |
| `+CMQTTCONNLOST` | `urcBuf` (loop) | หลุด broker | `mqttReady=false` |
| `+CMQTTNONET` | `urcBuf` (loop) | ไม่มีเน็ตให้ MQTT | `mqttReady=false` |
| `+CMQTTCONNECT: 0,<err>` | `connectMQTT` | ผลการต่อ | 0 = สำเร็จ |
| `+CMQTTPUB: 0,0` | `publishLocation` | publish สำเร็จ | `true` |

---

## 11. จังหวะเวลา / Timeout / Effective rate

### 11.1 จังหวะของ `loop()`

| งาน | ช่วงเวลา | เวลาที่กินจริง |
|---|---|---|
| Health check | ทุก 10 วิ | ปกติ ~20 ms / แย่สุด ~35 วิ (ถ้า reconnect) |
| GPS + publish | ทุก 1 วิ | ปกติ 0.2-0.6 วิ / ข้ามถ้า `!mqttReady` |
| URC pump | ทุกรอบ | ไม่กี่ ms |

### 11.2 อัตราการส่งจริง (ทำไมไม่ใช่ 1 Hz)

`loop()` ถูกบล็อกด้วย `sendAT()` ทำให้รอบจริงยืดออก:

```
รอบ 1 publish ที่สำเร็จ =  AT+CGNSSINFO  ~0.2 s
                        +  AT+CMQTTTOPIC ~0.05 s + sendRaw
                        +  AT+CMQTTPAYLOAD ~0.05 s + sendRaw
                        +  AT+CMQTTPUB   ~0.2 s
                        ≈  0.5-0.8 วินาที
```

ในทางปฏิบัติจึงได้ราว **1.2-2 วินาที/รอบ** และเมื่อรวม health check ที่อาจกิน 2-4 วิ ทุก 10 วิ ก็จะยิ่งช้าลง → **อัตราจริงประมาณ 0.5-0.8 Hz** ไม่ใช่ 1 Hz

### 11.3 Timeout ที่ควรจูน

| ค่า | ตำแหน่ง | ถ้าปรับ |
|---|---|---|
| `lastPub > 1000` | `loop:541` | ลดความถี่ publish (ประหยัด data/ไฟ) |
| `lastCheck > 10000` | `loop:507` | ตรวจสุขภาพถี่ขึ้น/ช้าลง |
| `nextMqttTry + 15000` | `loop:532` | backoff reconnect MQTT |
| `noRespCount >= 3` | `loop:515` | ทนต่อการไม่ตอบก่อน hard reset |
| `gpsErrorCount >= 3` | `getGPS:241` | ทนต่อ GNSS error ก่อน power cycle |
| `15000` (รอ modem หลัง power on) | `powerOnModem:132` | ขึ้นกับฝั่งไฟ/โมดูล |
| `timeoutMs 30000` (CMQTTCONNECT) | `connectMQTT:384` | เน็ตช้า/สัญญาณอ่อน |

---

## 12. ข้อสังเกต จุดเสี่ยง และสิ่งที่ควรปรับปรุง

### 🔴 ความปลอดภัย (สำคัญ)

| # | ประเด็น | รายละเอียด | ข้อเสนอ |
|---|---|---|---|
| 1 | **`authmode = 0`** (ไม่ตรวจ certificate) | `AT+CSSLCFG="authmode",0,0` → TLS เข้ารหัสจริง แต่**ไม่ยืนยันตัว broker** → เสี่ยง MITM. คอมเมนต์ในโค้ดก็ยอมรับว่าใช้ตอนพัฒนา | production: โหลด CA ด้วย `AT+CCERTDOWN` แล้วใช้ `authmode=1` |
| 2 | **credential เป็น plaintext ใน `secret.h`** | ไฟล์ถูก `.gitignore` (`*secret.h`) ✅ แต่ยังอยู่ในเครื่อง/แชร์ได้ | ใช้ `build_flags` หรือ `-D` จาก CI, หมุน (rotate) รหัสเป็นระยะ |
| 3 | **หมายเหตุผิดใน `secret.h`** | คอมเมนต์หัวไฟล์เขียน "HiveMQ Cloud" แต่ URL จริงคือ **EMQX Cloud** (`emqxsl.com`) | แก้คอมเมนต์กันสับสน |
| 4 | **ไม่มี heartbeat/LWT** | ถ้าอุปกรณ์ตายเงียบ server ไม่รู้ | ตั้ง Will message ใน `AT+CMQTTCONNECT` (พารามิเตอร์เพิ่ม) หรือ topic heartbeat |

### 🟡 ความถูกต้อง / ความทนทาน

| # | ประเด็น | อธิบาย | ข้อเสนอ |
|---|---|---|---|
| 5 | **`loop()` เป็น blocking ทั้งหมด** | `connectMQTT()` กินได้ถึง 55 วิ, `recoverModem()` ถึง 2 นาที → ไม่มี publish เลยช่วงนั้น และถ้าเปิด Task WDT จะเสี่ยงรีบูต | ใช้ state machine แบบ non-blocking หรือรันใน task แยก + คั่นด้วย `vTaskDelay` |
| 6 | **`waitNetwork()` ไม่คืนผล** | คืน `void` → `setup()` ไม่รู้ว่าเน็ตไม่ติด แล้วไป `connectMQTT()` ที่ต้องล้มเหลวอยู่ดี (เสียเวลา ~55 วิ) | เปลี่ยนเป็น `bool` แล้วข้าม connect ถ้าไม่ติด |
| 7 | **`enableGPS()` return ทันทีเมื่อไม่เจอ OK** | ไม่รอ READY → `getGPS` ERROR 3 ครั้ง → `gpsPowerCycle` กู้ (อ้อม) | ลอง `AT+CGNSSPWR?` ก่อน แล้วค่อยตัดสิน |
| 8 | **`urcBuf` ไม่เคยถูกเคลียร์ในเส้นทางปกติ** | นอกจากจุดที่กำหนด (หลัง recovery / setup) ถ้า `*ATREADY` เก่าค้างนาน ๆ จะ trigger recovery ซ้ำ แต่โค้ดเคลียร์ทันทีหลังใช้จึงปลอดภัยในทางปฏิบัติ | ใช้ flag "consume แล้ว" หรือ timestamp |
| 9 | **`+CMQTTPUB:` อาจมาหลัง timeout** | `sendAT(..., 10000, "+CMQTTPUB:")` ถ้าโมดูลช้ากว่า 10 วิ จะอ่านไม่ทัน แล้วถือว่าล้มเหลว → reconnect ทั้งที่ publish อาจสำเร็จ | เพิ่ม timeout เป็น 15-20 วิ หรือ retry แบบไม่ต้องต่อใหม่ |
| 10 | **timer drift** | `lastPub = millis()` (ไม่ใช่ `+= 1000`) → เวลาสะสมเพี้ยนขึ้นเรื่อย ๆ ตามเวลาที่ใช้ในรอบ | ใช้ `lastPub += 1000` หรือ timer ที่แม่นกว่า |
| 11 | **ไม่มี offline buffering** | ถ้าเน็ตหลุด ข้อมูลพิกัดช่วงนั้นหายหมด | เก็บ queue ลง NVS/SPIFFS แล้วส่งย้อนหลัง |
| 12 | **`String` ใช้หนัก** | `resp`/`urcBuf`/`data` เป็น Arduino `String` → heap fragmentation เมื่อรันนานเป็นเดือน | พิจารณา `char[]`/`snprintf` หรือจอง buffer ไว้ล่วงหน้า |
| 13 | **`noRespCount` ไม่รีเซ็ตเมื่อ reflash/หรือหลัง recover** | ในโค้ดรีเซ็ตก่อน hard reset แล้ว แต่ไม่รีเซ็ตหลัง `recoverModem()` ที่มาจาก URC path — รอบถัดไปนับต่อจากค่าเดิม | เซ็ต `noRespCount = 0` ใน `recoverModem()` (แต่เป็น `static` ใน `loop` ทำได้ยาก — ควรย้ายเป็น global) |
| 14 | **LED semantics กำกวม** | ติดเมื่อ GPS ได้ ไม่ใช่ MQTT สำเร็จ ทำให้ debug ผิด | แยกสถานะ: กะพริบ = กำลังเชื่อม, ติดค้าง = ปกติ, ดับ = error |
| 15 | **ไม่มี `AT+CGNSSPWR?` verify หลัง enable** | เฟิร์มแวร์บางรุ่นตอบ `OK` แต่ GNSS ไม่ได้เปิดจริง | verify ด้วย `AT+CGNSSPWR?` ซ้ำหลังหน่วงเล็กน้อย |

### 🟢 จุดที่ทำได้ดี (ควรคงไว้)

- เก็บ URC ลง `urcBuf` **ทุก byte** → ไม่พลาดสัญญาณรีบูต (จุดแข็งที่สุดของโค้ดนี้)
- `powerOnModem()` เช็กก่อนกด PWRKEY → ไม่พลาดไปสั่งปิดโมเดลที่ทำงานอยู่
- `getGPS()` มี **7 ด่านตรวจ** ตั้งแต่ ERROR / empty fields / mode=0 / ช่วงค่า → ทนข้อมูลเพี้ยนได้ดี
- `gpsPowerCycle()` แยกกรณี "ดับ" กับ "ค้าง" → ไม่สั่งซ้ำโดยเปล่าประโยชน์
- `loop()` ใช้ `millis()` แทน `delay()` → 3 งานเดินขนานกันได้
- backoff 15 วิ ป้องกัน reconnect ถี่จนกินไฟ/เน็ต
- `secret.h` ถูก gitignore แยกออกจากโค้ด ✅

---

## ภาคผนวก: สรุปคำตอบ 1 บรรทัดต่อฟังก์ชัน

| ฟังก์ชัน | สรุปสั้น |
|---|---|
| `noteByte` | เก็บ 1 byte ลง resp + urcBuf + echo (จำกัด urcBuf 1024) |
| `pumpURC` | รีด byte ค้างจาก UART เข้า urcBuf |
| `sendAT` | ส่ง AT + รอคำตอบ (หยุดที่ `until`/`OK`/`ERROR`/timeout) |
| `sendRaw` | ส่งข้อมูลดิบหลัง prompt `>` (topic/payload) |
| `modemAlive` | ยิง `AT` 3 ครั้ง เช็กโมดูลมีชีวิต |
| `pwrkeyRelease` | คืนขา PWRKEY เป็น HIGH หรือ hi-Z ตาม config |
| `pwrkeyPulse` | กด PWRKEY 1 ครั้ง (toggle เปิด/ปิด) 4.6 วิ |
| `powerOnModem` | เปิดโมดูลโดยเช็กก่อนว่าเปิดอยู่แล้วหรือไม่ |
| `hardResetModem` | ปิด (PWRKEY) → รอ → เปิด สำหรับโมดูลค้าง |
| `setupAPN` | ตั้ง APN เฉพาะเมื่อกำหนด `MODEM_APN` |
| `regOk` | parse `stat` จาก `+CREG:/+CEREG:` (1 หรือ 5 = ติด) |
| `isNetworkConnected` | เช็ก CEREG ก่อน fallback CREG |
| `waitNetwork` | วนรอลงทะเบียนเครือข่ายจนครบ timeout |
| `enableGPS` | เปิด GNSS แล้วรอ URC `+CGNSSPWR: READY` |
| `gpsPowerCycle` | ซ่อม GNSS: ถ้าดับ → เปิด, ถ้าค้าง → ปิด+เปิด |
| `getGPS` | ขอ+parse `+CGNSSINFO` ผ่าน 7 ด่าน → lat/lng |
| `cleanupMQTT` | DISC → REL → STOP + `mqttReady=false` |
| `isMQTTConnected` | ถาม `+CMQTTCONNECT?` โดยเปิดประตูด้วย `mqttReady` |
| `connectMQTT` | ตั้ง TLS → START → ACCQ → SSLCFG → CONNECT (9 ขั้น) |
| `recoverModem` | กู้ทั้งระบบหลังโมดูลรีบูต: CPIN→APN→NET→GPS→MQTT |
| `publishLocation` | สร้าง JSON → TOPIC → PAYLOAD → PUB QoS1 → รอผล |
| `setup` | init hardware + เปิดโมดูล + เน็ต + GPS + MQTT |
| `loop` | 3 งาน: health 10s / publish 1s / URC handler ทุกรอบ |
