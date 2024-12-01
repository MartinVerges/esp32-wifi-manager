/**
 * Wifi Manager
 * (c) 2022-2024 Martin Verges
 *
 * Licensed under CC BY-NC-SA 4.0
 * (Attribution-NonCommercial-ShareAlike 4.0 International)
**/
#include "wifimanager.h"
#include "AsyncJson.h"
#include "ArduinoJson.h"
#if ASYNC_WEBSERVER == true
  #include <ESPAsyncWebServer.h>
#else
  #include <WebServer.h>
#endif
#include <WiFi.h>
#include <Preferences.h>


/**
 * @brief Write a message to the Serial interface
 * @param msg The message to be written
 *
 * This function is a simple wrapper around Serial.print() to write a message
 * to the serial console. It can be overwritten by a custom implementation for 
 * enhanced logging.
 */
void WIFIMANAGER::logMessage(String msg) {
  Serial.print(msg);
}

/**
 * @brief Background Task running as a loop forever
 * @param param needs to be a valid WIFIMANAGER instance
 */
void wifiTask(void* param) {
  yield();
  delay(500); // wait a short time until everything is setup before executing the loop forever
  yield();
  const TickType_t xDelay = 10000 / portTICK_PERIOD_MS;
  WIFIMANAGER * wifimanager = (WIFIMANAGER *) param;

  for(;;) {
    yield();
    wifimanager->loop();
    yield();
    vTaskDelay(xDelay);
  }
}

/**
 * @brief Start the background task, which will take care of the Wifi connection
 *
 * This method will load the configuration from NVS, try to connect to the configured WIFI(s)
 * and then start a background task, which will keep monitoring and trying to reconnect
 * to the configured WIFI(s) in case the connection drops.
 */
void WIFIMANAGER::startBackgroundTask(String softApName, String softApPass) {
  if (softApName.length()) this->softApName = softApName;
  if (softApPass.length()) this->softApPass = softApPass;
  loadFromNVS();
  tryConnect();

  BaseType_t taskCreated = xTaskCreatePinnedToCore(
    wifiTask,
    "WifiManager",
    4096,   // Stack size in words
    this,   // Task input parameter
    1,      // Priority of the task
    &WifiCheckTask,  // Task handle.
    0       // Core where the task should run
  );

  if (taskCreated != pdPASS) {
    logMessage("[ERROR] WifiManager: Error creating background task\n");
  }
}

/**
 * @brief Construct a new WIFIMANAGER::WIFIMANAGER object
 * @details Puts the Wifi mode to AP+STA and registers Wifi Events
 * @param ns Namespace for the preferences non volatile storage (NVS)
 */
WIFIMANAGER::WIFIMANAGER(const char * ns) {
  NVS = (char *)ns;

  // AP on/off
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI] onEvent() AP mode started!\n");
    softApRunning = true;
#if ESP_ARDUINO_VERSION_MAJOR >= 2
    }, ARDUINO_EVENT_WIFI_AP_START); // arduino-esp32 2.0.0 and later
#else
    }, SYSTEM_EVENT_AP_START); // arduino-esp32 1.0.6
#endif
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI] onEvent() AP mode stopped!\n");
    softApRunning = false;
#if ESP_ARDUINO_VERSION_MAJOR >= 2
    }, ARDUINO_EVENT_WIFI_AP_STOP); // arduino-esp32 2.0.0 and later
#else
    }, SYSTEM_EVENT_AP_STOP); // arduino-esp32 1.0.6
#endif
  // AP client join/leave
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI] onEvent() new client connected to softAP!\n");
#if ESP_ARDUINO_VERSION_MAJOR >= 2
    }, ARDUINO_EVENT_WIFI_AP_STACONNECTED); // arduino-esp32 2.0.0 and later
#else
    }, SYSTEM_EVENT_AP_STACONNECTED); // arduino-esp32 1.0.6
#endif
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI] onEvent() Client disconnected from softAP!\n");
#if ESP_ARDUINO_VERSION_MAJOR >= 2
    }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED); // arduino-esp32 2.0.0 and later
#else
    }, SYSTEM_EVENT_AP_STADISCONNECTED); // arduino-esp32 1.0.6
#endif
}

/**
 * @brief Destroy the WIFIMANAGER::WIFIMANAGER object
 * @details will stop the background task as well but not cleanup the AsyncWebserver
 */
WIFIMANAGER::~WIFIMANAGER() {
  vTaskDelete(WifiCheckTask);
  // FIXME: get rid of the registered Webserver AsyncCallbackWebHandlers
}

/**
 * @brief If no WIFI is available, fallback to create an AP on the ESP32
 * @param state boolean true (create AP) or false (don't create an AP)
 */
void WIFIMANAGER::fallbackToSoftAp(const bool state) {
  createFallbackAP = state;
}

/**
 * @brief Get the current configured fallback state
 * @return true
 * @return false
 */
bool WIFIMANAGER::getFallbackState() {
  return createFallbackAP;
}

/**
 * @brief Remove all entries from the current known and configured Wifi list
 * @details This only affects memory, not the storage!
 * @details If you wan't to persist this, you need to call writeToNVS()
 */
void WIFIMANAGER::clearApList() {
  for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
    apList[i].apName = "";
    apList[i].apPass = "";
  }
}

/**
 * @brief Load last saved configuration from the NVS into the memory
 * @return true on success
 * @return false on error
 */
bool WIFIMANAGER::loadFromNVS() {
  configuredSSIDs = 0;
  if (preferences.begin(NVS, true)) {
    clearApList();
    char tmpKey[10] = { 0 };
    for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
      sprintf(tmpKey, "apName%d", i);
      if (preferences.getType(tmpKey) == PT_STR) {
        String apName = preferences.getString(tmpKey, "");
        if (apName.length() > 0) {
          sprintf(tmpKey, "apPass%d", i);
          String apPass = preferences.getString(tmpKey);
          logMessage(String("[WIFI] Load SSID '") + apName + "' to " + String(i+1) + ". slot.\n");
          apList[i].apName = apName;
          apList[i].apPass = apPass;
          configuredSSIDs++;
        }
      }
    }
    preferences.end();
    return true;
  }
  logMessage("[WIFI] Unable to load data from NVS, giving up...\n");
  return false;
}

/**
 * @brief Write the current in memory configuration to the non volatile storage
 * @return true on success
 * @return false on error with the NVS
 */
bool WIFIMANAGER::writeToNVS() {
  if (!preferences.begin(NVS, false)) {
    logMessage("[WIFI] Unable to write data to NVS, giving up...");
    return false;
  }

  preferences.clear();
  char tmpKey[10];
  for(uint8_t i = 0; i < WIFIMANAGER_MAX_APS; i++) {
    if (apList[i].apName.isEmpty()) continue;

    snprintf(tmpKey, sizeof(tmpKey), "apName%d", i);
    preferences.putString(tmpKey, apList[i].apName);

    snprintf(tmpKey, sizeof(tmpKey), "apPass%d", i);
    preferences.putString(tmpKey, apList[i].apPass);
  }

  preferences.end();
  return true;
}

/**
 * @brief Add a new WIFI SSID to the known credentials list
 * @param apName Name of the SSID to connect to
 * @param apPass Password (or empty) to connect to the SSID
 * @param updateNVS Write the new entry directly to NVS
 * @return true on success
 * @return false on failure
 */
bool WIFIMANAGER::addWifi(String apName, String apPass, bool updateNVS) {
  if(apName.length() < 1 || apName.length() > 31) {
    logMessage("[WIFI] No SSID given or ssid too long");
    return false;
  }

  if(apPass.length() > 63) {
    logMessage("[WIFI] Passphrase too long");
    return false;
  }

  for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
    if (apList[i].apName == "") {
      logMessage(String("[WIFI] Found unused slot Nr. ") + String(i) + " to store the new SSID '" + apName + "' credentials.\n");
      apList[i].apName = apName;
      apList[i].apPass = apPass;
      configuredSSIDs++;
      if (updateNVS) return writeToNVS();
      else return true;
    }
  }
  logMessage("[WIFI] No slot available to store SSID credentials");
  return false; // max entries reached
}

/**
 * @brief Drop a known SSID entry ID from the known list and write change to NVS
 * @param apId ID of the SSID within the array
 * @return true on success
 * @return false on error
 */
bool WIFIMANAGER::delWifi(uint8_t apId) {
  if (apId < WIFIMANAGER_MAX_APS) {
    apList[apId].apName.clear();
    apList[apId].apPass.clear();
    return writeToNVS();
  }
  return false;
}

/**
 * @brief Drop a known SSID name from the known list and write change to NVS
 * @param apName SSID name
 * @return true on success
 * @return false on error
 */
bool WIFIMANAGER::delWifi(String apName) {
  int num = 0;
  for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
    if (apList[i].apName == apName) {
      if (delWifi(i)) num++;
    }
  }
  return num > 0;
}

/**
 * @brief Provides information about the current configuration state
 * @details When at least 1 SSID is configured, the return value will be true, otherwise false
 * @return true if one or more SSIDs stored
 * @return false if no configuration is available
 */
bool WIFIMANAGER::configAvailable() {
    return configuredSSIDs != 0;
}

/**
 * @brief Provides the apList element id of the first configured slot
 * @details It's used to speed up connection by getting the first available configuration
 * @note only call this function when you have configuredSSIDs > 0, otherwise it will return 0 as well and fail!
 * @return uint8_t apList element id
 */
uint8_t WIFIMANAGER::getApEntry() {
  for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
    if (apList[i].apName.length()) return i;
  }
  logMessage("[WIFI][ERROR] We did not find a valid entry!\n");
  logMessage("[WIFI][ERROR] Make sure to not call this function if configuredSSIDs != 1.\n");
  return 0;
}

/**
 * @brief Background loop function running inside the task
 * @details regulary check if the connection is up&running, try to reconnect or create a fallback AP
 */
void WIFIMANAGER::loop() {
  if (millis() - lastWifiCheckMillis < intervalWifiCheckMillis) return;
  lastWifiCheckMillis = millis();

  if(WiFi.waitForConnectResult() == WL_CONNECTED) {
    // Check if we are connected to a well known SSID
    for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
      if (WiFi.SSID() == apList[i].apName) {
        logMessage(String("[WIFI][STATUS] Connected to known SSID: '") + WiFi.SSID() + "' with IP " + WiFi.localIP().toString() + "\n");
        return;
      }
    }
    // looks like we are connected to something else, strange!?
    logMessage("[WIFI] We are connected to an unknown SSID ignoring. Connected to: " + WiFi.SSID() + "\n");
  } else {
    if (softApRunning) {
      logMessage("[WIFI] Not trying to connect to a known SSID. SoftAP has " + String(WiFi.softAPgetStationNum()) + " clients connected!\n");
    } else {
      // let's try to connect to some WiFi in Range
      if (!tryConnect()) {
        if (createFallbackAP) runSoftAP();
        else logMessage("[WIFI] Auto creation of SoftAP is disabled, no starting AP!\n");
      }
    }
  }

  if (softApRunning && millis() - startApTimeMillis > timeoutApMillis) {
    if (WiFi.softAPgetStationNum() > 0) {
      logMessage("[WIFI] SoftAP has " + String(WiFi.softAPgetStationNum()) + " clients connected!\n");
      startApTimeMillis = millis(); // reset timeout as someone is connected
      return;
    }
    logMessage("[WIFI] Running in AP mode but timeout reached. Closing AP!\n");
    stopSoftAP();
    delay(100);
  }
}

/**
 * @brief Try to connect to one of the configured SSIDs (if available).
 * @details If more than 2 SSIDs configured, scan for available WIFIs and connect to the strongest
 * @return true on success
 * @return false on error or no configuration
 */
bool WIFIMANAGER::tryConnect() {
  if (!configAvailable()) {
    logMessage("[WIFI] No SSIDs configured in NVS, unable to connect\n");
    if (createFallbackAP) runSoftAP();
    return false;
  }

  if (softApRunning) {
    logMessage("[WIFI] Not trying to connect. SoftAP has " + String(WiFi.softAPgetStationNum()) + " clients connected!\n");
    return false;
  }

  int choosenAp = INT_MIN;
  if (configuredSSIDs == 1) {
    // only one configured SSID, skip scanning and try to connect to this specific one.
    choosenAp = getApEntry();
  } else {
    WiFi.mode(WIFI_STA);
    int8_t scanResult = WiFi.scanNetworks(false, true);
    if(scanResult <= 0) {
      logMessage("[WIFI] Unable to find WIFI networks in range to this device!\n");
      return false;
    }
    logMessage(String("[WIFI] Found ") + String(scanResult) + " networks in range\n");
    int choosenRssi = INT_MIN;  // we want to select the strongest signal with the highest priority if we have multiple SSIDs available
    for(int8_t x = 0; x < scanResult; ++x) {
      String ssid;
      uint8_t encryptionType;
      int32_t rssi;
      uint8_t* bssid;
      int32_t channel;
      WiFi.getNetworkInfo(x, ssid, encryptionType, rssi, bssid, channel);
      for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
        if (apList[i].apName.length() == 0 || apList[i].apName != ssid) continue;

        if (rssi > choosenRssi) {
          if(encryptionType == WIFI_AUTH_OPEN || apList[i].apPass.length() > 0) { // open wifi or we do know a password
            choosenAp = i;
            choosenRssi = rssi;
          }
        } // else lower wifi signal
      }
    }
    WiFi.scanDelete();
  }

  if (choosenAp == INT_MIN) {
    logMessage("[WIFI] Unable to find an SSID to connect to!\n");
    return false;
  } else {
    logMessage(String("[WIFI] Trying to connect to SSID ") + apList[choosenAp].apName 
      + " with password " + (apList[choosenAp].apPass.length() > 0 ? "'***'" : "''") + "\n"
    );

    WiFi.begin(apList[choosenAp].apName.c_str(), apList[choosenAp].apPass.c_str());
    wl_status_t status = (wl_status_t)WiFi.waitForConnectResult(5000UL);

    auto startTime = millis();
    // wait for connection, fail, or timeout
    while(status != WL_CONNECTED && status != WL_NO_SSID_AVAIL && status != WL_CONNECT_FAILED && (millis() - startTime) <= 10000) {
        delay(10);
        status = (wl_status_t)WiFi.waitForConnectResult(5000UL);
    }
    switch(status) {
      case WL_IDLE_STATUS:
        logMessage("[WIFI] Connecting failed (0): Idle\n");
        break;
      case WL_NO_SSID_AVAIL:
        logMessage("[WIFI] Connecting failed (1): The AP can't be found\n");
        break;
      case WL_SCAN_COMPLETED:
        logMessage("[WIFI] Connecting failed (2): Scan completed\n");
        break;
      case WL_CONNECTED: // 3
        logMessage("[WIFI] Connection successful\n");
        logMessage("[WIFI] SSID   : " + WiFi.SSID() + "\n");
        logMessage("[WIFI] IP     : " + WiFi.localIP().toString() + "\n");
        stopSoftAP();
        return true;
        break;
      case WL_CONNECT_FAILED:
        logMessage("[WIFI] Connecting failed (4): Unknown reason\n");
        break;
      case WL_CONNECTION_LOST:
        logMessage("[WIFI] Connecting failed (5): Connection lost\n");
        break;
      case WL_DISCONNECTED:
        logMessage("[WIFI] Connecting failed (6): Disconnected\n");
        break;
      case WL_NO_SHIELD:
        logMessage("[WIFI] Connecting failed (7): No Wifi shield found\n");
        break;
      default:
        logMessage("[WIFI] Connecting failed (" + String(status) + "): Unknown status code\n");
        break;
    }
  }
  return false;
}

void WIFIMANAGER::configueSoftAp(String apName, String apPass) {
  this->softApName = apName;
  this->softApPass = apPass;
}

/**
 * @brief Start a SoftAP for direct client access
 * @param apName name of the AP to create (default is ESP_XXXXXXXX)
 * @return true on success
 * @return false o error or if a SoftAP already runs
 */
bool WIFIMANAGER::runSoftAP(String apName, String apPass) {
  if (apName.length()) this->softApName = apName;
  if (apPass.length()) this->softApPass = apPass;

  if (softApRunning) return true;
  startApTimeMillis = millis();

  if (this->softApName == "") this->softApName = "ESP_" + String((uint32_t)ESP.getEfuseMac());
  logMessage("[WIFI] Starting configuration portal on AP SSID " + this->softApName + "\n");

  WiFi.mode(WIFI_AP);
  bool state = WiFi.softAP(this->softApName.c_str(), (this->softApPass.length() ? this->softApPass.c_str() : NULL));
  if (state) {
    IPAddress IP = WiFi.softAPIP();
    logMessage("[WIFI] AP created. My IP is: " + String(IP) + "\n");
    return true;
  } else {
    logMessage("[WIFI] Unable to create SoftAP!\n");
    return false;
  }
}

/**
 * @brief Stop/Disconnect a current running SoftAP
 */
void WIFIMANAGER::stopSoftAP() {
  WiFi.softAPdisconnect();
  WiFi.mode(WIFI_STA);
}

/**
 * @brief Stop/Disconnect a current wifi connection
 */
void WIFIMANAGER::stopClient() {
  WiFi.disconnect();
}

/**
 * @brief Stop/Disconnect all running wifi activies and optionally kill the background task as well
 * @param killTask true to kill the background task to prevent reconnects
 */
void WIFIMANAGER::stopWifi(bool killTask) {
  if (killTask) vTaskDelete(WifiCheckTask);
  stopSoftAP();
  stopClient();
}

/**
 * @brief Attach the WebServer to the WifiManager to register the RESTful API
 * @param srv WebServer object
 */
#if ASYNC_WEBSERVER == true
void WIFIMANAGER::attachWebServer(AsyncWebServer * srv) {
#else
void WIFIMANAGER::attachWebServer(WebServer * srv) {
#endif
  webServer = srv; // store it in the class for later use

#if ASYNC_WEBSERVER == true
  // not required
#else
  // just for debugging
  webServer->onNotFound([&]() {
    String uri = WebServer::urlDecode(webServer->uri());  // required to read paths with blanks

     // Dump debug data
    String message;
    message.reserve(100);
    message = F("Error: File not found\n\nURI: ");
    message += uri;
    message += F("\nMethod: ");
    message += (webServer->method() == HTTP_GET) ? "GET" : "POST";
    message += F("\nArguments: ");
    message += webServer->args();
    message += '\n';
    for (uint8_t i = 0; i < webServer->args(); i++) {
      message += F(" NAME:");
      message += webServer->argName(i);
      message += F("\n VALUE:");
      message += webServer->arg(i);
      message += '\n';
    }
    message += "path=";
    message += webServer->arg("path");
    message += '\n';
    logMessage(message);
  });
#endif

#if ASYNC_WEBSERVER == true
  webServer->on((apiPrefix + "/softap/start").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request) {
    request->send(200, "application/json", "{\"message\":\"Soft AP stopped\"}");
#else
  webServer->on((apiPrefix + "/softap/start").c_str(), HTTP_POST, [&]() {
    webServer->send(200, "application/json", "{\"message\":\"Soft AP stopped\"}");
#endif
    yield();
    delay(250);
    runSoftAP();
  });
  
#if ASYNC_WEBSERVER == true
  webServer->on((apiPrefix + "/softap/stop").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request) {
    request->send(200, "application/json", "{\"message\":\"Soft AP stopped\"}");
#else
  webServer->on((apiPrefix + "/softap/stop").c_str(), HTTP_POST, [&]() {
    webServer->send(200, "application/json", "{\"message\":\"Soft AP stopped\"}");
#endif
    yield();
    delay(250); // It's likely that this message won't go trough, but we give it a short time
    stopSoftAP();
  });

#if ASYNC_WEBSERVER == true
  webServer->on((apiPrefix + "/client/stop").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request) {
    request->send(200, "application/json", "{\"message\":\"Terminating current Wifi connection\"}");
#else
  webServer->on((apiPrefix + "/client/stop").c_str(), HTTP_POST, [&]() {
    webServer->send(200, "application/json", "{\"message\":\"Terminating current Wifi connection\"}");
#endif
    yield();
    delay(500); // It's likely that this message won't go trough, but we give it a short time
    stopClient();
  });

#if ASYNC_WEBSERVER == true
  webServer->on((apiPrefix + "/add").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, (const char*)data);
    auto resp = request;
#else
  webServer->on((apiPrefix + "/add").c_str(), HTTP_POST, [&]() {
    if (webServer->args() != 1) {
      webServer->send(400, "application/json", "{\"message\":\"Bad Request. Only accepting one json body in request!\"}");
    }
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, webServer->arg(0));
    auto resp = webServer;
#endif
    if (!jsonBuffer["apName"].is<String>() || !jsonBuffer["apPass"].is<String>()) {
      resp->send(422, "application/json", "{\"message\":\"Invalid data\"}");
      return;
    }
    if (!addWifi(jsonBuffer["apName"].as<String>(), jsonBuffer["apPass"].as<String>())) {
      resp->send(500, "application/json", "{\"message\":\"Unable to process data\"}");
    } else resp->send(200, "application/json", "{\"message\":\"New AP added\"}");
  });

#if ASYNC_WEBSERVER == true
  webServer->on((apiPrefix + "/id").c_str(), HTTP_DELETE, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, (const char*)data);
    auto resp = request;
#else
  webServer->on((apiPrefix + "/id").c_str(), HTTP_DELETE, [&]() {
    if (webServer->args() != 1) {
      webServer->send(400, "application/json", "{\"message\":\"Bad Request. Only accepting one json body in request!\"}");
    }
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, webServer->arg(0));
    auto resp = webServer;
#endif
    if (!jsonBuffer["id"].is<uint8_t>() || jsonBuffer["id"].as<uint8_t>() >= WIFIMANAGER_MAX_APS) {
      resp->send(422, "application/json", "{\"message\":\"Invalid data\"}");
      return;
    }
    if (!delWifi(jsonBuffer["id"].as<uint8_t>())) {
      resp->send(500, "application/json", "{\"message\":\"Unable to delete entry\"}");
    } else resp->send(200, "application/json", "{\"message\":\"AP deleted\"}");
  });

#if ASYNC_WEBSERVER == true
  webServer->on((apiPrefix + "/apName").c_str(), HTTP_DELETE, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, (const char*)data);
    auto resp = request;
#else
  webServer->on((apiPrefix + "/apName").c_str(), HTTP_DELETE, [&]() {
    if (webServer->args() != 1) {
      webServer->send(400, "application/json", "{\"message\":\"Bad Request. Only accepting one json body in request!\"}");
    }
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, webServer->arg(0));
    auto resp = webServer;
#endif
    if (!jsonBuffer["apName"].is<String>()) {
      resp->send(422, "application/json", "{\"message\":\"Invalid data\"}");
      return;
    }
    if (!delWifi(jsonBuffer["apName"].as<String>())) {
      resp->send(500, "application/json", "{\"message\":\"Unable to delete entry\"}");
    } else resp->send(200, "application/json", "{\"message\":\"AP deleted\"}");
  });

#if ASYNC_WEBSERVER == true
  webServer->on((apiPrefix + "/configlist").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
#else
  webServer->on((apiPrefix + "/configlist").c_str(), HTTP_GET, [&]() {
    String buffer;
#endif
    JsonDocument jsonDoc;
    auto jsonArray = jsonDoc.to<JsonArray>();
    for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
      if (apList[i].apName.length() > 0) {
        JsonObject wifiNet = jsonArray.createNestedObject();
        wifiNet["id"] = i;
        wifiNet["apName"] = apList[i].apName;
        wifiNet["apPass"] = apList[i].apPass.length() > 0 ? true : false;
      }
    }
#if ASYNC_WEBSERVER == true
    serializeJson(jsonArray, *response);
    response->setCode(200);
    response->setContentLength(measureJson(jsonDoc));
    request->send(response);
#else
    // Improve me: not that efficient without the stream response
    serializeJson(jsonArray, buffer);
    webServer->send(200, "application/json", (buffer.equals("null") ? "{}" : buffer));
#endif
  });

#if ASYNC_WEBSERVER == true
  webServer->on((apiPrefix + "/scan").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
#else
  webServer->on((apiPrefix + "/scan").c_str(), HTTP_GET, [&]() {
    String buffer;
#endif
    JsonDocument jsonDoc;

    int scanResult;
    String ssid;
    uint8_t encryptionType;
    int32_t rssi;
    uint8_t* bssid;
    int32_t channel;

    scanResult = WiFi.scanComplete();
    if (scanResult == WIFI_SCAN_FAILED) {
      scanResult = WiFi.scanNetworks(true, true);   // FIXME: scanNetworks is disconnecting clients!
      jsonDoc["status"] = "scanning";
    } else if (scanResult > 0) {
      for (int8_t i = 0; i < scanResult; i++) {
        WiFi.getNetworkInfo(i, ssid, encryptionType, rssi, bssid, channel);

        JsonObject wifiNet = jsonDoc.createNestedObject();
        wifiNet["ssid"] = ssid;
        wifiNet["encryptionType"] = encryptionType;
        wifiNet["rssi"] = rssi;
        wifiNet["channel"] = channel;
        yield();
      }
      WiFi.scanDelete();
    }
#if ASYNC_WEBSERVER == true
    serializeJson(jsonDoc, *response);
    response->setCode(200);
    response->setContentLength(measureJson(jsonDoc));
    request->send(response);
#else
    // Improve me: not that efficient without the stream response
    serializeJson(jsonDoc, buffer);
    webServer->send(200, "application/json", buffer);
#endif
  });

#if ASYNC_WEBSERVER == true
  webServer->on((apiPrefix + "/status").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
#else
  webServer->on((apiPrefix + "/status").c_str(), HTTP_GET, [&]() {
    String buffer;
#endif
    JsonDocument jsonDoc;

    jsonDoc["ssid"] = WiFi.SSID();
    jsonDoc["signalStrengh"] = WiFi.RSSI();

    jsonDoc["ip"] = WiFi.localIP().toString();
    jsonDoc["gw"] = WiFi.gatewayIP().toString();
    jsonDoc["nm"] = WiFi.subnetMask().toString();

    jsonDoc["hostname"] = WiFi.getHostname();

    jsonDoc["chipModel"] = ESP.getChipModel();
    jsonDoc["chipRevision"] = ESP.getChipRevision();
    jsonDoc["chipCores"] = ESP.getChipCores();

    jsonDoc["getHeapSize"] = ESP.getHeapSize();
    jsonDoc["freeHeap"] = ESP.getFreeHeap();

#if ASYNC_WEBSERVER == true
    serializeJson(jsonDoc, *response);
    response->setCode(200);
    response->setContentLength(measureJson(jsonDoc));
    request->send(response);
#else
    // Improve me: not that efficient without the stream response
    serializeJson(jsonDoc, buffer);
    webServer->send(200, "application/json", buffer);
#endif
  });
}
