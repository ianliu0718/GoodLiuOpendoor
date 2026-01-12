#include <MD_MAX72xx.h>

// 建議使用 GENERIC_HW，因為它的腳位對應最直接 (Bit 0-7 對應 Seg DP, A-G)
#define HARDWARE_TYPE MD_MAX72XX::GENERIC_HW  // ← FC16_HW 、 GENERIC_HW
#define MAX_DEVICES   1

// 你的 ESP32 腳位
#define DATA_PIN      18
#define CLK_PIN       19
#define CS_PIN        21

MD_MAX72XX mx(HARDWARE_TYPE, DATA_PIN, CLK_PIN, CS_PIN, MAX_DEVICES); // 注意建構子順序: Data, Clk, CS (不同版本庫有時順序不同，若不行請調換 CLK/CS)

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
    case 'G': case 'g': return 0xFA; // G (與6相同，通常用大寫) 或用 0x7A (不帶頂槓)
    case 'H':           return 0xEC; // H (大寫)
    case 'h':           return 0xE8; // h (小寫)
    case 'I': case 'i': return 0x0C; // I (顯示為 1，右側) 或 0x20 (左側)
    case 'J': case 'j': return 0x3C; // J (大寫)
    case 'K': case 'k': return 0xE8; // K (顯示如 h，七段無法顯示K)
    case 'L': case 'l': return 0x70; // L (大寫)
    case 'M': case 'm': return 0xEA; // m (顯示為兩段分離，類似 n 加一豎，難以完美顯示) -> 這裡用類似A但頂部缺口 
                        // 建議顯示 'n' 兩次或用特殊符號，這裡暫定顯示為 n
    case 'N': case 'n': return 0xA8; // n (小寫)
    case 'O': case 'o': return 0xB8; // o (小寫) -> 若用大寫會跟 0 混淆
    case 'P': case 'p': return 0xE6; // P (大寫)
    case 'Q': case 'q': return 0xCE; // q (小寫)
    case 'R': case 'r': return 0xA0; // r (小寫)
    case 'S': case 's': return 0xDA; // S (顯示為 5)
    case 'T': case 't': return 0xF0; // t (小寫)
    case 'U':           return 0x7C; // U (大寫)
    case 'u':           return 0x38; // u (小寫)
    case 'V': case 'v': return 0x38; // u (顯示為 u，七段無法顯示斜線)
    case 'W': case 'w': return 0x38; // 類似 u (無法顯示 W)
    case 'X': case 'x': return 0xEC; // 顯示為 H (無法顯示 X)
    case 'Y': case 'y': return 0xDC; // y (小寫)
    case 'Z': case 'z': return 0xB6; // Z (顯示為 2)

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

void showTextOnLed(String text, int delayTime = 700) {
  mx.clear();
  int len = text.length();
  for (int i = 0; i < 8; i++) {
    mx.setColumn(i, get7SegPattern(text[len-i-1])); 
  }
  delay(delayTime);
}

void setup(){
  //Serial.begin(9600);
  //Serial.println("TEST START");
  
  mx.begin();
  mx.control(MD_MAX72XX::INTENSITY, 1); // 亮度 0-15
  mx.clear();
}

void loop(){
  // 顯示測試文字
  showTextOnLed("Proc123"); 
  delay(1000);
  
  showTextOnLed("AbCd-890");
  delay(1000);

  // 簡單計數測試
  for(int i=0; i<10; i++) {
    showTextOnLed("Count " + String(i), 200);
  }
}