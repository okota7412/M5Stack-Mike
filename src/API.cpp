#include "API.h"
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

// コンストラクタ: APIのベースURLを設定
MyApi::MyApi(const String& baseUrl) : _baseUrl(baseUrl) {}

// ログイン
String MyApi::loginToApi(const String& ID, const String& password) {
  String jsonBody = "{ \"loginId\": \"" + ID + "\", \"password\": \"" + password + "\" }";
  return httpPostJson("/auth/login", jsonBody);
}

// テナント一覧ゲット
String MyApi::get_Tenants(const String& token) {
  return httpGet("/tenants", token);
}

//ログインユーザー情報ゲット
String MyApi::get_Me(const String& token) {
    return httpGet("/auth/me", token);
  }


String MyApi::get_Chats(const String& token){
    return httpGet("/chats", token);
}




// HTTP POST リクエストの共通処理
String MyApi::httpPostJson(const String& endpoint, const String& jsonBody, const String& token) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = _baseUrl + endpoint;
  String payload;

  if (http.begin(client, url)) {
    http.addHeader("Content-Type", "application/json");
    if (token.length() > 0) {
      http.addHeader("Authorization", "Bearer " + token);
    }

    int httpCode = http.POST(jsonBody);
    if (httpCode > 0) {
      payload = http.getString();
    } else {
      payload = "[HTTP] POST failed, error: " + String(http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    payload = "[HTTP] Unable to connect " + url;
  }

  return payload;
}

// HTTP GET リクエストの共通処理
String MyApi::httpGet(const String& endpoint, const String& token) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  String url = _baseUrl + endpoint;
  String payload;

  if (http.begin(client, url)) {
    if (token.length() > 0) {
      http.addHeader("Authorization", "Bearer " + token);
    }

    int httpCode = http.GET();
    if (httpCode > 0) {
      payload = http.getString();
    } else {
      payload = "[HTTP] GET failed, error: " + String(http.errorToString(httpCode).c_str());
    }
    http.end();
  } else {
    payload = "[HTTP] Unable to connect " + url;
  }

  return payload;
}
