// ==== NUBWO X-Series → ESP32 + L298N Car Control (with Serial debug) ====
// Wiring (L298N IN pins):
//   IN1 = GPIO4   (Motor A dir1)
//   IN2 = GPIO25  (Motor A dir2)
//   IN3 = GPIO26  (Motor B dir1)
//   IN4 = GPIO27  (Motor B dir2)
 
#include <Arduino.h>
#include <Bluepad32.h>
 
#ifndef DPAD_UP
  #define DPAD_UP    0x01
  #define DPAD_DOWN  0x02
  #define DPAD_LEFT  0x04
  #define DPAD_RIGHT 0x08
#endif
 
// ---------------- Pin mapping ----------------
#define IN1 4
#define IN2 25
#define IN3 26
#define IN4 27
 
// ---- Control & debug params ----
const uint32_t NO_INPUT_TIMEOUT_MS = 400; // เผื่อจอยหายไป → หยุด
const int AXIS_DEADZONE = 120;            // สำหรับแกนซ้าย (~ -512..512)
const uint32_t PRINT_EVERY_MS = 80;       // พิมพ์สถานะจอยทุก 80 ms
 
ControllerPtr myControllers[BP32_MAX_GAMEPADS];
uint32_t lastInputMs = 0;
uint32_t lastPrintMs = 0;
 
// ---------------- Motor helpers ----------------
static inline void motorStop() {
  digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
  digitalWrite(IN3, LOW); digitalWrite(IN4, LOW);
}
static inline void motorForward() {       // วิ่งตรงไป
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);   // A forward
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);   // B forward
}
static inline void motorBackward() {      // วิ่งถอยหลัง
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);  // A backward
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);  // B backward
}
static inline void motorLeft() {          // หมุนซ้ายอยู่กับที่ (A ถอย, B เดิน)
  digitalWrite(IN1, LOW);  digitalWrite(IN2, HIGH);  // A backward
  digitalWrite(IN3, HIGH); digitalWrite(IN4, LOW);   // B forward
}
static inline void motorRight() {         // หมุนขวาอยู่กับที่ (A เดิน, B ถอย)
  digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);   // A forward
  digitalWrite(IN3, LOW);  digitalWrite(IN4, HIGH);  // B backward
}
 
// ---------------- Debug print ----------------
void printGamepadState(ControllerPtr ctl) {
  uint32_t now = millis();
  if (now - lastPrintMs < PRINT_EVERY_MS) return; // rate-limit
  lastPrintMs = now;
 
  Serial.printf(
    "idx=%d | dpad=0x%02X btn=0x%04X | LX=%4d LY=%4d | RX=%4d RY=%4d | A=%d B=%d X=%d Y=%d\n",
    ctl->index(),
    ctl->dpad(),
    ctl->buttons(),
    ctl->axisX(), ctl->axisY(),
    ctl->axisRX(), ctl->axisRY(),
    ctl->a(), ctl->b(), ctl->x(), ctl->y()
  );
}
 
// ---------------- Input helpers ----------------
bool anyActiveInput(ControllerPtr ctl) {
  // มี D-Pad / ปุ่ม / แกนซ้ายเกิน deadzone ถือว่า "มีอินพุต"
  if (ctl->dpad() != 0) return true;
  if (ctl->a() || ctl->b() || ctl->x() || ctl->y() || ctl->l1() || ctl->r1() || ctl->l2() || ctl->r2()) return true;
  if (abs(ctl->axisX()) > AXIS_DEADZONE) return true;
  if (abs(ctl->axisY()) > AXIS_DEADZONE) return true;
  return false;
}
 
void driveFromInput(ControllerPtr ctl) {
  // เรียงลำดับ: D-Pad > ปุ่ม > แกนซ้าย
  uint8_t d = ctl->dpad();
  if (d) {
    if (d & DPAD_UP)    { motorForward();  lastInputMs = millis(); Serial.println("[CMD] FORWARD (DPAD)");  return; }
    if (d & DPAD_DOWN)  { motorBackward(); lastInputMs = millis(); Serial.println("[CMD] BACKWARD (DPAD)"); return; }
    if (d & DPAD_LEFT)  { motorLeft();     lastInputMs = millis(); Serial.println("[CMD] LEFT (DPAD)");     return; }
    if (d & DPAD_RIGHT) { motorRight();    lastInputMs = millis(); Serial.println("[CMD] RIGHT (DPAD)");    return; }
  }
 
  if (ctl->y()) { motorForward();  lastInputMs = millis(); Serial.println("[CMD] FORWARD (Y)");  return; }
  if (ctl->a()) { motorBackward(); lastInputMs = millis(); Serial.println("[CMD] BACKWARD (A)"); return; }
  if (ctl->x()) { motorLeft();     lastInputMs = millis(); Serial.println("[CMD] LEFT (X)");     return; }
  if (ctl->b()) { motorRight();    lastInputMs = millis(); Serial.println("[CMD] RIGHT (B)");    return; }
 
  int lx = ctl->axisX();  // ~ -512..512
  int ly = ctl->axisY();
  if (abs(ly) > abs(lx)) {
    if (ly < -AXIS_DEADZONE)     { motorForward();  lastInputMs = millis(); Serial.println("[CMD] FORWARD (LY)");  return; }
    else if (ly > AXIS_DEADZONE) { motorBackward(); lastInputMs = millis(); Serial.println("[CMD] BACKWARD (LY)"); return; }
  } else {
    if (lx < -AXIS_DEADZONE)     { motorLeft();  lastInputMs = millis(); Serial.println("[CMD] LEFT (LX)");  return; }
    else if (lx > AXIS_DEADZONE) { motorRight(); lastInputMs = millis(); Serial.println("[CMD] RIGHT (LX)"); return; }
  }
 
  // ถ้าเข้ามาถึงตรงนี้ = ไม่มีอินพุตที่มีนัยสำคัญ → หยุด "ทันที"
  motorStop();
  Serial.println("[CMD] STOP (idle)");
}
 
void processControllers() {
  bool anyCtl = false;
  for (auto ctl : myControllers) {
    if (ctl && ctl->isConnected() && ctl->hasData()) {
      anyCtl = true;
      printGamepadState(ctl);
 
      if (anyActiveInput(ctl)) {
        driveFromInput(ctl);  // ขับตามอินพุต
      } else {
        motorStop();          // ไม่มีอินพุต → หยุดทันที
      }
    }
  }
 
  // failsafe เพิ่มเติม: ไม่มีจอย/ไม่มีอินพุตนานเกินกำหนด → หยุด
  if (!anyCtl || (millis() - lastInputMs > NO_INPUT_TIMEOUT_MS)) {
    motorStop();
  }
}
 
// ---------------- Bluepad32 callbacks ----------------
void onConnectedController(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == nullptr) {
      myControllers[i] = ctl;
      ControllerProperties p = ctl->getProperties();
      Serial.printf("Connected idx=%d | %s VID=0x%04x PID=0x%04x\n",
                    i, ctl->getModelName().c_str(), p.vendor_id, p.product_id);
      return;
    }
  }
  Serial.println("Connected but no free slot.");
}
void onDisconnectedController(ControllerPtr ctl) {
  for (int i = 0; i < BP32_MAX_GAMEPADS; i++) {
    if (myControllers[i] == ctl) {
      myControllers[i] = nullptr;
      Serial.printf("Disconnected idx=%d\n", i);
      motorStop();
      return;
    }
  }
  Serial.println("Disconnected, but not found.");
}
 
// ---------------- Arduino setup/loop ----------------
void setup() {
  Serial.begin(115200);
 
  pinMode(IN1, OUTPUT);
  pinMode(IN2, OUTPUT);
  pinMode(IN3, OUTPUT);
  pinMode(IN4, OUTPUT);
  motorStop();
 
  Serial.printf("Firmware: %s\n", BP32.firmwareVersion());
  const uint8_t* addr = BP32.localBdAddress();
  Serial.printf("BD Addr: %02X:%02X:%02X:%02X:%02X:%02X\n",
                addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
 
  BP32.setup(&onConnectedController, &onDisconnectedController);
  BP32.enableVirtualDevice(false);
  BP32.enableNewBluetoothConnections(true);
 
  // ถ้าจับคู่ติดๆดับๆ ค่อยเปิดบรรทัดนี้ "ครั้งเดียว" แล้วอัปโหลดใหม่ จากนั้นคอมเมนต์ทิ้ง
  // BP32.forgetBluetoothKeys();
 
  Serial.println("Ready. Open Serial Monitor @115200 and pair your NUBWO X.");
}
 
void loop() {
  bool updated = BP32.update();
  if (updated) processControllers();
  delay(10); // กัน watchdog
}
 
 