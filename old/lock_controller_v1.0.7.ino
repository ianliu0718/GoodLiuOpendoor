// NodeMCU 智慧門鎖主控程式
// 功能：指紋辨識、密碼開鎖、遠端HTTP開鎖
// 材料：NodeMCU(ESP8266)、AS608、4x4 Keypad、繼電器

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Keypad.h>
#include <Adafruit_Fingerprint.h>
#include <SoftwareSerial.h>
#include <vector>
#include <Servo.h>
#include <LedControl.h>
#include <EEPROM.h>
#include <time.h> // 新增 NTP 校時

// WiFi 設定
#define EEPROM_SIZE 128
#define SSID_ADDR 0
#define PASS_ADDR 32
#define MAINPW_ADDR 64
String sta_ssid = "goodliu-2.4G";
String sta_password = "11111111";
bool inSetupMode = false;
ESP8266WebServer server(80);

// 指紋模組設定
//SoftwareSerial fingerSerial(D9, D10); // RX, TX
//Adafruit_Fingerprint finger = Adafruit_Fingerprint(&fingerSerial);

bool inFingerRun = false;
bool fingerAvailable = false;
bool isShowingResult = false;

// 4x4 Keypad 設定
const byte ROWS = 4;
const byte COLS = 4;
char keys[ROWS][COLS] = {
  {'1','2','3','A'},
  {'4','5','6','B'},
  {'7','8','9','C'},
  {'*','0','#','D'}
};
byte rowPins[ROWS] = {D1, D2, D3, D4}; // 與 MAX7219 共用
byte colPins[COLS] = {D5, D6, D7, D8}; // 與 MAX7219 共用
Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

// 密碼設定
String correct_password = ""; // 主要預設密碼
String password_input = "";       //  程式修改後儲存密碼
String currentDisplayText = "";   //  程式比對用的密碼
unsigned long lastKeypadCheck = 0;
const unsigned long KEYPAD_CHECK_INTERVAL = 120; // 每120ms掃描一次

// SG90控制腳位
#define SERVO_PIN D0
Servo lockServo;

// 伺服馬達狀態控制
bool servoAttached = false;
unsigned long lastServoAction = 0;
const unsigned long SERVO_COOLDOWN = 2000; // 伺服馬達冷卻時間

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
#define TEMP_PW_EEPROM_ADDR 80 // EEPROM 80~127
#define TEMP_PW_STRUCT_SIZE 14 // 8+4+2

void saveTempPasswordsToEEPROM() {
  int addr = TEMP_PW_EEPROM_ADDR;
  for (int i = 0; i < TEMP_PW_MAX; i++) {
    if (i < tempPasswords.size()) {
      TempPassword &tp = tempPasswords[i];
      // 密碼字串（8 bytes）
      for (int j = 0; j < TEMP_PW_LEN; j++) {
        char c = (j < tp.pw.length()) ? tp.pw[j] : '\0';
        EEPROM.write(addr + j, c);
      }
      // 到期時間（4 bytes）
      unsigned long t = tp.expireTime;
      for (int j = 0; j < 4; j++) {
        EEPROM.write(addr + TEMP_PW_LEN + j, (t >> (8 * j)) & 0xFF);
      }
      // 次數（2 bytes）
      int16_t cnt = tp.remainCount;
      EEPROM.write(addr + TEMP_PW_LEN + 4, cnt & 0xFF);
      EEPROM.write(addr + TEMP_PW_LEN + 5, (cnt >> 8) & 0xFF);
    } else {
      // 清空 - 確保完全清空，避免殘留數據
      for (int j = 0; j < TEMP_PW_LEN; j++) {
        EEPROM.write(addr + j, 0xFF); // 密碼部分用0xFF
      }
      // 時間和次數部分也用0xFF
      for (int j = TEMP_PW_LEN; j < TEMP_PW_STRUCT_SIZE; j++) {
        EEPROM.write(addr + j, 0xFF);
      }
    }
    addr += TEMP_PW_STRUCT_SIZE;
  }
  EEPROM.commit();
}

void loadTempPasswordsFromEEPROM() {
  tempPasswords.clear();
  int addr = TEMP_PW_EEPROM_ADDR;
  for (int i = 0; i < TEMP_PW_MAX; i++) {
    // 讀密碼
    char pwbuf[TEMP_PW_LEN + 1];
    for (int j = 0; j < TEMP_PW_LEN; j++) {
      pwbuf[j] = EEPROM.read(addr + j);
    }
    pwbuf[TEMP_PW_LEN] = '\0';
    String pw = String(pwbuf);
    
    // 檢查是否為有效的數字密碼
    bool validPw = true;
    if (pw.length() == 0 || pw.length() > 8) {
      validPw = false;
    } else {
      for (int j = 0; j < pw.length(); j++) {
        if (pw[j] < '0' || pw[j] > '9') {
          validPw = false;
          break;
        }
      }
    }
    
    // 若密碼無效或全為0xFF視為空
    bool empty = true;
    for (int j = 0; j < TEMP_PW_STRUCT_SIZE; j++) {
      if (EEPROM.read(addr + j) != 0xFF) { empty = false; break; }
    }
    
    if (empty || !validPw) { 
      addr += TEMP_PW_STRUCT_SIZE; 
      continue; 
    }
    
    // 讀到期時間
    unsigned long t = 0;
    for (int j = 0; j < 4; j++) {
      t |= ((unsigned long)EEPROM.read(addr + TEMP_PW_LEN + j)) << (8 * j);
    }
    // 讀次數
    int16_t cnt = EEPROM.read(addr + TEMP_PW_LEN + 4) | (EEPROM.read(addr + TEMP_PW_LEN + 5) << 8);
    
    TempPassword tp;
    tp.pw = pw;
    tp.expireTime = t;
    tp.remainCount = cnt;
    tempPasswords.push_back(tp);
    addr += TEMP_PW_STRUCT_SIZE;
  }
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

// MAX7219 腳位對應（與 Keypad 共用）
#define DIN_PIN  D1
#define CS_PIN   D2
#define CLK_PIN  D3

LedControl lc = LedControl(DIN_PIN, CLK_PIN, CS_PIN, 1); // 1 代表一片 MAX7219

// 切換到 MAX7219 模式
void switchToMax7219() {
  // 設定腳位為輸出模式（MAX7219 需要）
  pinMode(D1, OUTPUT);
  pinMode(D2, OUTPUT);
  pinMode(D3, OUTPUT);
  lc.shutdown(0, false); // 啟動
  lc.setIntensity(0, 8); // 亮度 0~15
}

// 切換回 Keypad 模式
void switchToKeypad() {
  // 設定腳位為輸入模式（Keypad 需要）
  pinMode(D1, INPUT_PULLUP);
  pinMode(D2, INPUT_PULLUP);
  pinMode(D3, INPUT_PULLUP);
  pinMode(D4, INPUT_PULLUP);
  pinMode(D5, OUTPUT);
  pinMode(D6, OUTPUT);
  pinMode(D7, OUTPUT);
  pinMode(D8, OUTPUT);
}

// 顯示
void showTextOnLed(String text) {
  switchToMax7219();

  int len = text.length();
  for (int i = 0; i < 8; i++) {
    if (i < len) {
      char c = text[len - 1 - i]; // 右對齊
      if (c >= '0' && c <= '9') {
        lc.setDigit(0, i, c - '0', false);
      } else {
        lc.setChar(0, i, c, false);
      }
    } else {
      lc.setChar(0, i, ' ', false);
    }
  }
  delay(700);
}

// 顯示密碼在 MAX7219（右對齊，最多8碼）
void showPasswordOnLed(String pw) {
  Serial.println(pw);
  if (pw == currentDisplayText) return; // 避免重複刷新
  currentDisplayText = pw;
  switchToMax7219();

  int len = pw.length();
  for (int i = 0; i < 8; i++) {
    if (i < len) {
      char c = pw[len - 1 - i]; // 右對齊
      if (c >= '0' && c <= '9') {
        lc.setDigit(0, i, c - '0', false);
      } else {
        lc.setChar(0, i, ' ', false);
      }
    } else {
      lc.setChar(0, i, ' ', false);
    }
  }
  delay(700);
}

// 顯示錯誤編號在 MAX7219（右對齊，最多4碼）
void showErrorNoOnLed(int no) {
  Serial.println(no);
  switchToMax7219();
  clearDisplay();

  lc.setChar(0, 6, 'E', false);
  lc.setRow(0, 5, B00000101); // r
  lc.setRow(0, 4, B00000101); // r

  for (int i = 0; i < 4; i++) {
    if (no > 0) {
      int digit = no % 10;
      lc.setDigit(0, i, digit, false);
      no /= 10;
    } else {
      lc.setChar(0, i, ' ', false);
    }
  }
  delay(2000); // 顯示2秒
  clearDisplay();
}

// 顯示結果（O 或 E）只支援 0-9、E、H、L、P、-、空白等）
void showResult(char result) {
  Serial.println(result);
  switchToMax7219();
  for (int i = 0; i < 8; i++) lc.setChar(0, i, ' ', false);
  if (result == 'O') {
    lc.setRow(0, 0, B00011101); // o
  } else if (result == 'I') {
    lc.setRow(0, 0, B00000110); // I
  } else if (result == 'E') {
    // 顯示 "Error"
    lc.setChar(0, 4, 'E', false);
    lc.setRow(0, 3, B00000101); // r
    lc.setRow(0, 2, B00000101); // r
    lc.setChar(0, 1, 'o', false);
    lc.setRow(0, 0, B00000101); // r
  } else {
    lc.setChar(0, 0, result, false);
  }
  delay(2000);
  clearDisplay();
}

// 清空顯示
void clearDisplay() {
  lc.clearDisplay(0);
}

// 檢查按鍵（短暫切換到 Keypad 模式）
char checkKeypadInput() {
  switchToKeypad();
  char key = keypad.getKey();
  switchToMax7219();
  return key;
}

// EEPROM 相關函式
void writeStringToEEPROM(int addr, const String& str) {
  for (unsigned int i = 0; i < str.length(); ++i) {
    EEPROM.write(addr + i, str[i]);
  }
  EEPROM.write(addr + str.length(), '\0');
  EEPROM.commit();
}
String readStringFromEEPROM(int addr) {
  char data[32];
  int len = 0;
  unsigned char c;
  while ((c = EEPROM.read(addr + len)) != '\0' && len < 31) {
    data[len] = c;
    len++;
  }
  data[len] = '\0';
  return String(data);
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
  lc.setRow(0, 7, B01001110); // C
  lc.setRow(0, 6, B00011101); // o
  lc.setRow(0, 5, B00010101); // n
  for(int i=0;i<5;i++) lc.setChar(0, i, ' ', false);
  while (WiFi.status() != WL_CONNECTED && retries < 5) {
    delay(6000);
    Serial.print(".");
    lc.setRow(0, 5-retries-1, B10000000); // .
    retries++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi 連線成功");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
    // 顯示IP後兩組數字於MAX7219
    String ipStr = WiFi.localIP().toString();
    int firstDot = ipStr.lastIndexOf('.', ipStr.lastIndexOf('.') - 1);
    String last2 = ipStr.substring(firstDot + 1); // 取最後兩組
    int dotPos = last2.indexOf('.');
    String group1 = last2.substring(0, dotPos);   // 倒數第2組
    String group2 = last2.substring(dotPos + 1);  // 倒數第1組
    // 顯示 IP
    for(int i=group2.length();i<4;i++) group2 = " " + group2;
    showTextOnLed(group1 + group2);
    delay(3000);
  } else {
    Serial.println("\nWiFi 連線失敗，啟動AP模式");
    inSetupMode = true;
    WiFi.softAP("Lock_Setup_AP", "password");
    server.on("/", HTTP_GET, handleRoot);
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    lc.setRow(0, 7, B01110111);   // A
    lc.setRow(0, 6, B01100111);   // P
    for(int i=0;i<6;i++) lc.setChar(0, i, ' ', false);
    delay(3000);
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
    for(int i=group2.length();i<4;i++) group2 = " " + group2;
    showTextOnLed(group1 + group2);
  }
}
void handleRoot() {
  String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>WiFi設定</title></head><body>";
  html += "<form method='post' action='/save'>WiFi SSID:<input name='ssid'><br>密碼:<input name='pass' type='password'><br><button type='submit'>儲存</button></form>";
  html += "</body></html>";
  server.send(200, "text/html; charset=utf-8", html);
}
void handleSave() {
  writeStringToEEPROM(SSID_ADDR, server.arg("ssid"));
  writeStringToEEPROM(PASS_ADDR, server.arg("pass"));
  server.send(200, "text/plain", "設定已儲存，將重啟...");
  delay(1000);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  Serial.println("v1.0.7");
  //pinMode(RELAY_PIN, OUTPUT);
  //digitalWrite(RELAY_PIN, LOW); // 預設關閉
  
  Serial.println("初始化伺服馬達...");
  safeServoAttach();
  lockServo.write(90);
  delay(1000);
  lockServo.write(0);   // 轉回0度（關鎖）
  delay(1000);
  safeServoDetach();

  
  // 初始化 MAX7219
  lc.shutdown(0, false); // 啟動
  lc.setIntensity(0, 8); // 亮度 0~15
  clearDisplay();    // 清除

  switchToMax7219();
  
  sta_ssid = readStringFromEEPROM(SSID_ADDR);
  sta_password = readStringFromEEPROM(PASS_ADDR);
  correct_password = readStringFromEEPROM(MAINPW_ADDR);
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
    lc.setRow(0, 7, B01110111);   // A
    lc.setRow(0, 6, B01100111);   // P
    for(int i=0;i<6;i++) lc.setChar(0, i, ' ', false);
    delay(3000);
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
    for(int i=group2.length();i<4;i++) group2 = " " + group2;
    showTextOnLed(group1 + group2);
  }
  if (inSetupMode) return;

  // ===== NTP 校時 =====
  configTime(8 * 3600, 0, "pool.ntp.org"); // 設定台灣時區+08:00
  Serial.print("等待 NTP 校時");
  int retries = 0;
  lc.setRow(0, 7, B00010101); // n
  lc.setRow(0, 6, B00001111); // t
  lc.setRow(0, 5, B01100111); // P
  for(int i=0;i<5;i++) lc.setChar(0, i, ' ', false);
  while (time(nullptr) < 1000000000) {
    delay(500);
    Serial.print(".");
    lc.setRow(0, 5-retries-1, B10000000); // .
    retries++;
    if (retries >= 5){
      retries = 0;
      for(int i=0;i<6;i++) lc.setChar(0, i, ' ', false);
    }
  }
  Serial.println("\nNTP 校時完成");
  // ====================

  // HTTP 遠端開門
  server.on("/open", HTTP_GET, []() {
    openLock();
    server.send(200, "text/plain", "OK");
  });
  //// 指紋註冊API
  //server.on("/enroll", HTTP_GET, []() {
  //  if (!server.hasArg("id")) { server.send(400, "text/plain", "缺少id"); return; }
  //  int id = server.arg("id").toInt();
  //  server.send(200, "text/plain", enrollFingerprint(id) ? "OK" : "FAIL");
  //});
  //// 指紋刪除API
  //server.on("/delete", HTTP_GET, []() {
  //  if (!server.hasArg("id")) { server.send(400, "text/plain", "缺少id"); return; }
  //  int id = server.arg("id").toInt();
  //  server.send(200, "text/plain", deleteFingerprint(id) ? "OK" : "FAIL");
  //});
  // 主密碼變更API
  server.on("/set_password", HTTP_GET, []() {
    if (!server.hasArg("pw")) { server.send(400, "text/plain", "缺少pw"); showErrorNoOnLed(1); return; }
    String pw = server.arg("pw");
    //if (pw.length() > 8) {
    //  server.send(400, "text/plain", "密碼長度不能超過8位");
    //  return;
    //}
    // 檢查是否只包含數字
    for (int i = 0; i < pw.length(); i++) {
      if (pw[i] < '0' || pw[i] > '9') {
        server.send(400, "text/plain", "密碼只能包含數字");
        showErrorNoOnLed(2);
        return;
      }
    }
    correct_password = pw;
    writeStringToEEPROM(MAINPW_ADDR, correct_password);
    server.send(200, "text/plain", "OK");
  });
  // 新增臨時密碼(到期)
  server.on("/add_temp_pw", HTTP_GET, []() {
    if (tempPasswords.size() >= 10) {
      server.send(400, "text/plain", "臨時密碼已達上限");
      showErrorNoOnLed(3);
      return;
    }
    if (!server.hasArg("pw")) { server.send(400, "text/plain", "缺少pw"); showErrorNoOnLed(4); return; }
    String pw = server.arg("pw");
    //if (pw.length() > 8) {
    //  server.send(400, "text/plain", "密碼長度不能超過8位");
    //  return;
    //}
    // 檢查是否只包含數字
    for (int i = 0; i < pw.length(); i++) {
      if (pw[i] < '0' || pw[i] > '9') {
        server.send(400, "text/plain", "密碼只能包含數字");
        showErrorNoOnLed(5);
        return;
      }
    }
    TempPassword tp; tp.pw = pw;
    if (server.hasArg("expire")) {
      tp.expireTime = parseTime(server.arg("expire"));
      Serial.println(String(tp.expireTime));
    }
    if (server.hasArg("count")) tp.remainCount = server.arg("count").toInt();
    tempPasswords.push_back(tp);
    saveTempPasswordsToEEPROM(); // 儲存臨時密碼
    server.send(200, "text/plain", "OK");
  });
  // 刪除臨時密碼
  server.on("/remove_temp_pw", HTTP_GET, []() {
    if (!server.hasArg("pw")) { server.send(400, "text/plain", "缺少pw"); showErrorNoOnLed(6); return; }
    String pw = server.arg("pw");
    bool found = false;
    for (auto it = tempPasswords.begin(); it != tempPasswords.end(); ++it) {
      if (it->pw == pw) {
        tempPasswords.erase(it);
        saveTempPasswordsToEEPROM(); // 儲存臨時密碼
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
    saveTempPasswordsToEEPROM();
    server.send(200, "text/plain", "OK");
  });
  server.on("/", HTTP_GET, []() {
    String html = "<!DOCTYPE html><html><head><meta charset='utf-8'><title>智慧門鎖控制</title></head><body>";
    html += "<h2>智慧門鎖控制面板</h2>";
    html += "<button onclick=\"fetch('/open').then(r=>alert('已開鎖'))\">開鎖</button><br><br>";
    html += "<input id='pw' type='password' placeholder='新主密碼'><button onclick=\"setPw()\">設定主密碼</button><br><br>";
    html += "<input id='tpw' type='text' placeholder='臨時密碼'>";
    html += "<input id='expire' type='datetime-local' placeholder='到期時間'>";
    html += "<input id='count' type='number' placeholder='次數'><button onclick=\"addTempPw()\">新增臨時密碼</button><br><br>";
    html += "<button onclick=\"clearAllTempPw()\" style=\"background-color: #ff4444; color: white;\">清除所有臨時密碼</button><br><br>";
    html += "<h3>密碼清單</h3><div id='pwlist'>載入中...</div>";
    html += "<script>\n";
    html += "function setPw(){let pw=document.getElementById('pw').value;if(!pw){alert('請輸入密碼');return;}fetch('/set_password?pw='+pw).then(r=>r.text()).then(t=>{alert(t);loadPwList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function addTempPw(){let pw=document.getElementById('tpw').value;if(!pw){alert('請輸入臨時密碼');return;}let expire=document.getElementById('expire').value;let count=document.getElementById('count').value;let url='/add_temp_pw?pw='+pw;if(expire)url+='&expire='+expire;if(count)url+='&count='+count;fetch(url).then(r=>r.text()).then(t=>{alert(t);loadPwList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function removeTempPw(pw){if(confirm('確定移除?'))fetch('/remove_temp_pw?pw='+pw).then(r=>r.text()).then(t=>{alert(t);loadPwList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function clearAllTempPw(){if(confirm('確定清除所有臨時密碼?'))fetch('/clear_temp_pw').then(r=>r.text()).then(t=>{alert(t);loadPwList();}).catch(e=>{alert('錯誤: '+e);});}\n";
    html += "function loadPwList(){fetch('/list_passwords').then(r=>r.json()).then(j=>{let html='<b>主密碼:</b> '+j.main+'<br><b>臨時密碼:</b><ul>';for(let tp of j.temps){html+='<li>'+tp.pw+' ';if(tp.expire)html+='(到期:'+new Date(tp.expire*1000).toLocaleString()+') ';if(tp.count>=0)html+='(剩餘:'+tp.count+') ';html+='<button onclick=\"removeTempPw(\\''+tp.pw+'\\')\">移除</button></li>';}html+='</ul>';document.getElementById('pwlist').innerHTML=html;}).catch(e=>{document.getElementById('pwlist').innerHTML='載入失敗: '+e;});}\n";
    html += "loadPwList();\n";
    html += "</script></body></html>";
    server.send(200, "text/html", html);
  });
  server.begin();

  //// 指紋模組初始化
  //Serial.println("指紋模組偵測");
  //finger.begin(57600);
  //if (finger.verifyPassword()) {
  //  Serial.println("指紋模組連接成功");
  //  fingerAvailable = true;
  //} else {
  //  Serial.println("指紋模組連接失敗");
  //  fingerAvailable = false;
  //}

  //顯示oooo代表完成，可輸入密碼。
  clearDisplay();
  for(int i=4;i<8;i++) lc.setRow(0, i, B00011101);
  delay(2000); // 顯示2秒
  clearDisplay();

  loadTempPasswordsFromEEPROM(); // ← 開機時載入臨時密碼
}

void loop() {
  server.handleClient();
  if (inSetupMode) return;
  //if (fingerAvailable && inFingerRun) checkFingerprint();
  
  // 每隔 KEYPAD_CHECK_INTERVAL ms 檢查一次按鍵
  if (millis() - lastKeypadCheck >= KEYPAD_CHECK_INTERVAL) {
    lastKeypadCheck = millis();
    checkKeypad();
  }
}

// 指紋辨識流程
//void checkFingerprint() {
//  if (finger.getImage() == FINGERPRINT_NOFINGER) {
//    // 沒有手指，立刻 return，不阻塞
//    return;
//  }
//  if (finger.getImage() == FINGERPRINT_OK) {
//    if (finger.image2Tz() == FINGERPRINT_OK) {
//      if (finger.fingerSearch() == FINGERPRINT_OK) {
//        Serial.println("指紋辨識通過，開門");
//        openLock();
//        delay(2000); // 防止重複觸發
//      } else {
//        Serial.println("指紋不符");
//      }
//    }
//  }
//}

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
    // 先確保腳位狀態穩定
    switchToMax7219();
    delay(100); // 等待腳位穩定
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
      saveTempPasswordsToEEPROM(); // 保存變更到EEPROM
      continue;
    }

    if (it->pw == input) {
      if ((it->expireTime==0 || now < it->expireTime) && (it->remainCount==-1 || it->remainCount>0)) {
        if (it->remainCount>0) it->remainCount--;
        if (it->remainCount==0) {
          it = tempPasswords.erase(it);
          saveTempPasswordsToEEPROM(); // 保存變更到EEPROM
        } else {
          ++it;
          saveTempPasswordsToEEPROM(); // 保存變更到EEPROM
        }
        Serial.println("臨時密碼正確，準備開門");
        // 先確保腳位狀態穩定
        switchToMax7219();
        delay(100); // 等待腳位穩定
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
      //if (inFingerRun) { Serial.println("開啟指紋偵測"); }
      //else { Serial.println("關閉指紋偵測"); }
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
  
  // 確保腳位狀態穩定
  switchToMax7219();
  delay(200); // 等待腳位完全穩定
  
  Serial.println("1. 準備連接伺服馬達");
  safeServoAttach();
  
  Serial.println("2. 開始轉動到180度（開鎖）");
  lockServo.write(180); // 轉到180度（開鎖）
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

// 指紋註冊/刪除函式
//bool enrollFingerprint(int id) {
//  finger.getTemplateCount();
//  finger.LEDcontrol(true);
//  Serial.print("請放上手指...");
//  if (finger.getImage() != FINGERPRINT_OK) return false;
//  if (finger.image2Tz(1) != FINGERPRINT_OK) return false;
//  Serial.print("請再次放上手指...");
//  delay(2000);
//  if (finger.getImage() != FINGERPRINT_OK) return false;
//  if (finger.image2Tz(2) != FINGERPRINT_OK) return false;
//  if (finger.createModel() != FINGERPRINT_OK) return false;
//  if (finger.storeModel(id) != FINGERPRINT_OK) return false;
//  finger.LEDcontrol(false);
//  return true;
//}
//bool deleteFingerprint(int id) {
//  return finger.deleteModel(id) == FINGERPRINT_OK;
//} 

// v1.0.7 - 修復網頁界面載入問題
