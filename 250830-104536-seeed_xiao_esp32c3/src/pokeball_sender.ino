/*
 * 專案：寶可夢NFC圖鑑 - 寶貝球發送端
 * 開發板：Seeed Studio XIAO ESP32C3
 * 模組：PN532 (I2C) + ESP-NOW
 * 功能：掃描NFC卡片，讀取卡片中的文字內容(寶可夢編號)，並透過ESP-NOW傳送出去。
 * 版本：v5 (採用通用數字解析)
 */

#include <esp_now.h>
#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_PN532.h>
#include <ctype.h> // 引用 ctype.h 以使用 isdigit()

// --- PN532 設定 ---
#define PN532_IRQ   (-1) 
#define PN532_RESET (-1)
Adafruit_PN532 nfc(PN532_IRQ, PN532_RESET);

// --- ESP-NOW 設定 ---
// TODO: 請將這裡的 MAC 位址替換成您的接收端(D1 R32)的位址
uint8_t broadcastAddress[] = {0x08, 0x3A, 0xF2, 0xB7, 0xC0, 0xEC};
#define WIFI_CHANNEL 1 // 定義一個固定的 Wi-Fi 頻道

// 定義要傳送的資料結構
typedef struct struct_message {
  int pokemon_id;
} struct_message;

struct_message myData;
esp_now_peer_info_t peerInfo;

void OnDataSent(const wifi_tx_info_t *info, esp_now_send_status_t status) {
  Serial.print("\r\n傳送狀態: ");
  if (status == ESP_NOW_SEND_SUCCESS) {
    Serial.println("資料傳送成功");
  } else {
    Serial.println("資料傳送失敗 - 請檢查接收端是否開啟且頻道設定一致");
  }
}

void setup() {
  Serial.begin(115200);
  while (!Serial);
  Serial.println("ESP-NOW 發送端初始化 (v5)...");

  // 設定 Wi-Fi 模式為 Station 並指定頻道
  WiFi.mode(WIFI_STA);
  WiFi.channel(WIFI_CHANNEL);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW 初始化失敗");
    return;
  }

  esp_now_register_send_cb(OnDataSent);

  // 註冊通訊對象
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = WIFI_CHANNEL; 
  peerInfo.encrypt = false;
  
  if (esp_now_add_peer(&peerInfo) != ESP_OK){
    Serial.println("新增通訊對象失敗");
    return;
  }
  Serial.println("ESP-NOW 初始化完成");

  // --- 初始化 PN532 ---
  nfc.begin();
  uint32_t versiondata = nfc.getFirmwareVersion();
  if (!versiondata) {
    Serial.print("找不到 PN532 模組，請檢查接線！");
    while (1); 
  }
  Serial.println("找到 PN532 模組！");
  nfc.SAMConfig();
  Serial.println("等待 NFC 卡片...");
}


// **最終修正**: 通用化解析函式，直接從 payload 中提取數字字串
int parseTextPayload(uint8_t* payload, int payload_length) {
    if (payload_length < 1) return 0;

    char numeric_buffer[payload_length + 1];
    int numeric_idx = 0;
    bool found_digit = false;

    // 遍歷整個 payload
    for (int i = 0; i < payload_length; i++) {
        // 如果當前字元是數字
        if (isdigit(payload[i])) {
            numeric_buffer[numeric_idx++] = payload[i];
            found_digit = true;
        } else {
            // 如果已經找到數字，但又遇到非數字，表示數字序列結束
            if (found_digit) {
                break;
            }
            // 如果還沒找到數字，就繼續尋找
        }
    }

    // 如果整個 payload 都沒有數字
    if (!found_digit) {
        Serial.println("在 NDEF 負載中未找到任何數字。");
        return 0;
    }
    
    // 加上字串結束符
    numeric_buffer[numeric_idx] = '\0';

    Serial.print("從負載中提取的數字字串: \"");
    Serial.print(numeric_buffer);
    Serial.println("\"");

    // 將提取出的數字字串轉換為整數
    return atoi(numeric_buffer);
}


// 讀取卡片原始資料，手動解析 NDEF 結構，並回傳寶可夢 ID
int getPokemonIdFromCard() {
  uint8_t rawCardData[64]; // 用於儲存 Mifare Ultralight 前 16 頁 (64 bytes) 的資料
  bool readSuccess = false;

  Serial.println("正在讀取卡片原始資料...");
  
  // 讀取前 16 頁 (使用者資料通常從第 4 頁開始)
  for (uint8_t page = 0; page < 16; page++) {
    uint8_t page_buffer[4];
    if (nfc.mifareultralight_ReadPage(page, page_buffer)) {
      readSuccess = true;
      memcpy(rawCardData + (page * 4), page_buffer, 4);
    } else {
      Serial.print("讀取頁面 ");
      Serial.print(page);
      Serial.println(" 失敗。");
      readSuccess = false; // 如果有任何一頁讀取失敗，就中止
      break; 
    }
  }

  if (!readSuccess) {
    Serial.println("讀取卡片資料失敗。");
    return 0;
  }

  Serial.println("原始資料讀取成功。開始解析 NDEF...");

  // --- 手動 NDEF 解析 ---
  // 從第 16 個位元組 (第 4 頁) 開始尋找，這是常見的使用者資料起始位置
  int pos = 16; 
  while (pos < 64) {
    uint8_t tlv_type = rawCardData[pos];
    
    if (tlv_type == 0x03) { // 找到 NDEF Message TLV (Type)
      Serial.println("找到 NDEF Message TLV (0x03)。");
      pos++;
      if (pos >= 64) break;
      
      uint8_t tlv_len = rawCardData[pos]; // Length
      pos++;
      
      Serial.print("NDEF 訊息長度: ");
      Serial.println(tlv_len);
      
      if (pos + tlv_len <= 64) {
        // 解析 NDEF 訊息本身 (Value)
        uint8_t* message_ptr = rawCardData + pos;
        
        uint8_t type_length = message_ptr[1];
        uint8_t payload_length = message_ptr[2]; // 假設是短記錄格式

        Serial.print("偵測到的 NDEF 記錄類型: ");
        for(int i = 0; i < type_length; i++) {
          Serial.print((char)message_ptr[3+i]);
        }
        Serial.println();

        // 只要有負載 (payload)，就直接嘗試解析
        if (payload_length > 0) {
            Serial.println("發現有效負載 (payload)，嘗試解析...");
            int payload_start = 3 + type_length;
            return parseTextPayload(message_ptr + payload_start, payload_length);
        } else {
            Serial.println("NDEF 記錄中沒有負載 (payload)。");
        }
      }
      return 0; // 處理完第一個 NDEF 訊息後就結束
    } else if (tlv_type == 0xFE) { // 找到結束標記
      Serial.println("找到 NDEF 結束標記 (0xFE)。");
      break;
    } else if (tlv_type == 0x00) { // 空的 TLV
      pos++;
    } else { // 其他類型的 TLV，直接跳過
      pos++; 
      if (pos < 64) {
          pos += rawCardData[pos] + 1;
      }
    }
  }

  Serial.println("在卡片中未找到有效的 NDEF 訊息。");
  return 0;
}


void loop() {
  uint8_t success;
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t uidLength;

  success = nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength);

  if (success) {
    Serial.println("-------------------");
    Serial.println("偵測到卡片！");
    
    int pokemonId = getPokemonIdFromCard();

    if (pokemonId > 0) {
      myData.pokemon_id = pokemonId;
      Serial.print("成功解析寶可夢 ID: ");
      Serial.println(myData.pokemon_id);
      
      Serial.println("正在透過 ESP-NOW 發送...");
      esp_err_t result = esp_now_send(broadcastAddress, (uint8_t *) &myData, sizeof(myData));
     
      if (result != ESP_OK) {
        Serial.println("傳送指令執行時發生錯誤");
      }
    } else {
      Serial.println("無法從卡片中讀取有效的寶可夢 ID。");
    }
    
    delay(2000); // 避免立即重複讀取
  }
}

