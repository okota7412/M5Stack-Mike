#ifndef MY_API_H
#define MY_API_H

#include <Arduino.h>

class MyApi {
public:
  // コンストラクタ (API のベースURLを設定)
  MyApi(const String& baseUrl = "https://nagara-care-api.onrender.com");

  //ログイン
  String loginToApi(const String& ID, const String& password);


  // `GET /tenants`
  String get_Tenants(const String& token);

  String get_Me(const String& token);

  String get_Chats(const String& token);


private:
  String _baseUrl;

  //HtTP POST　リクエストの共通関数
  String httpPostJson(const String& endpoint, const String& jsonBody, const String& token = "");

  // HTTP GET リクエストの共通関数
  String httpGet(const String& endpoint, const String& token = "");
};

#endif
