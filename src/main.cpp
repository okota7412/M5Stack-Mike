#include <M5Core2.h>
#include <driver/i2s.h>
#include "SD.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "mbedtls/base64.h"
#include <ArduinoJson.h>
#include "CUF_24px.h"
//#include "efont.h"                                          // Unicode表示用ライブラリ
//#include "efontESP32.h"                                     // Unicode表示用ライブラリ　
//#include "efontEnableJa.h"                                  // Unicode表示用ライブラリ（日本語）
File recordingFile; // SDカードに書き込む用のファイル変数

//APIキーの定義
char *api_key = getenv("GOOGLE_API_KEY");
//バケット名
const char* bucketName = "dcon_bucket";
//レコーディングファイル名
const char* fileName = "recording.wav";

//署名付きURL
const char* signed_url = "https://storage.googleapis.com/dcon_bucket/recording.wav?x-goog-signature=486f3ad63f1b2cfcfa1b04241853e8d5b37e18bd8ed8a4a5267ec9813fdc8da43e0d875b312a2643577863d7f23f534d32a55b30bda623773dae1a9883471df10878e56ff00dd8d21172de44b677c715e176e5a5ff7ff9558cf2ffc3d3820b2da888a7e163a61a6bb58ec3022d28567a61d587007ceaeadfe7bc708b2f0075e81861c0f12026397660f0469a7ea09f503e2ef082ab3d9fbe870d06431e3891419785c9dae12e860e9de743085a0db23de54f0505c1a8eb50b2065f257e21ba0d065d09b58596f1e7585b3b297942b34cee9570c7d7c4afbe430efc013d80c611b7b66dc1bc2a7526685f577c1810fcdd371d758241e658665a0f3e37b0881e9d&x-goog-algorithm=GOOG4-RSA-SHA256&x-goog-credential=dconproject%40analog-forest-451916-d4.iam.gserviceaccount.com%2F20250304%2Fasia1%2Fstorage%2Fgoog4_request&x-goog-date=20250304T110943Z&x-goog-expires=3600&x-goog-signedheaders=host";
//関数プロトタイプ宣言         
void readRecordingFile(); 
void readFileFromSD(const char *filename);
void uploadToGCS2();
void transcribeAudio();
void displayTranscription(String jsonResponse);
void writeWavHeader(File file, int sampleRate, int bitsPerSample, int numChannels);
void updateWavHeader(File file);

//WiFiパスワード設定
const char* ssid = "30F772C441EE-2G";
const char* password = "2215093902304";

// ==== I2S設定 ==== //
static const i2s_port_t I2S_PORT = I2S_NUM_0;

// 録音状態を管理するフラグ
bool isRecording = false;

void setup() {
  // M5Core2の初期化
  M5.begin(true, true, true, true);
  M5.Axp.SetLDOEnable(2, true);
  M5.Lcd.setFreeFont(&unicode_24px);
  //M5.Lcd.setTextDatum(TC_DATUM);

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

  //WiFiの接続とローカルIPの確認
  WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
    }
    M5.Lcd.println("WiFi Connected!");
    M5.Lcd.println(WiFi.localIP());
    int nextY = M5.Lcd.getCursorY();
    



  // ==== I2Sの初期化 ====
  // 1. I2Sの設定
  i2s_config_t i2s_config = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
    .sample_rate = 16000,                       // サンプリング周波数
    .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT, // 16bit
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,  // 左チャンネルのみ
    .communication_format = I2S_COMM_FORMAT_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 4,
    .dma_buf_len = 1024,
    .use_apll = false
  };

  // 2. I2Sピン設定（M5Core2用）
  i2s_pin_config_t pin_config = {
    .bck_io_num = 12,   // BCKピン
    .ws_io_num = 0,     // LRCKピン
    .data_out_num = -1, // 出力不要（録音のみ）
    .data_in_num = 34   // DINピン
  };

  // 3. I2Sドライバをインストール
  i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_PORT, &pin_config);

  // 4. 入力データを16bit幅として正しく取得するための設定
  i2s_set_clk(I2S_PORT, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);

}

void loop() {
  // 毎フレーム、M5のタッチやボタン状態を更新
  M5.update();

  // 右ボタン（BtnC）を押したら 録音の開始/停止 を切り替え
  if (M5.BtnC.wasPressed()) {
    if (!isRecording) {
      // === 録音開始 ===
      isRecording = true;
      SD.remove("/recording.wav");
      recordingFile = SD.open("/recording.wav", FILE_WRITE);
      writeWavHeader(recordingFile, 16000, 16, 1);
      if (!recordingFile) {
        M5.Lcd.println("Failed to open file for recording!");
        isRecording = false;
        return;
      }

      M5.Lcd.fillScreen(TFT_BLACK);
      M5.Lcd.setCursor(0, 0);
      M5.Lcd.println("Recording... (Press BtnC again to stop)");
    } else {
      // === 録音停止 ===
      isRecording = false;
      recordingFile.close();

      M5.Lcd.fillScreen(TFT_BLACK);
      M5.Lcd.setCursor(0, 0);
      M5.Lcd.println("Recording Stopped!");
      //録音データをAPIで日本語化、画面に表示
      updateWavHeader(recordingFile);
      uploadToGCS2();
      transcribeAudio();  // 日本語化処理を実行
      //sendAudioToGoogle();
    }
  }

  //録音中
  if (isRecording) {
    uint8_t buffer[1024];  
    size_t bytesRead = 0;
    esp_err_t result = i2s_read(I2S_PORT, buffer, sizeof(buffer), &bytesRead, pdMS_TO_TICKS(100));

    if (result == ESP_OK && bytesRead > 0) {
        for (int i = 0; i < 10 && i < bytesRead; i++) {
          Serial.printf("%02X ", buffer[i]); // 16進数で表示
      }
        recordingFile.write(buffer, bytesRead);
    } else {
        Serial.println("I2S read failed or returned 0 bytes");
    }
}

}

//SDカードの中身の確認
void readFileFromSD(const char *filename) {
  File file = SD.open("/recording.wav", FILE_READ);
  if (!file) {
      Serial.println("Failed to open recording.wav");
      return;
  }

  Serial.printf("WAV File Size: %d bytes\n", file.size());

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





//APIたたいて日本語化
void transcribeAudio() {
  HTTPClient http;
  String url = "https://speech.googleapis.com/v1/speech:recognize";
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  
  // アクセストークン
  String accessToken = "ya29.c.c0ASRK0GYTd3mWrrQ-IpT0RVnDTYD-NeiBrqIInOUyEKvcqJ4r3wPQADA-eh8rEHx_iYwzbCQzBkGZTMxgMZknX20N6sepzWibMO-l13TsAeKKwvuJKuc1Ikjcu120pe4WsSA_xNAhNvVHpRdbj3pzn5SO8tTCwQlTrpi0VlTDDjN2_WN_FoTK5VhHUqU1Lm0iQUJkRXabpVMMWH3U2oBrg-kVPSVo8bAKTc8n4SIw8M2-oieSZUfJRFIl4F7Z-4qqeQJ6BFMrWCHKAU1kf0Cg7jfUzxmUp-z3Hedv8R3JET5EUMhkwCzhF_cHF3-6Cood6Xj8gFmIWCylaR0D-jFocLsxMEHP_gABm9xNVvAT41-I9jUa5IFnF5vPnAL387Pi-O8atv57_QS_druOmMi1erSx2FlW1hg4gqW6u2Ooljv71r7exvQqlSUxkXunR6_8YfF3prUF8vpnqutvrWUfRr9BSXJ6Od5fRpi9d6V53fgOljkkYkY_w1B4VJZkSMJQVyroMQ4XcU0JtWf3y-JupbsRBeU7apQ67yFoJ8UgF1j1lq63Z8Yk2O3l2F3wr-u74vjx_JOhV_9BgYoky1Bg2kr8uJi-t2Zzmb0SWZasu4YR-yniUsztj0wBlaRZOljYSu1Qx44gvaBOZ3x_hwzQQsg4anhrtojWbjhO0xcZv9YSXQm0-eOWxsf_q5och6_Jp72wrcRJpMa7kt85_kph4Vrueh-rjqSs0B8dFgWkoQRgvuV0hYVUy3wZ-gnnVjBilnp2_2kd37hOaFIpZsOY3S-Jaq44dj2z0uh-byekW8y16p5z8QeUIJlr59Q8Ug2cJStYyfpU6yZdJQtzSm0tIYpkgWjekr9d8xmh0IfXXtuV53Sbc6R7a-rzYw8ssg__z2ir9lRtcXpl4lVo9pxobw7gqFVfbJhsmico6wzzBSUbsv7qolZ2-vBzUwWWydsfphQRw3cIiYeFa-eF6cWOkXRqF25ZuoQblsquFWfeb46je7-yVM6JMgs";  // コピーしたトークンをここに貼る
  http.addHeader("Authorization", "Bearer " + accessToken);

  String jsonPayload = "{"
      "\"config\": {"
          "\"encoding\": \"LINEAR16\","
          "\"sampleRateHertz\": 16000,"
          "\"languageCode\": \"ja-JP\","
          "\"enableAutomaticPunctuation\": true"
      "},"
      "\"audio\": {"
          "\"uri\": \"gs://dcon_bucket/recording.wav\""
      "}"
  "}";

  int httpResponseCode = http.POST(jsonPayload);
  String response = http.getString();
  http.end();

  Serial.println("Speech-to-Text API Response:");
  Serial.println(response);

  displayTranscription(response);
}


// ==== 取得したJSONから日本語テキストを抽出 & 表示 ====
void displayTranscription(String jsonResponse) {
  M5.Lcd.fillScreen(TFT_BLACK);
    M5.Lcd.setCursor(0, 0);
    Serial.println("Parsing JSON response...");

    // JSONドキュメントを作成
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, jsonResponse);

    if (error) {
        M5.Lcd.println("JSON解析エラー...");
        Serial.println("JSON解析エラー: " + String(error.c_str()));
        return;
    }

    // `results` 配列が存在するかチェック
    if (!doc.containsKey("results") || doc["results"].size() == 0) {
        M5.Lcd.println("認識結果が見つかりませんでした");
        Serial.println("認識結果が見つかりませんでした");
        return;
    }

    // `results` の中の `alternatives` の `transcript` を取得
    String transcript = "";
    for (JsonObject result : doc["results"].as<JsonArray>()) {
        if (result.containsKey("alternatives") && result["alternatives"].size() > 0) {
            transcript += result["alternatives"][0]["transcript"].as<String>() + " ";
        }
    }

    // 取得した認識結果を表示
    M5.Lcd.fillScreen(TFT_BLACK);  // 画面を黒でクリア
    M5.Lcd.setCursor(0, 0);
    M5.Lcd.setTextColor(TFT_WHITE, TFT_BLACK);  // 文字色と背景色を指定
    M5.Lcd.setTextSize(0.5);

    if (transcript.length() > 0) {
        M5.Lcd.drawString(transcript.c_str(), 0, 0, 1);
        Serial.println("認識結果:");
        Serial.println(transcript);
    } else {
        M5.Lcd.println("認識結果が見つかりませんでした");
        Serial.println("認識結果が見つかりませんでした");
    }
}



//Google Cloud Strageにファイルをアップロード
void uploadToGCS2() {
  if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi not connected!");
      return;
  }

  File file = SD.open("/recording.wav", FILE_READ);
  if (!file) {
      Serial.println("Failed to open /recording.wav");
      return;
  }

  HTTPClient http;
  http.begin(signed_url);
  http.addHeader("Content-Type", "audio/wav");

  Serial.println("Uploading WAV file to GCS...");

  // HTTP PUT でファイルをアップロード
  int httpResponseCode = http.sendRequest("PUT", &file, file.size());

  if (httpResponseCode > 0) {
      Serial.println("Upload successful! Response code: " + String(httpResponseCode));
  } else {
      Serial.println("Upload failed! Error code: " + String(httpResponseCode));
  }

  http.end();
  file.close();
}


//wavヘッダーをちゃんとする
void writeWavHeader(File file, int sampleRate, int bitsPerSample, int numChannels) {
  uint32_t fileSize = 0; // 最後に更新する
  uint32_t dataChunkSize = 0;

  // WAVヘッダー（44バイト）
  uint8_t wavHeader[44] = {
      'R', 'I', 'F', 'F', fileSize, fileSize >> 8, fileSize >> 16, fileSize >> 24,
      'W', 'A', 'V', 'E', 
      'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 
      numChannels, 0, 
      sampleRate, sampleRate >> 8, sampleRate >> 16, sampleRate >> 24,
      (sampleRate * numChannels * bitsPerSample / 8), 0, 0, 0, 
      (numChannels * bitsPerSample / 8), 0, 
      bitsPerSample, 0, 
      'd', 'a', 't', 'a', dataChunkSize, dataChunkSize >> 8, dataChunkSize >> 16, dataChunkSize >> 24
  };

  file.write(wavHeader, 44);
}


//wavヘッダーのデータサイズ部分の更新
void updateWavHeader(File file) {
  if (!file) return;

  uint32_t fileSize = file.size();
  uint32_t dataChunkSize = fileSize - 44; // ヘッダーを除いたデータサイズ

  file.seek(4);
  file.write((uint8_t *)&fileSize, 4);

  file.seek(40);
  file.write((uint8_t *)&dataChunkSize, 4);
}


