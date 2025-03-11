#include <M5Core2.h>
#include <driver/i2s.h>
#include "SD.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include "mbedtls/base64.h"
#include <ArduinoJson.h>
#include "CUF_24px.h"
#include <iostream>
#include <string>

//=============================
//  リングバッファ関連
//=============================
#define BUFFER_SIZE (16000 * 2 * 10) // 16bit(2byte)×16000Hz×1秒×8秒 = 256,000バイト
uint8_t* audioRingBuffer = nullptr;
static uint8_t* tempBuffer = nullptr;

// 書き込みインデックスと読み出しインデックスを分離
volatile size_t writeIndex = 0;
volatile size_t readIndex  = 0;

// 書き込み・読み取り時の競合を防ぐためのMutex
SemaphoreHandle_t ringBufferMutex;

//=============================
//  変数や定数
//=============================
uint8_t buffer[1024];  
size_t bytesRead = 0;

int cursol = 0;
bool btnC = false; //グローバルボタン検知変数

// OpenAI Whisper APIのホストとポート
const char* host = "api.openai.com";
const int httpsPort = 443;

// レコーディングファイル名
const char* fileName = "recording.wav";

File recordingFile; // SDカードに書き込む用
File chunkFile;

// APIキーの定義
char *api_key = ""; // 適宜置き換え

// WiFiパスワード設定
const char* ssid = "30F772C441EE-2G";
const char* password = "2215093902304";

// 録音状態を管理するフラグ
bool isRecording = false;

// ==== I2S設定 ==== //
static const i2s_port_t I2S_PORT = I2S_NUM_0;

//=============================
//  関数プロトタイプ
//=============================
void readFileFromSD(const char *filename);
void transcribeAudio();
std::string displayTranscription(const std::string& response);
void writeWavHeader(File file, int sampleRate, int bitsPerSample, int numChannels);
void updateWavHeader(File file);
void CtranscribeAudio(int cursol);

//=============================
//  タスク 0 (録音・書き込み用)
//=============================
void task0(void* arg) {
  while (1) {
    // 右ボタンを押すと録音開始/停止をトグル
    if (btnC) {
      Serial.println("btnC pressed");
      if (!isRecording) {
        //=== 録音開始 ===
        isRecording = true;
        SD.remove("/recording.wav");
        recordingFile = SD.open("/recording.wav", FILE_APPEND);
        writeWavHeader(recordingFile, 16000, 16, 1);

        if (!recordingFile) {
          M5.Lcd.println("Failed to open file for recording!");
          isRecording = false;
          return;
        }
        // chunkFile はここでは開かない
        // chunk処理はtask1側で行う

        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.println("Recording... (Press BtnC again to stop)");

      } else {
        //=== 録音停止 ===
        isRecording = false;

        M5.Lcd.fillScreen(TFT_BLACK);
        M5.Lcd.setCursor(0, 0);
        M5.Lcd.println("Recording Stopped!");
        Serial.println("recording stopped");

        // WAVヘッダ更新してファイルをクローズ
        updateWavHeader(recordingFile);
        recordingFile.close();

        // テスト用に transcribeAudio() を呼ぶ
        recordingFile = SD.open("/recording.wav", FILE_READ);
        readFileFromSD("/recording.wav");
        transcribeAudio();
        recordingFile.close();
        M5.Lcd.setCursor(0, 0);
        cursol = 0;
      }
    }

    // 録音中はI2Sからバッファを取得しSDに書き込み＋リングバッファに書き込み
    if (isRecording) {
      esp_err_t result = i2s_read(I2S_PORT, buffer, sizeof(buffer), &bytesRead, pdMS_TO_TICKS(100));
      if (result == ESP_OK && bytesRead > 0) {
        // SDにも直接書き込む
        recordingFile.write(buffer, bytesRead);

        // リングバッファに書き込み (排他制御)
        if (xSemaphoreTake(ringBufferMutex, (TickType_t)10) == pdTRUE) {
          for (size_t i = 0; i < bytesRead; i++) {
            audioRingBuffer[writeIndex] = buffer[i];
            writeIndex = (writeIndex + 1) % BUFFER_SIZE;
          }
          xSemaphoreGive(ringBufferMutex);
        }
      } else {
        Serial.println("I2S read failed or returned 0 bytes");
      }
    }

    // 30msおきにチェック
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

//=============================
//  タスク 1 (5秒おきにデータを取得しAPI送信)
//=============================
void task1(void* arg) {
  // 5秒インターバル
  const TickType_t interval = pdMS_TO_TICKS(3000);
  TickType_t lastWakeTime = xTaskGetTickCount();

  while (1) {
    // ここで「3秒おきに」リングバッファからデータを抜き出し chunkFile.wav へ書き込む
    // まずは待機
    vTaskDelayUntil(&lastWakeTime, interval);

    if (isRecording) {
      Serial.println("task1 isRecording: reading from ring buffer");

      // リングバッファから未処理分を読み取る
      size_t available = 0;
      if (xSemaphoreTake(ringBufferMutex, (TickType_t)100) == pdTRUE) {
        // いまの書き込み位置をコピーして、
        size_t currentWriteIndex = writeIndex;
        // (writeIndex - readIndex + BUFFER_SIZE) % BUFFER_SIZE が未読バイト数
        available = (currentWriteIndex + BUFFER_SIZE - readIndex) % BUFFER_SIZE;

        // 大きすぎると chunkFile が巨大になるので、必要なら制限
        // ここではとりあえず全部読み出す
        for (size_t i = 0; i < available; i++) {
          tempBuffer[i] = audioRingBuffer[readIndex];
          readIndex = (readIndex + 1) % BUFFER_SIZE;
        }
        xSemaphoreGive(ringBufferMutex);
      }

      if (available == 0) {
        Serial.println("No new data in ring buffer");
        continue; // 次の3秒へ
      }

      // chunkファイルに書き込み
      SD.remove("/chunkfile.wav");
      chunkFile = SD.open("/chunkfile.wav", FILE_WRITE);
      writeWavHeader(chunkFile, 16000, 16, 1);
      chunkFile.write(tempBuffer, available);
      updateWavHeader(chunkFile);
      chunkFile.flush();
      chunkFile.close();

      // APIに投げる
      chunkFile = SD.open("/chunkfile.wav", FILE_READ);
      if (chunkFile) {
        CtranscribeAudio(cursol);
        chunkFile.close();
      }

      // 画面への表示位置を少し下げる
      cursol += 25;
    }
  }
}

//=============================
//  setup()
//=============================
void setup() {
  // M5Core2の初期化
  M5.begin(true, true, true, true);
  M5.Axp.SetLDOEnable(2, true);
  M5.Lcd.setFreeFont(&unicode_24px);
  Serial.begin(115200);

  // LCD初期表示
  M5.Lcd.setTextSize(0.5);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.println("Press Right Button (BtnC) to Start Recording");

  // SDカードの存在確認
  if (!SD.begin()) {
    M5.Lcd.println("SD Card Mount Failed!");
    return;
  }

  // WiFiの接続とローカルIPの確認
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
  }
  M5.Lcd.println("WiFi Connected!");
  M5.Lcd.println(WiFi.localIP());

  // PSRAMにメモリを確保
  audioRingBuffer = (uint8_t*) ps_malloc(BUFFER_SIZE);
  tempBuffer = (uint8_t*) ps_malloc(BUFFER_SIZE);  // <-- こちらもPSRAMに確保
   if (!audioRingBuffer || !tempBuffer) {
       Serial.println("Failed to allocate buffers in PSRAM!");
       while(1);
   }
  Serial.println("Successfully allocated buffer in PSRAM");

  // リングバッファ用Mutexを生成
  ringBufferMutex = xSemaphoreCreateMutex();
  if (ringBufferMutex == NULL) {
    Serial.println("Failed to create ringBuffer mutex!");
    while (1);
  }

  // ==== I2Sの初期化 ====
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate = 16000,                      
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT, 
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false
  };

  i2s_pin_config_t pin_config = {
    .bck_io_num = 12,   // BCKピン
    .ws_io_num = 0,     // LRCKピン
    .data_out_num = -1, // 出力不要（録音のみ）
    .data_in_num = 34   // DINピン
  };

  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);
  i2s_set_clk(I2S_PORT, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

  // Task作成
  xTaskCreatePinnedToCore(task0, "Task0", 16384, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(task1, "Task1", 16384, NULL, 1, NULL, 1);
}

void loop() {
  M5.update();
  if (M5.BtnC.wasPressed()) {
    // BtnCを押したフラグを立てる
    btnC = true;
    vTaskDelay(pdMS_TO_TICKS(50));
    btnC = false;
  }
}

//=============================
//  SDカード上のWAVファイルを確認
//=============================
void readFileFromSD(const char *filename) {
  File file = SD.open(filename, FILE_READ);
  if (!file) {
    Serial.printf("Failed to open %s\n", filename);
    return;
  }

  Serial.printf("%s File Size: %d bytes\n", filename, file.size());

  Serial.println("First 64 bytes of file (hex):");
  for (int i = 0; i < 64; i++) {
    if (file.available()) {
      Serial.printf("%02X ", file.read());
    } else {
      break;
    }
  }
  Serial.println("\nEnd of File.");
  file.close();
}

//=============================
//  OpenAI API へ音声ファイルを送信 (Whisper)
//=============================
void transcribeAudio() {
  WiFiClientSecure client;
  client.setInsecure();
  Serial.println("OpenAI APIへ接続中...");
  if (!client.connect(host, httpsPort)) {
    Serial.println("OpenAI APIへの接続に失敗しました");
    return;
  }
  size_t fileSize = recordingFile.size();
  Serial.printf("recording.wavサイズ: %d バイト\n", fileSize);

  // マルチパートフォームの境界文字列
  String boundary = "----M5StackBoundary";

  // 各フォームパート
  String partModel =
    "--" + boundary + "\r\n" +
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n" +
    "whisper-1\r\n";

  String partLanguage =
    "--" + boundary + "\r\n" +
    "Content-Disposition: form-data; name=\"language\"\r\n\r\n" +
    "ja\r\n";

  String partFileHeader =
    "--" + boundary + "\r\n" +
    "Content-Disposition: form-data; name=\"file\"; filename=\"recording.wav\"\r\n" +
    "Content-Type: audio/wav\r\n\r\n";

  String partEnd = "\r\n--" + boundary + "--\r\n";

  size_t contentLength =
    partModel.length() +
    partLanguage.length() +
    partFileHeader.length() +
    fileSize +
    partEnd.length();

  String request =
    String("POST ") + "/v1/audio/transcriptions" + " HTTP/1.1\r\n" +
    "Host: " + host + "\r\n" +
    "Authorization: Bearer " + String(api_key) + "\r\n" +
    "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
    "Content-Length: " + String(contentLength) + "\r\n" +
    "Connection: close\r\n\r\n";

  // ヘッダー送信
  client.print(request);
  Serial.println("リクエストヘッダー送信完了");

  // 各パート送信
  client.print(partModel);
  client.print(partLanguage);
  client.print(partFileHeader);

  // ファイルデータを送信
  uint8_t buf[512];
  while (recordingFile.available()) {
    int n = recordingFile.read(buf, sizeof(buf));
    client.write(buf, n);
  }
  recordingFile.close();

  // 締め
  client.print(partEnd);
  Serial.println("リクエストボディ送信完了");

  // レスポンス受信
  String response = "";
  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 10000) {
    while (client.available()) {
      char c = client.read();
      response += c;
    }
  }
  Serial.println("APIレスポンス:");
  Serial.println(response);

  // 認識結果を表示
  M5.Lcd.fillScreen(TFT_BLACK);
  M5.Lcd.setCursor(0, 0);
  M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);
  M5.Lcd.setTextSize(0.5);

  String text = displayTranscription(response.c_str()).c_str();
  Serial.println(text);
  M5.Lcd.drawString(text, 0, 0, 1);
}

//=============================
//  JSONレスポンスから "text":"～" を抜き出し
//=============================
std::string displayTranscription(const std::string& response) {
  const std::string pattern = "{\"text\":\"";
  size_t startPos = response.find(pattern);
  if (startPos == std::string::npos) {
    return "";
  }
  startPos += pattern.length();
  size_t endPos = response.find("\"", startPos);
  if (endPos == std::string::npos) {
    return "";
  }
  return response.substr(startPos, endPos - startPos);
}

//=============================
//  WAVヘッダーを書き込む
//=============================
void writeWavHeader(File file, int sampleRate, int bitsPerSample, int numChannels) {
  uint32_t fileSize = 0; // 後で更新
  uint32_t dataChunkSize = 0;

  uint8_t wavHeader[44] = {
    'R','I','F','F',
    (uint8_t)(fileSize      ), (uint8_t)(fileSize >> 8 ), (uint8_t)(fileSize >>16 ), (uint8_t)(fileSize >>24 ),
    'W','A','V','E',
    'f','m','t',' ',
    16,0,0,0,
    1,0,
    (uint8_t)numChannels, (uint8_t)(numChannels >> 8),
    (uint8_t)(sampleRate      ), (uint8_t)(sampleRate >> 8 ), (uint8_t)(sampleRate >>16 ), (uint8_t)(sampleRate >>24 ),
    (uint8_t)((sampleRate * numChannels * bitsPerSample/8)      ),
    (uint8_t)((sampleRate * numChannels * bitsPerSample/8) >> 8 ),
    (uint8_t)((sampleRate * numChannels * bitsPerSample/8) >>16 ),
    (uint8_t)((sampleRate * numChannels * bitsPerSample/8) >>24 ),
    (uint8_t)(numChannels * bitsPerSample/8),0,
    (uint8_t)bitsPerSample,0,
    'd','a','t','a',
    (uint8_t)(dataChunkSize      ), (uint8_t)(dataChunkSize >> 8 ),
    (uint8_t)(dataChunkSize >>16 ), (uint8_t)(dataChunkSize >>24 )
  };

  file.write(wavHeader, 44);
}

//=============================
//  WAVヘッダーのサイズ更新
//=============================
void updateWavHeader(File file) {
  if (!file) return;
  uint32_t fileSize = file.size();
  uint32_t dataChunkSize = fileSize - 44; // 実際のデータサイズ

  file.seek(4);
  file.write((uint8_t *)&fileSize, 4);

  file.seek(40);
  file.write((uint8_t *)&dataChunkSize, 4);
}

//=============================
//  chunkFile を OpenAI APIに投げる
//=============================
void CtranscribeAudio(int cursol) {
  WiFiClientSecure client;
  client.setInsecure();
  Serial.println("OpenAI APIへ接続中...");
  if (!client.connect(host, httpsPort)) {
    Serial.println("OpenAI APIへの接続に失敗しました");
    return;
  }
  size_t fileSize = chunkFile.size();
  Serial.printf("chunkfile.wavサイズ: %d バイト\n", fileSize);

  String boundary = "----M5StackBoundary";
  String partModel =
    "--" + boundary + "\r\n" +
    "Content-Disposition: form-data; name=\"model\"\r\n\r\n" +
    "whisper-1\r\n";
  String partLanguage =
    "--" + boundary + "\r\n" +
    "Content-Disposition: form-data; name=\"language\"\r\n\r\n" +
    "ja\r\n";
  String partFileHeader =
    "--" + boundary + "\r\n" +
    "Content-Disposition: form-data; name=\"file\"; filename=\"chunkfile.wav\"\r\n" +
    "Content-Type: audio/wav\r\n\r\n";
  String partEnd = "\r\n--" + boundary + "--\r\n";

  size_t contentLength =
    partModel.length() +
    partLanguage.length() +
    partFileHeader.length() +
    fileSize +
    partEnd.length();

  String request =
    String("POST ") + "/v1/audio/transcriptions" + " HTTP/1.1\r\n" +
    "Host: " + host + "\r\n" +
    "Authorization: Bearer " + String(api_key) + "\r\n" +
    "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n" +
    "Content-Length: " + String(contentLength) + "\r\n" +
    "Connection: close\r\n\r\n";

  client.print(request);
  Serial.println("リクエストヘッダー送信完了");

  client.print(partModel);
  client.print(partLanguage);
  client.print(partFileHeader);

  // chunkFileの実データ送信
  uint8_t buf[512];
  while (chunkFile.available()) {
    int n = chunkFile.read(buf, sizeof(buf));
    client.write(buf, n);
  }

  client.print(partEnd);
  Serial.println("リクエストボディ送信完了");

  // レスポンス受信
  String response = "";
  unsigned long timeout = millis();
  while (client.connected() && millis() - timeout < 10000) {
    while (client.available()) {
      char c = client.read();
      response += c;
    }
  }

  Serial.println("APIレスポンス:");
  Serial.println(response);

  // 認識結果を表示
  String text = displayTranscription(response.c_str()).c_str();
  Serial.println(text);
  M5.Lcd.drawString(text, 0, cursol, 1);
}
