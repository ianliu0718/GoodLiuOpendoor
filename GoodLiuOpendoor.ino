// v1.1.6 - 修正：MQTT 連線問題
// ESP32 智慧門鎖主控程式
// 功能：指紋辨識、密碼開鎖、遠端HTTP開鎖
// 材料：ESP32 DEVKIT V1、AS608、4x4 Keypad、繼電器

#include <WiFi.h>
#include <WebServer.h>
#include <PubSubClient.h> // 新增 MQTT 支援
#include <ArduinoJson.h> // JSON 解析庫
#include <Keypad.h>
#include <Adafruit_Fingerprint.h>
#include <HardwareSerial.h>
#include <vector>
#include <ESP32Servo.h>
#include <MD_MAX72xx.h> // 使用 MD_MAX72XX 庫
#include <Preferences.h>
#include <time.h> // 新增 NTP 校時

// WiFi 設定
#define SSID_ADDR 0
#define PASS_ADDR 32
#define MAINPW_ADDR 64
//String sta_ssid = "goodliu-2.4G";
//String sta_password = "11111111";
String sta_ssid = "HiHiLive2323";
String sta_password = "ianian520";
bool inSetupMode = false;
WebServer server(80);

// MQTT 設定
const char* mqtt_server = "liunas2323.synology.me"; // 替換為您的 NAS IP
const int mqtt_port = 1883;
const char* mqtt_user = "x7814778r"; // MQTT 使用者名稱
const char* mqtt_pass = "19880718"; // MQTT 密碼
const char* mqtt_topic = "door/open";
const char* mqtt_topic_add_temp_pw = "door/add_temp_pw"; // 新增臨時密碼 topic
WiFiClient espClient;
PubSubClient client(espClient);

// 使用 Preferences 替代 EEPROM
Preferences preferences;

// 指紋模組設定 - 使用專用腳位，不共用
#define FINGER_RX 16  // GPIO 16
#define FINGER_TX 17  // GPIO 17
HardwareSerial fingerSerial(1); // 使用 UART2
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

bool inFingerRun = false;
bool fingerAvailable = false;
bool isShowingResult = false;

// 4x4 Keypad 設定 - 重新分配腳位，避免共用
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
// 新的腳位分配，根據您的要求反轉接線順序
// 排線 Pin 1-4 (Rows) -> ESP32 GPIO 13, 12, 14, 27
// 排線 Pin 5-8 (Cols) -> ESP32 GPIO 26, 25, 33, 32
byte rowPins[ROWS] = {13, 12, 14, 27}; // ESP32 腳位 (Rows)
byte colPins[COLS] = {26, 25, 33, 32}; // ESP32 腳位 (Cols)
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// 密碼設定
String correct_password = ""; // 主要預設密碼
String password_input = "";       //  程式修改後儲存密碼
String currentDisplayText = "";   //  程式比對用的密碼
unsigned long lastKeypadCheck = 0;
const unsigned long KEYPAD_CHECK_INTERVAL = 120; // 每120ms掃描一次

// SG90控制腳位
#define SERVO_PIN 5  // ESP32 腳位
Servo lockServo;

// 伺服馬達狀態控制
bool servoAttached = false;
unsigned long lastServoAction = 0;
const unsigned long SERVO_COOLDOWN = 2000; // 伺服馬達冷卻時間
int servoOpenAngle = 180; // 開門角度，可於網頁調整（0~180）

// 安全的伺服馬達控制函數
void safeServoAttach() {
  if (!servoAttached) {
    Serial.println("安全連接伺服馬達");
    lockServo.attach(SERVO_PIN, 500, 2400); // 修正脈衝寬度範圍，才能轉到180度
    servoAttached = true;
    delay(500); // 等待穩定
  }
}

void safeServoDetach() {
  if (servoAttached) {
    Serial.println("安全斷開伺服馬達");
    lockServo.detach();
    servoAttached = false;
    delay(200); // 等待完全斷開
  }
}

// ====== 臨時密碼結構 ======
struct TempPassword {
  String pw;
  unsigned long expireTime = 0; // 0=無期限
  int remainCount = -1; // -1=無次數限制
};
std::vector<TempPassword> tempPasswords;

// ====== 臨時密碼序列化/反序列化 ======
#define TEMP_PW_MAX 10
#define TEMP_PW_LEN 8
#define TEMP_PW_STRUCT_SIZE 14 // 8+4+2

void saveTempPasswordsToPreferences() {
  preferences.begin("lock", false);
  preferences.clear();
  
  for (int i = 0; i < tempPasswords.size() && i < TEMP_PW_MAX; i++) {
    TempPassword &tp = tempPasswords[i];
    String key = "tp" + String(i);
    
    // 儲存密碼
    preferences.putString((key + "pw").c_str(), tp.pw);
    // 儲存到期時間
    preferences.putULong((key + "exp").c_str(), tp.expireTime);
    // 儲存次數
    preferences.putInt((key + "cnt").c_str(), tp.remainCount);
  }
  
  // 儲存總數
  preferences.putInt("tp_count", tempPasswords.size());
  preferences.end();
}

void loadTempPasswordsFromPreferences() {
  tempPasswords.clear();
  preferences.begin("lock", true);
  
  int count = preferences.getInt("tp_count", 0);
  
  for (int i = 0; i < count && i < TEMP_PW_MAX; i++) {
    String key = "tp" + String(i);
    
    String pw = preferences.getString((key + "pw").c_str(), "");
    unsigned long expireTime = preferences.getULong((key + "exp").c_str(), 0);
    int remainCount = preferences.getInt((key + "cnt").c_str(), -1);
    
    // 檢查密碼是否有效
    if (pw.length() > 0 && pw.length() <= 8) {
      bool validPw = true;
      for (int j = 0; j < pw.length(); j++) {
        if (pw[j] < '0' || pw[j] > '9') {
          validPw = false;
          break;
        }
      }
      
      if (validPw) {
        TempPassword tp;
        tp.pw = pw;
        tp.expireTime = expireTime;
        tp.remainCount = remainCount;
        tempPasswords.push_back(tp);
      }
    }
  }
  
  preferences.end();
}

// ====== 工具函式 ======
unsigned long parseTime(String iso8601) {
  // 簡易解析格式: YYYY-MM-DDTHH:MM
  int y = iso8601.substring(0,4).toInt();
  int m = iso8601.substring(5,7).toInt();
  int d = iso8601.substring(8,10).toInt();
  int h = iso8601.substring(11,13).toInt();
  int min = iso8601.substring(14,16).toInt();
  tm t;
  t.tm_year = y-1900; t.tm_mon = m-1; t.tm_mday = d;
  t.tm_hour = h; t.tm_min = min; t.tm_sec = 0;
  unsigned long ts = mktime(&t);
  return ts;
}

// --- 這裡定義七段顯示器的字型表 ---
byte get7SegPattern(char c) {
  switch (c) {
    // --- 數字 0-9 (你提供的正確數值) ---
    case '0': return 0x7E;
    case '1': return 0x0C;
    case '2': return 0xB6;
    case '3': return 0x9E;
    case '4': return 0xCC;
    case '5': return 0xDA;
    case '6': return 0xFA;
    case '7': return 0x4E; // 帶鉤的7
    case '8': return 0xFE;
    case '9': return 0xDE;

    // --- 字母 (A-Z / a-z) ---
    // 七段顯示器無法完美顯示所有字母，這裡採用最容易辨識的混和寫法
    
    case 'A': case 'a': return 0xEE; // A (大寫)
    case 'B': case 'b': return 0xF8; // b (小寫)
    case 'C':           return 0x72; // C (大寫)
    case 'c':           return 0xB0; // c (小寫)
    case 'D': case 'd': return 0xBC; // d (小寫)
    case 'E': case 'e': return 0xF2; // E (大寫)
    case 'F': case 'f': return 0xE2; // F (大寫)
    case 'G':           return 0x7A; // G (大寫)
    case 'g':           return 0xDE; // g (小寫)
    case 'H':           return 0xEC; // H (大寫)
    case 'h':           return 0xE8; // h (小寫)
    case 'I': case 'i': return 0x0A; // i (小寫)
    case 'J': case 'j': return 0x1A; // j (小寫)
    case 'K': case 'k': return 0xEA; // k (小寫)
    case 'L': case 'l': return 0x70; // L (大寫)
    case 'M': case 'm': return 0xAA; // M (大寫)
    case 'N': case 'n': return 0xA8; // n (小寫)
    case 'O': case 'o': return 0xB8; // o (小寫) -> 若用大寫會跟 0 混淆
    case 'P': case 'p': return 0xE6; // P (大寫)
    case 'Q': case 'q': return 0xCE; // q (小寫)
    case 'R': case 'r': return 0xA0; // r (小寫)
    case 'S': case 's': return 0xDA; // S (顯示為 5)
    case 'T': case 't': return 0xF0; // t (小寫)
    case 'U': case 'u': return 0x38; // u (小寫)
    case 'V': case 'v': return 0x55; // V (大寫)
    case 'W': case 'w': return 0xD4; // W (大寫)
    case 'X': case 'x': return 0x28; // X (大寫)
    case 'Y': case 'y': return 0xDC; // y (小寫)
    case 'Z': case 'z': return 0x36; // Z (大寫)

    // --- 特殊符號 ---
    case '-': return 0x80; // 中間橫線 (G段)
    case '_': return 0x10; // 底部橫線 (D段)
    case '=': return 0x90; // 等號 (G+D)
    case ' ': return 0x00; // 空白
    case '.': return 0x01; // 小數點 (Bit 0)
    case '°': return 0xC6; // 度 (例如溫度) A+B+F+G
    
    default: return 0x00; // 未定義字元不顯示
  }
}

// MAX7219 腳位對應 - 專用腳位，不共用
// 腳位已更換，以釋放連續腳位給鍵盤使用
#define DATA_PIN 18
#define CLK_PIN  19
#define CS_PIN   21

// 創建 MD_MAX72XX 實例 - 嘗試 GENERIC_HW 類型
#define MAX_DEVICES 1 // MAX7219 模組數量
MD_MAX72XX mx = MD_MAX72XX(MD_MAX72XX::GENERIC_HW, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES);

// 初始化 MAX7219
void initMax7219() {
  Serial.println("初始化 MAX7219...");
  Serial.println("注意: MAX7219 通常需要 5V 電源才能正常工作！");
  Serial.println("如果使用 3.3V，可能無法正常顯示。");
  Serial.print(", DATA=");
  Serial.print(DATA_PIN);
  Serial.print("腳位設定: CLK=");
  Serial.print(CLK_PIN);
  Serial.print(", CS=");
  Serial.println(CS_PIN);
  
  // 設定 MAX7219 腳位為輸出
  pinMode(DATA_PIN, OUTPUT);
  pinMode(CLK_PIN, OUTPUT);
  pinMode(CS_PIN, OUTPUT);
  
  // 啟動 MAX7219 模組
  mx.begin();
  
  // 設定亮度 0~15
  mx.control(MD_MAX72XX::INTENSITY, 1);
  
  // 清除顯示
  mx.clear();
  
  delay(500);
  
  Serial.println("MAX7219 初始化完成");
}

// 顯示
void showTextOnLed(String text, int delayTime = 700) {
  Serial.println("showTextOnLed: " + text);
  mx.clear();
  
  int len = text.length();
  for (int i = 0; i < 8; i++) {
    mx.setColumn(i, get7SegPattern(text[len-i-1])); 
  }
  
  delay(delayTime);
}

// 顯示板號（支援小數點）
void showversion(String text, int delayTime = 700) {
  Serial.println("showVersion: " + text);
  mx.clear();
  
  // 先移除小數點，並記錄哪些位置後面有小數點
  String cleanText = "";
  bool dotAfter[16] = {false};  // 記錄每個字元後是否有小數點
  
  for (int i = 0; i < text.length(); i++) {
    if (text[i] == '.') {
      // 小數點：標記前一個字元需要加上小數點
      if (cleanText.length() > 0) {
        dotAfter[cleanText.length() - 1] = true;
      }
    } else {
      cleanText += text[i];
    }
  }
  
  // 從右到左顯示，最多8個位數
  int len = cleanText.length();
  int displayCount = min(len, 8);
  
  for (int i = 0; i < displayCount; i++) {
    int charIndex = len - 1 - i;  // 從最後一個字元開始
    byte pattern = get7SegPattern(cleanText[charIndex]);
    
    // 如果該字元後面有小數點，加上小數點 bit (0x01)
    if (dotAfter[charIndex]) {
      pattern |= 0x01;
    }
    
    mx.setColumn(i, pattern);
  }
  
  delay(delayTime);
}

// 顯示密碼在 MAX7219（右對齊，最多8碼）
void showPasswordOnLed(String pw) {
  Serial.println("密碼輸入: " + pw);
  if (pw == currentDisplayText) return; // 避免重複刷新
  currentDisplayText = pw;
  showTextOnLed(pw);
}

// 顯示錯誤編號在 MAX7219（右對齊，最多4碼）
void showErrorNoOnLed(int no) {
  Serial.println("錯誤編號: " + String(no));
  showTextOnLed(" Err" + String(no), 2000);
  clearDisplay();
}

// 顯示結果（O 或 E）只支援 0-9、E、H、L、P、-、空白等）
void showResult(char result) {
  Serial.println("結果: " + String(result));
  if (result == 'E') {
    showTextOnLed("Error", 2000);
  } else {
    showTextOnLed(String(result), 2000);
  }
  clearDisplay();
}

// 清空顯示
void clearDisplay() {
  mx.clear();
}

// 檢查按鍵
char checkKeypadInput() {
  char key = keypad.getKey();
  return key;
}

// ====== 指紋機相關函式 ======

// 初始化指紋機
void initFingerprint() {
  Serial.println("初始化指紋機...");
  
  // 初始化指紋偵測為關閉
  inFingerRun = false;
  
  // 初始化指紋機通訊
  fingerSerial.begin(57600, SERIAL_8N1, FINGER_RX, FINGER_TX);
  delay(1000);
  
  if (finger.verifyPassword()) {
    Serial.println("指紋機連接成功");
    fingerAvailable = true;
  } else {
    Serial.println("指紋機連接失敗");
    fingerAvailable = false;
  }
}

// 指紋辨識流程
void checkFingerprint() {
  if (!fingerAvailable) return;
  
  finger.LEDcontrol(true);
  
  // 顯示指紋辨識狀態，短暫顯示以提示使用者可以掃描
  showTextOnLed("FP", 100);  // 顯示 FP 100ms，提示可以掃描
  
  // 等待手指放置
  int result = finger.getImage();
  if (result == FINGERPRINT_NOFINGER) {
    // 沒有手指，關閉 LED 並返回
    finger.LEDcontrol(false);
    return;
  }
  
  if (result == FINGERPRINT_OK) {
    // 顯示處理中
    showTextOnLed("Proc", 500);
    
    if (finger.image2Tz() == FINGERPRINT_OK) {
      result = finger.fingerSearch();
      if (result == FINGERPRINT_OK) {
        Serial.println("指紋辨識通過，開門");
        finger.LEDcontrol(false);
        showResult('O'); // 顯示成功
        openLock();
        delay(2000); // 防止重複觸發
      } else {
        Serial.println("指紋不符");
        finger.LEDcontrol(false);
        showErrorNoOnLed(11); // 顯示錯誤
      }
    } else {
      Serial.println("指紋圖像處理失敗");
      finger.LEDcontrol(false);
      showErrorNoOnLed(12); // 顯示錯誤
    }
  } else {
    Serial.println("指紋圖像獲取失敗");
    finger.LEDcontrol(false);
    showErrorNoOnLed(13); // 顯示錯誤
  }
  
  // 關閉指紋機
  inFingerRun = false;
  showTextOnLed("FPOFF", 1000);
  clearDisplay();
}

// 指紋註冊函式
bool enrollFingerprint(int id) {
  if (!fingerAvailable) return false;
  // 顯示註冊狀態
  showTextOnLed("Enroll", 2000);
  
  finger.LEDcontrol(true);
  Serial.print("請放上手指...");
  
  if (finger.getImage() != FINGERPRINT_OK) {
    return false;
  }
  
  if (finger.image2Tz(1) != FINGERPRINT_OK) {
    return false;
  }
  
  Serial.print("請再次放上手指...");
  delay(2000);
  
  if (finger.getImage() != FINGERPRINT_OK) {
    return false;
  }
  
  if (finger.image2Tz(2) != FINGERPRINT_OK) {
    return false;
  }
  
  if (finger.createModel() != FINGERPRINT_OK) {
    return false;
  }
  
  if (finger.storeModel(id) != FINGERPRINT_OK) {
    return false;
  }
  
  // 保存 ID 與名稱
  addFingerprintId(id);
  if (!fingerprintNameExists(id)) {
    setFingerprintName(id, String("User") + String(id));
  }
  
  finger.LEDcontrol(false);
  
  // 顯示成功
  showResult('O');
  return true;
}

// 指紋刪除函式
bool deleteFingerprint(int id) {
  if (!fingerAvailable) return false;
  
  bool result = finger.deleteModel(id) == FINGERPRINT_OK;
  
  if (result) {
    removeFingerprintId(id);
    deleteFingerprintName(id);
    showResult('O');
  } else {
    showResult('E');
  }
  
  return result;
}

// Preferences 相關函式
void writeStringToPreferences(const String& key, const String& value) {
  preferences.begin("lock", false);
  preferences.putString(key.c_str(), value);
  preferences.end();
}

String readStringFromPreferences(const String& key) {
  preferences.begin("lock", true);
  String value = preferences.getString(key.c_str(), "");
  preferences.end();
  return value;
}

void writeIntToPreferences(const String& key, int value) {
  preferences.begin("lock", false);
  preferences.putInt(key.c_str(), value);
  preferences.end();
}

int readIntFromPreferences(const String& key, int def) {
  preferences.begin("lock", true);
  int v = preferences.getInt(key.c_str(), def);
  preferences.end();
  return v;
}

// WiFi 連線與AP模式切換
void connectToWiFi() {
  sta_ssid.trim();
  sta_password.trim();
  Serial.print("連線SSID: [");
  Serial.print(sta_ssid.c_str());
  Serial.println("]");
  Serial.print("密碼: [");
  Serial.print(sta_password.c_str());
  Serial.println("]");
  WiFi.begin(sta_ssid.c_str(), sta_password.c_str());
  int retries = 0;
  String conStr = "Con";
  while (WiFi.status() != WL_CONNECTED && retries < 5) {
    conStr +=  ".";
    showTextOnLed(conStr, 1000);
    Serial.print(".");
    delay(6000);
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi 連線成功");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    IPAddress ip = WiFi.localIP();
    String ipArr[4];
    ipArr[0] = String(ip[0]);
    ipArr[1] = String(ip[1]);
    ipArr[2] = String(ip[2]);
    ipArr[3] = String(ip[3]);
    String group1 = ipArr[0];
    String group2 = ipArr[1];
    group2 = String("    ").substring(0, 4 - group2.length()) + group2;  // 左補空格
    showTextOnLed(group1 + group2, 1500);
    group1 = ipArr[2];
    group2 = ipArr[3];
    group2 = String("    ").substring(0, 4 - group2.length()) + group2;  // 左補空格
    showTextOnLed(group1 + group2, 3000);
  } else {
    Serial.println("\nWiFi 連線失敗，啟動AP模式");
    inSetupMode = true;
    WiFi.softAP("Lock_Setup_AP", "password");
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    showTextOnLed("AP     ", 3000);
    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP模式啟動，請連接WiFi並設定，AP IP: ");
    Serial.println(apIP);
    // 顯示IP於MAX7219
    String ipStr = apIP.toString();
    int firstDot = ipStr.lastIndexOf('.', ipStr.lastIndexOf('.') - 1);
    String last2 = ipStr.substring(firstDot + 1); // 取最後兩組
    int dotPos = last2.indexOf('.');
    String group1 = last2.substring(0, dotPos);   // 倒數第2組
    String group2 = last2.substring(dotPos + 1);  // 倒數第1組
    group2 = String("    ").substring(0, 4 - group2.length()) + group2;  // 左補空格
    showTextOnLed(group1 + group2, 3000);
  }
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>WiFi設定</title></head><body>";
  html += "<form method='post' action='/save'>WiFi SSID:<input name='ssid'><br>密碼:<input name='pass' type='password'><br><button type='submit'>儲存</button></form>";
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}

void handleSave() {
  writeStringToPreferences("ssid", server.arg("ssid"));
  writeStringToPreferences("pass", server.arg("pass"));
  server.send(200, "text/plain", "設定已儲存，將重啟...");
  delay(1000);
  ESP.restart();
}

// ====== 指紋名單保存/載入（Preferences） ======
String getFingerprintIdsCsv() {
  preferences.begin("lock", true);
  String csv = preferences.getString("fp_ids", "");
  preferences.end();
  return csv;
}

void setFingerprintIdsCsv(const String &csv) {
  preferences.begin("lock", false);
  preferences.putString("fp_ids", csv);
  preferences.end();
}

std::vector<int> parseIdsCsv(const String &csv) {
  std::vector<int> ids;
  String s = csv + ",";
  int start = 0;
  while (true) {
    int idx = s.indexOf(',', start);
    if (idx == -1) break;
    String token = s.substring(start, idx);
    token.trim();
    if (token.length() > 0) ids.push_back(token.toInt());
    start = idx + 1;
  }
  return ids;
}

String buildIdsCsv(const std::vector<int> &ids) {
  String out = "";
  for (size_t i = 0; i < ids.size(); i++) {
    if (i > 0) out += ",";
    out += String(ids[i]);
  }
  return out;
}

bool addFingerprintId(int id) {
  std::vector<int> ids = parseIdsCsv(getFingerprintIdsCsv());
  for (int v : ids) if (v == id) return false;
  ids.push_back(id);
  setFingerprintIdsCsv(buildIdsCsv(ids));
  return true;
}

bool removeFingerprintId(int id) {
  std::vector<int> ids = parseIdsCsv(getFingerprintIdsCsv());
  bool removed = false;
  for (auto it = ids.begin(); it != ids.end(); ) {
    if (*it == id) { it = ids.erase(it); removed = true; }
    else ++it;
  }
  if (removed) setFingerprintIdsCsv(buildIdsCsv(ids));
  return removed;
}

String getFingerprintName(int id) {
  String key = String("fp_name_") + String(id);
  preferences.begin("lock", true);
  String name = preferences.getString(key.c_str(), (String("User") + String(id)).c_str());
  preferences.end();
  return name;
}

void setFingerprintName(int id, const String &name) {
  String key = String("fp_name_") + String(id);
  preferences.begin("lock", false);
  preferences.putString(key.c_str(), name);
  preferences.end();
}

void deleteFingerprintName(int id) {
  String key = String("fp_name_") + String(id);
  preferences.begin("lock", false);
  preferences.remove(key.c_str());
  preferences.end();
}

// ====== 指紋名單保存/載入（Preferences）結束 ======

bool fingerprintNameExists(int id) {
  String key = String("fp_name_") + String(id);
  preferences.begin("lock", true);
  bool exists = preferences.isKey(key.c_str());
  preferences.end();
  return exists;
}


// 通用的添加臨時密碼函數（供 HTTP 和 MQTT 使用）
// 返回: 0=成功, 1=已達上限, 2=缺少密碼, 3=密碼格式錯誤, 4=時間格式錯誤
int addTempPassword(String pw, String expireStr = "", int count = -1) {
  // 檢查是否已達上限
  if (tempPasswords.size() >= TEMP_PW_MAX) {
    Serial.println("臨時密碼已達上限");
    showErrorNoOnLed(3);
    return 1;
  }
  
  // 檢查密碼是否存在
  if (pw.length() == 0) {
    Serial.println("缺少密碼");
    showErrorNoOnLed(4);
    return 2;
  }
  
  // 檢查密碼格式（只能包含數字，長度1-8）
  if (pw.length() > 8) {
    Serial.println("密碼長度超過8位");
    showErrorNoOnLed(14);
    return 3;
  }
  
  for (int i = 0; i < pw.length(); i++) {
    if (pw[i] < '0' || pw[i] > '9') {
      Serial.println("密碼只能包含數字");
      showErrorNoOnLed(5);
      return 3;
    }
  }
  
  // 創建臨時密碼結構
  TempPassword tp;
  tp.pw = pw;
  tp.remainCount = count;
  
  // 解析到期時間
  if (expireStr.length() > 0) {
    // 驗證時間格式: YYYY-MM-DDTHH:MM (至少16個字符)
    if (expireStr.length() < 16) {
      Serial.println("時間格式錯誤: 長度不足");
      showErrorNoOnLed(15);
      return 4;
    }
    // 檢查基本格式
    if (expireStr.charAt(4) != '-' || expireStr.charAt(7) != '-' || 
        expireStr.charAt(10) != 'T' || expireStr.charAt(13) != ':') {
      Serial.println("時間格式錯誤: 格式不符");
      showErrorNoOnLed(18);
      return 4;
    }
    tp.expireTime = parseTime(expireStr);
    // 檢查解析後的時間是否合理（應該大於當前時間或接近）
    unsigned long now = getNow();
    if (tp.expireTime < now - 86400) { // 如果時間比現在早超過1天，可能是解析錯誤
      Serial.println("時間格式錯誤: 時間過早");
      showErrorNoOnLed(19);
      return 4;
    }
  } else {
    tp.expireTime = 0; // 無期限
  }
  
  // 添加到列表
  tempPasswords.push_back(tp);
  saveTempPasswordsToPreferences();
  
  Serial.print("成功添加臨時密碼: ");
  Serial.print(pw);
  if (expireStr.length() > 0) {
    Serial.print(", 到期時間: ");
    Serial.print(expireStr);
  }
  if (count >= 0) {
    Serial.print(", 剩餘次數: ");
    Serial.print(count);
  }
  Serial.println();
  
  return 0; // 成功
}

void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 智慧門鎖 v1.1.6");
  
  Serial.println("初始化伺服馬達...");
  safeServoAttach();
  lockServo.write(45);
  delay(1000);
  lockServo.write(0);   // 轉回0度（關鎖）
  delay(1000);
  safeServoDetach();

  // 初始化 MAX7219
  initMax7219();
  
  // 簡單測試 MAX7219 是否工作
  showversion("v1.1.6", 1000);
  clearDisplay();
  
  // 增加按鍵去抖動時間，嘗試解決鬼鍵問題
  keypad.setDebounceTime(50); // 預設是 10ms，我們增加到 50ms

  // 初始化指紋機
  initFingerprint();
  
  sta_ssid = readStringFromPreferences("ssid");
  sta_password = readStringFromPreferences("pass");
  correct_password = readStringFromPreferences("mainpw");
  servoOpenAngle = readIntFromPreferences("servoAngle", 180);
  if (correct_password.length() == 0) correct_password = "1234";
  Serial.println(correct_password);
  
  if (sta_ssid.length() > 0) {
    connectToWiFi();
  } else {
    inSetupMode = true;
    WiFi.softAP("Lock_Setup_AP", "password");
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    showTextOnLed("AP     ", 3000);
    IPAddress apIP = WiFi.softAPIP();
    Serial.print("AP模式啟動，請連接WiFi並設定，AP IP: ");
    Serial.println(apIP);
    // 顯示IP於MAX7219
    String ipStr = apIP.toString();
    int firstDot = ipStr.lastIndexOf('.', ipStr.lastIndexOf('.') - 1);
    String last2 = ipStr.substring(firstDot + 1); // 取最後兩組
    int dotPos = last2.indexOf('.');
    String group1 = last2.substring(0, dotPos);   // 倒數第2組
    String group2 = last2.substring(dotPos + 1);  // 倒數第1組
    group2 = String("    ").substring(0, 4 - group2.length()) + group2;  // 左補空格
    showTextOnLed(group1 + group2, 3000);
  }
  
  if (inSetupMode) return;

  // ===== NTP 校時 =====
  configTime(8 * 3600, 0, "pool.ntp.org"); // 設定台灣時區+08:00
  Serial.print("等待 NTP 校時");
  int retries = 0;
  showTextOnLed("ntP    ", 1000);
  while (time(nullptr) < 1000000000) {
    delay(500);
    Serial.print(".");
    retries++;
    if (retries >= 5){
      retries = 0;
    }
  }
  Serial.println("\nNTP 校時完成");
  // ====================

  // HTTP 遠端開門
  server.on("/open", HTTP_GET, []() {
    openLock();
    clearDisplay();
    server.send(200, "text/plain", "OK");
  });

  // 主密碼變更API
  server.on("/set_password", HTTP_GET, []() {
    if (!server.hasArg("pw")) { server.send(400, "text/plain", "缺少pw"); showErrorNoOnLed(1); return; }
    String pw = server.arg("pw");
    // 檢查是否只包含數字
    for (int i = 0; i < pw.length(); i++) {
      if (pw[i] < '0' || pw[i] > '9') {
        server.send(400, "text/plain", "密碼只能包含數字");
        showErrorNoOnLed(2);
        return;
      }
    }
    correct_password = pw;
    writeStringToPreferences("mainpw", correct_password);
    server.send(200, "text/plain", "OK");
  });
  
  // 新增臨時密碼(到期)
  server.on("/add_temp_pw", HTTP_GET, []() {
    if (!server.hasArg("pw")) { 
      server.send(400, "text/plain", "缺少pw"); 
      return; 
    }
    String pw = server.arg("pw");
    String expireStr = server.hasArg("expire") ? server.arg("expire") : "";
    int count = server.hasArg("count") ? server.arg("count").toInt() : -1;
    
    int result = addTempPassword(pw, expireStr, count);
    if (result == 0) {
      server.send(200, "text/plain", "OK");
    } else if (result == 1) {
      server.send(400, "text/plain", "臨時密碼已達上限");
    } else if (result == 2) {
      server.send(400, "text/plain", "缺少pw");
    } else if (result == 3) {
      server.send(400, "text/plain", "密碼只能包含數字");
    } else {
      server.send(400, "text/plain", "時間格式錯誤");
    }
  });
  
  // 刪除臨時密碼
  server.on("/remove_temp_pw", HTTP_GET, []() {
    if (!server.hasArg("pw")) { server.send(400, "text/plain", "缺少pw"); showErrorNoOnLed(6); return; }
    String pw = server.arg("pw");
    bool found = false;
    for (auto it = tempPasswords.begin(); it != tempPasswords.end(); ++it) {
      if (it->pw == pw) {
        tempPasswords.erase(it);
        saveTempPasswordsToPreferences(); // 儲存臨時密碼
        found = true;
        break;
      }
    }
    if (found) {
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "臨時密碼不存在");
      showErrorNoOnLed(8);
    }
  });
  
  // 列出所有密碼API
  server.on("/list_passwords", HTTP_GET, []() {
    String json = "{\"main\":\"" + correct_password + "\",\"temps\":[";
    for (int i = 0; i < tempPasswords.size(); i++) {
      if (i > 0) json += ",";
      json += "{\"pw\":\"" + tempPasswords[i].pw + "\",\"expire\":" + String(tempPasswords[i].expireTime) + ",\"count\":" + String(tempPasswords[i].remainCount) + "}";
    }
    json += "]}";
    server.send(200, "application/json", json);
  });
  
  // 清除所有臨時密碼API
  server.on("/clear_temp_pw", HTTP_GET, []() {
    tempPasswords.clear();
    saveTempPasswordsToPreferences();
    server.send(200, "text/plain", "OK");
  });
  
  // 指紋註冊API
  server.on("/enroll", HTTP_GET, []() {
    if (!server.hasArg("id")) { server.send(400, "text/plain", "缺少id"); return; }
    int id = server.arg("id").toInt();
    if (fingerprintNameExists(id)) {
      server.send(400, "text/plain", "指紋ID已存在");
      showErrorNoOnLed(9);
      return;
    }
    if (enrollFingerprint(id)) {
      addFingerprintId(id); // 新增指紋ID到名單
      setFingerprintName(id, "User" + String(id)); // 新增指紋名稱
      server.send(200, "text/plain", "OK");
    } else {
      server.send(400, "text/plain", "指紋註冊失敗");
      showErrorNoOnLed(10);
    }
  });
  
  // 指紋清單 API
  server.on("/list_fingers", HTTP_GET, []() {
    std::vector<int> ids = parseIdsCsv(getFingerprintIdsCsv());
    String json = "[";
    for (size_t i = 0; i < ids.size(); i++) {
      if (i > 0) json += ",";
      int id = ids[i];
      String name = getFingerprintName(id);
      json += "{\"id\":" + String(id) + ",\"name\":\"" + name + "\"}";
    }
    json += "]";
    server.send(200, "application/json", json);
  });

  server.on("/rename_finger", HTTP_GET, []() {
    if (!server.hasArg("id") || !server.hasArg("name")) { server.send(400, "text/plain", "缺少id或name"); return; }
    int id = server.arg("id").toInt();
    String name = server.arg("name");
    std::vector<int> ids = parseIdsCsv(getFingerprintIdsCsv());
    bool exists = false; for (int v : ids) if (v == id) { exists = true; break; }
    if (!exists) { server.send(404, "text/plain", "指紋不存在"); return; }
    setFingerprintName(id, name);
    server.send(200, "text/plain", "OK");
  });

  // 指紋刪除API
  server.on("/delete_finger", HTTP_GET, []() {
    if (!server.hasArg("id")) { server.send(400, "text/plain", "缺少id"); return; }
    int id = server.arg("id").toInt();
    // 先刪除感測器中的指紋
    bool ok = deleteFingerprint(id);
    if (!ok) { server.send(500, "text/plain", "刪除失敗"); return; }
    server.send(200, "text/plain", "OK");
  });

  // 伺服角度 API：讀取與設定
  server.on("/get_servo_angle", HTTP_GET, []() {
    server.send(200, "application/json", String("{\"angle\":" + String(servoOpenAngle) + "}"));
  });

  server.on("/set_servo_angle", HTTP_GET, []() {
    if (!server.hasArg("angle")) { server.send(400, "text/plain", "缺少angle"); return; }
    int a = server.arg("angle").toInt();
    if (a < 0) a = 0; if (a > 180) a = 180;
    servoOpenAngle = a;
    writeIntToPreferences("servoAngle", servoOpenAngle);
    server.send(200, "text/plain", "OK");
  });
  
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>智慧門鎖控制</title></head><body>";
    html += "<h2>智慧門鎖控制面板</h2>";
    html += "<button onclick=\"fetch('/open').then(r=>alert('已開鎖'))\">開鎖</button><br><br>";
    html += "<h3>開門角度</h3>";
    html += "<button onclick=\"adjAngle(-1)\">-1</button> ";
    html += "<input id='angle' type='number' min='0' max='180' step='1' style='width:80px'> ";
    html += "<button onclick=\"adjAngle(1)\">+1</button> ";
    html += "<button onclick=\"saveAngle()\">儲存角度</button><br><br>";
    html += "<input id='pw' type='password' placeholder='新主密碼'><button onclick=\"setPw()\">設定主密碼</button><br><br>";
    html += "<input id='tpw' type='text' placeholder='臨時密碼'>";
    html += "<input id='expire' type='datetime-local' placeholder='到期時間'>";
    html += "<input id='count' type='number' placeholder='次數'><button onclick=\"addTempPw()\">新增臨時密碼</button><br><br>";
    html += "<button onclick=\"clearAllTempPw()\" style=\"background-color: #ff4444; color: white;\">清除所有臨時密碼</button><br><br>";
    html += "<h3>指紋管理</h3>";
    html += "<input id='fid' type='number' placeholder='指紋ID'><button onclick=\"enrollFinger()\">註冊指紋</button><br>";
    html += "<input id='did' type='number' placeholder='指紋ID'><button onclick=\"deleteFinger()\">刪除指紋</button><br><br>";
    html += "<h3>指紋清單</h3><div id='fingerlist'>載入中...</div>";
    html += "<h3>密碼清單</h3><div id='pwlist'>載入中...</div>";
    html += "<script>\n";
    html += "function loadAngle(){fetch('/get_servo_angle').then(r=>r.json()).then(j=>{document.getElementById('angle').value=j.angle;}).catch(e=>{console.log(e);});}\n";
    html += "function adjAngle(n){let el=document.getElementById('angle');let v=parseInt(el.value||0);v+=n;if(v<0)v=0;if(v>180)v=180;el.value=v;}\n";
    html += "function saveAngle(){let v=parseInt(document.getElementById('angle').value||0);if(isNaN(v))v=180;if(v<0)v=0;if(v>180)v=180;fetch('/set_servo_angle?angle='+v).then(r=>r.text()).then(t=>{alert('已儲存角度: '+v);}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function setPw(){let pw=document.getElementById('pw').value;if(!pw){alert('請輸入密碼');return;}fetch('/set_password?pw='+pw).then(r=>r.text()).then(t=>{alert(t);loadPwList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function addTempPw(){let pw=document.getElementById('tpw').value;if(!pw){alert('請輸入臨時密碼');return;}let expire=document.getElementById('expire').value;let count=document.getElementById('count').value;let url='/add_temp_pw?pw='+pw;if(expire)url+='&expire='+expire;if(count)url+='&count='+count;fetch(url).then(r=>r.text()).then(t=>{alert(t);loadPwList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function removeTempPw(pw){if(confirm('確定移除?'))fetch('/remove_temp_pw?pw='+pw).then(r=>r.text()).then(t=>{alert(t);loadPwList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function clearAllTempPw(){if(confirm('確定清除所有臨時密碼?'))fetch('/clear_temp_pw').then(r=>r.text()).then(t=>{alert(t);loadPwList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function enrollFinger(){let id=document.getElementById('fid').value;if(!id){alert('請輸入指紋ID');return;}fetch('/enroll?id='+id).then(r=>r.text()).then(t=>{alert(t);loadFingerList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function deleteFinger(){let id=document.getElementById('did').value;if(!id){alert('請輸入指紋ID');return;}fetch('/delete_finger?id='+id).then(r=>r.text()).then(t=>{alert(t);loadFingerList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function renameFinger(id){let name=prompt('輸入新名稱');if(!name){return;}fetch('/rename_finger?id='+id+'&name='+encodeURIComponent(name)).then(r=>r.text()).then(t=>{alert(t);loadFingerList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function loadFingerList(){fetch('/list_fingers').then(r=>r.json()).then(j=>{let html='<ul>';for(let f of j){html+='<li>#'+f.id+': '+f.name+' <button onclick=\\'renameFinger('+f.id+')\\'>改名</button> <button onclick=\\'deleteFingerId('+f.id+')\\'>刪除</button></li>';}html+='</ul>';document.getElementById('fingerlist').innerHTML=html;}).catch(e=>{document.getElementById('fingerlist').innerHTML='載入失敗: '+e;});}\n";
    html += "function deleteFingerId(id){if(confirm('確定刪除?'))fetch('/delete_finger?id='+id).then(r=>r.text()).then(t=>{alert(t);loadFingerList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function loadPwList(){fetch('/list_passwords').then(r=>r.json()).then(j=>{let html='<b>主密碼:</b> '+j.main+'<br><b>臨時密碼:</b><ul>';for(let tp of j.temps){html+='<li>'+tp.pw+' ';if(tp.expire)html+='(到期:'+new Date(tp.expire*1000).toLocaleString()+') ';if(tp.count>=0)html+='(剩餘:'+tp.count+') ';html+='<button onclick=\"removeTempPw(\\''+tp.pw+'\\')\">移除</button></li>';}html+='</ul>';document.getElementById('pwlist').innerHTML=html;}).catch(e=>{document.getElementById('pwlist').innerHTML='載入失敗: '+e;});}\n";
    html += "loadAngle();\n";
    html += "loadFingerList();\n";
    html += "loadPwList();\n";
    html += "</script></body></html>";
    server.send(200, "text/html", html);
  });
  
  server.begin();

  // 初始化 MQTT（僅在 WiFi 連接成功時）
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi 已連接，初始化 MQTT...");
    client.setServer(mqtt_server, mqtt_port);
    client.setCallback(mqttCallback);
    reconnectMQTT();
  } else {
    Serial.println("WiFi 未連接，跳過 MQTT 初始化");
  }

  //顯示oooo代表完成，可輸入密碼。
  showTextOnLed("oooo    ", 2000);
  clearDisplay();

  loadTempPasswordsFromPreferences(); // ← 開機時載入臨時密碼
}

void loop() {
  server.handleClient();
  
  // 僅在 WiFi 連接成功且不在設置模式時處理 MQTT
  if (!inSetupMode && WiFi.status() == WL_CONNECTED) {
    if (!client.connected()) {
      showErrorNoOnLed(16);
      reconnectMQTT();
    }
    client.loop();
  }
  
  if (inSetupMode) return;
  
  // 檢查指紋辨識
  if (fingerAvailable && inFingerRun) {
    checkFingerprint();
  } else {
    // 未在指紋偵測模式時，確保 LED 關閉
    if (fingerAvailable) finger.LEDcontrol(false);
  }
  
  // 每隔 KEYPAD_CHECK_INTERVAL ms 檢查一次按鍵
  if (millis() - lastKeypadCheck >= KEYPAD_CHECK_INTERVAL) {
    lastKeypadCheck = millis();
    checkKeypad();
  }
}

// 取得現在的 UNIX timestamp
unsigned long getNow() {
  time_t now;
  time(&now);
  return now;
}

// 驗證密碼（含臨時密碼）
bool checkAllPasswords(String input) {
  if (input == correct_password) {
    Serial.println("主密碼正確，準備開門");
    showResult('O'); // 顯示 O 代表 Open
    delay(100); // 等待顯示穩定
    Serial.println("開始執行開鎖動作");
    openLock();
    Serial.println("開鎖動作完成");
    return true;
  } 
  unsigned long now = getNow(); // 取得正確的現在時間
  for (auto it = tempPasswords.begin(); it != tempPasswords.end(); ) {
    Serial.print("pw：");
    Serial.println(it->pw); // 印出臨時密碼
    Serial.println(it->expireTime); // 印出到期時間
    Serial.println(it->remainCount); // 印出剩餘次數
    Serial.print("now：");
    Serial.println(now);
    Serial.print("相差：");
    Serial.println(it->expireTime - now); // 印出到期時間與現在的差

    // 檢查是否過期
    if (it->expireTime != 0 && now >= it->expireTime) {
      it = tempPasswords.erase(it); // 刪除過期密碼，iterator自動指向下一個
      saveTempPasswordsToPreferences(); // 保存變更到Preferences
      continue;
    }

    if (it->pw == input) {
      if ((it->expireTime==0 || now < it->expireTime) && (it->remainCount==-1 || it->remainCount>0)) {
        if (it->remainCount>0) it->remainCount--;
        if (it->remainCount==0) {
          it = tempPasswords.erase(it);
          saveTempPasswordsToPreferences(); // 保存變更到Preferences
        } else {
          ++it;
          saveTempPasswordsToPreferences(); // 保存變更到Preferences
        }
        Serial.println("臨時密碼正確，準備開門");
        showResult('O'); // 顯示 O 代表 Open
        delay(100); // 等待顯示穩定
        Serial.println("開始執行開鎖動作");
        openLock();
        Serial.println("開鎖動作完成");
        return true;
      } else {
        showErrorNoOnLed(7);
        return false;
      }
    } else {
      ++it;
    }
  }
  showResult('E');
  return false;
}

// 密碼輸入流程
void checkKeypad() {
  char key = checkKeypadInput();
  if (key) {
    if (key == '#') {
      checkAllPasswords(password_input);
      password_input = "";
    } else if (key == '*') {
      password_input = "";
      Serial.println("已清除");
      showResult('-'); // 顯示 - 代表 已清除
    } else if (key == 'A') {
      inFingerRun = !inFingerRun;
      if (inFingerRun) { 
        Serial.println("開啟指紋偵測");
        // 顯示指紋機啟動狀態
        showTextOnLed("FPON    ", 1000);
        clearDisplay();
      } else { 
        Serial.println("關閉指紋偵測");
        // 顯示指紋機關閉狀態
        showTextOnLed("FPOF    ", 1000);
        clearDisplay();
        // 關閉指紋機 LED（閒置時不亮）
        finger.LEDcontrol(false);
      }
    } else if (key == 'B') {
    } else if (key == 'C') {
    } else if (key == 'D') {
    } else {
      password_input += key;
      showPasswordOnLed(password_input);
    }
  }
}

// 開鎖控制（SG90 180度→3秒→0度）
void openLock() {
  Serial.println("=== 開鎖程序開始 ===");
  
  // 檢查冷卻時間
  unsigned long now = millis();
  if (now - lastServoAction < SERVO_COOLDOWN) {
    Serial.println("伺服馬達還在冷卻中，請稍候");
    return;
  }
  lastServoAction = now;
  
  Serial.println("1. 準備連接伺服馬達");
  safeServoAttach();
  
  Serial.print("2. 開始轉動到");
  Serial.print(servoOpenAngle);
  Serial.println("度（開鎖）");
  lockServo.write(servoOpenAngle); // 轉到設定角度（開鎖）
  Serial.println("開門");
  delay(3000); // 開鎖3秒
  
  Serial.println("3. 開始轉動到0度（關鎖）");
  lockServo.write(0);   // 轉回0度（關鎖）
  Serial.println("關門");
  delay(1000); // 關鎖1秒
  
  Serial.println("4. 斷開伺服馬達連接");
  safeServoDetach();
  
  Serial.println("=== 開鎖程序完成 ===");
}

// MQTT 回調函數
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  Serial.print("Message arrived [");
  Serial.print(topic);
  Serial.print("] ");
  String message = "";
  for (int i = 0; i < length; i++) {
    message += (char)payload[i];
  }
  message.trim(); // 去除前後空白字符
  Serial.println(message);
  
  String topicStr = String(topic);
  
  // 處理開門訊號
  if (topicStr == mqtt_topic && message == "open") {
    Serial.println("收到開門訊號，執行開門動作");
    showResult('O'); // 顯示成功
    openLock();
  }
  // 處理新增臨時密碼
  else if (topicStr == mqtt_topic_add_temp_pw) {
    Serial.println("收到新增臨時密碼請求");
    
    // 解析 JSON
    DynamicJsonDocument doc(512);
    DeserializationError error = deserializeJson(doc, message);
    
    if (error) {
      Serial.print("JSON 解析失敗: ");
      Serial.println(error.c_str());
      showErrorNoOnLed(17); // 使用錯誤編號 11 表示 JSON 解析錯誤
      return;
    }
    
    // 檢查必要欄位
    if (!doc.containsKey("pw")) {
      Serial.println("JSON 缺少必要欄位: pw");
      showErrorNoOnLed(4);
      return;
    }
    
    // 提取參數
    String pw = doc["pw"].as<String>();
    String expireStr = doc.containsKey("expire") ? doc["expire"].as<String>() : "";
    int count = doc.containsKey("count") ? doc["count"].as<int>() : -1;
    
    // 添加臨時密碼
    int result = addTempPassword(pw, expireStr, count);
    
    if (result == 0) {
      Serial.println("MQTT: 成功添加臨時密碼");
      showTextOnLed("OK     ", 1000); // 顯示成功
      clearDisplay();
    } else {
      Serial.print("MQTT: 添加臨時密碼失敗，錯誤碼: ");
      Serial.println(result);
      showErrorNoOnLed(3 + result); // 顯示對應的錯誤編號
    }
  }
}

// MQTT 重連函數
void reconnectMQTT() {
  // 檢查 WiFi 連接狀態
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi 未連接，無法連接 MQTT");
    return;
  }
  
  while (!client.connected()) {
    Serial.print("Attempting MQTT connection...");
    if (client.connect("ESP32Client", mqtt_user, mqtt_pass)) {
      Serial.println("connected");
      Serial.print("訂閱 topic: ");
      Serial.println(mqtt_topic);
      client.subscribe(mqtt_topic);
      Serial.print("訂閱 topic: ");
      Serial.println(mqtt_topic_add_temp_pw);
      client.subscribe(mqtt_topic_add_temp_pw);
    } else {
      Serial.print("failed, rc=");
      Serial.print(client.state());
      Serial.println(" try again in 5 seconds");
      delay(5000);
    }
  }
}

// v1.1.6