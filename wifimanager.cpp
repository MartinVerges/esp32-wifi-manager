/**
 * Wifi Manager
 * (c) 2022 Martin Verges
 *
 * Licensed under CC BY-NC-SA 4.0
 * (Attribution-NonCommercial-ShareAlike 4.0 International)
**/
#include "wifimanager.h"
#include "AsyncJson.h"
#include "ArduinoJson.h"
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <Preferences.h>

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
 * @brief Load config from NVS, start to connect to WIFI and initiate a background task to keep it up
 */
void WIFIMANAGER::startBackgroundTask() {
  loadFromNVS();
  tryConnect();
  xTaskCreatePinnedToCore(
    wifiTask,
    "WifiManager",
    4000,   // Stack size in words
    this,   // Task input parameter
    0,      // Priority of the task
    &WifiCheckTask,  // Task handle.
    0       // Core where the task should run
  );
}

/**
 * @brief Construct a new WIFIMANAGER::WIFIMANAGER object
 * @details Puts the Wifi mode to AP+STA and registers Wifi Events
 * @param ns Namespace for the preferences non volatile storage (NVS)
 */
WIFIMANAGER::WIFIMANAGER(const char * ns) {
  NVS = (char *)ns;
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE, INADDR_NONE);

  // AP on/off
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.println(F("[WIFI] AP mode started!"));
    softApRunning = true;
  }, SYSTEM_EVENT_AP_START);
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.println(F("[WIFI] AP mode stopped!"));
    softApRunning = false;
  }, SYSTEM_EVENT_AP_STOP);
  // AP client join/leave
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.println(F("[WIFI] New client connected to softAP!"));
  }, SYSTEM_EVENT_AP_STACONNECTED);
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    Serial.println(F("[WIFI] Client disconnected from softAP!"));
  }, SYSTEM_EVENT_AP_STADISCONNECTED);
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
void WIFIMANAGER::fallbackToSoftAp(bool state) {
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
      String apName = preferences.getString(tmpKey, "");
      if (apName.length() > 0) {
        sprintf(tmpKey, "apPass%d", i);
        String apPass = preferences.getString(tmpKey);
        Serial.printf("[WIFI] Load SSID '%s' to %d. slot.\n", apName.c_str(), i+1);
        apList[i].apName = apName;
        apList[i].apPass = apPass;
        configuredSSIDs++;
      }
    }
    preferences.end();
    return true;
  }
  Serial.println(F("[WIFI] Unable to load data from NVS, giving up..."));
  return false;
}

/**
 * @brief Write the current in memory configuration to the non volatile storage
 * @return true on success
 * @return false on error with the NVS
 */
bool WIFIMANAGER::writeToNVS() {
  if (preferences.begin(NVS, false)) {
    preferences.clear();
    char tmpKey[10] = { 0 };
    for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
      if (!apList[i].apName.length()) continue;
      sprintf(tmpKey, "apName%d", i);
      preferences.putString(tmpKey, apList[i].apName);
      sprintf(tmpKey, "apPass%d", i);
      preferences.putString(tmpKey, apList[i].apPass);
    }
    preferences.end();
    return true;
  } 
  Serial.println(F("[WIFI] Unable to write data to NVS, giving up..."));
  return false;
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
      Serial.println(F("[WIFI] No SSID given or ssid too long"));
      return false;
  }

  if(apPass.length() > 63) {
      Serial.println(F("[WIFI] Passphrase too long"));
      return false;
  }

  for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
    if (apList[i].apName == "") {
      Serial.printf("[WIFI] Found unused slot Nr. %d to store the new SSID '%s' credentials.\n", i, apName.c_str());
      apList[i].apName = apName;
      apList[i].apPass = apPass;
      if (updateNVS) return writeToNVS();
      else return true;
    }
  }
  Serial.println(F("[WIFI] No slot available to store SSID credentials"));
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
    return configuredSSIDs > 0;
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
  Serial.print(F("[WIFI][ERROR] We did not find a valid entry!"));
  Serial.print(F("[WIFI][ERROR] Make sure to not call this function if configuredSSIDs != 1."));
  return 0;
}

/**
 * @brief Background loop function running inside the task
 * @details regulary check if the connection is up&running, try to reconnect or create a fallback AP
 */
void WIFIMANAGER::loop() {
  if (millis() - lastWifiCheck < intervalWifiCheck) return;
  lastWifiCheck = millis();

  if(WiFi.status() == WL_CONNECTED) {
    // Check if we are connected to a well known SSID
    for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
      if (WiFi.SSID() == apList[i].apName) {
        Serial.printf("[WIFI][STATUS] Connected to known SSID: '%s' with IP %s.\n",
          WiFi.SSID().c_str(),
          WiFi.localIP().toString().c_str()
        );
        return;
      }
    }
    // looks like we are connected to something else, strange!?
    Serial.print(F("[WIFI] We are connected to an unknown SSID ignoring. Connected to: "));
    Serial.println(WiFi.SSID());
  } else {
    // let's try to connect to some WiFi in Range
    if (!tryConnect()) {
      if (createFallbackAP) runSoftAP();
      else Serial.println(F("[WIFI] Auto creation of SoftAP is disabled, no starting AP!"));
    }
  }
  
  if (millis() - startApTime > timeoutApMillis) {
    if (WiFi.softAPgetStationNum() > 0) {
      Serial.println(F("[WIFI] Running in AP mode with client connected"));
      startApTime = millis(); // reset timeout as someone is connected
      return;
    }
    Serial.println(F("[WIFI] Running in AP mode but timeout reached. Closing AP!"));
    stopSoftAp();
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
    Serial.println(F("[WIFI] No SSIDs configured in NVS, unable to connect."));
    if (createFallbackAP) runSoftAP();
    return false;
  }

  int choosenAp = INT_MIN;
  if (configuredSSIDs == 1) {
    // only one configured SSID, skip scanning and try to connect to this specific one.
    choosenAp = getApEntry();
  } else {
    int8_t scanResult = WiFi.scanNetworks(false, true);
    if(scanResult <= 0) {
      Serial.println(F("[WIFI] Unable to find WIFI networks in range to this device!"));
      return false;
    }
    Serial.print(F("[WIFI] Found networks: "));
    Serial.println(scanResult);
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
    Serial.println(F("[WIFI] Unable to find an SSID to connect to!"));
    return false;
  } else {
    Serial.printf("[WIFI] Trying to connect to SSID %s with password %s.\n", 
      apList[choosenAp].apName.c_str(),
      (apList[choosenAp].apPass.length() > 0 ? "'***'" : "''")
    );

    WiFi.begin(apList[choosenAp].apName.c_str(), apList[choosenAp].apPass.c_str());
    wl_status_t status = WiFi.status();

    auto startTime = millis();
    // wait for connection, fail, or timeout
    while(status != WL_CONNECTED && status != WL_NO_SSID_AVAIL && status != WL_CONNECT_FAILED && (millis() - startTime) <= 10000) {
        delay(10);
        status = WiFi.status();
    }
    switch(status) {
      case WL_CONNECTED:
        Serial.println(F("[WIFI] Connection successful."));
        Serial.printf("[WIFI] SSID   : %s\n", WiFi.SSID().c_str());
        Serial.printf("[WIFI] IP     : %s\n", WiFi.localIP().toString().c_str());

        stopSoftAp();
        return true;
        break;
      case WL_NO_SSID_AVAIL:
        Serial.println(F("[WIFI] Connection failed: The AP can't be found."));
        break;
      case WL_CONNECT_FAILED:
        Serial.println(F("[WIFI] Connecting failed: Unknown reason"));
        break;
      default:
        Serial.printf("[WIFI] Connecting Failed (Status: %d).\n", status);
        break;
    }
  }
  return false;
}

/**
 * @brief Start a SoftAP for direct client access
 * @param apName name of the AP to create (default is ESP_XXXXXXXX)
 * @return true on success
 * @return false o error or if a SoftAP already runs
 */
bool WIFIMANAGER::runSoftAP(String apName) {
  if (softApRunning) return true;
  startApTime = millis();

  if (apName == "") apName = "ESP_" + String((uint32_t)ESP.getEfuseMac());
  Serial.printf("[WIFI] Starting configuration portal on AP SSID %s\n", apName.c_str());

  bool state = WiFi.softAP(apName.c_str());
  if (state) {
    IPAddress IP = WiFi.softAPIP();
    Serial.print(F("[WIFI] AP created. My IP is: "));
    Serial.println(IP);
    return true;
  } else {
    Serial.println(F("[WIFI] Unable to create soft AP!"));
    return false;
  }
}

/**
 * @brief Stop/Disconnect a current running SoftAP
 */
void WIFIMANAGER::stopSoftAp() {
  WiFi.softAPdisconnect();
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
  stopSoftAp();
  stopClient();
}

/**
 * @brief Attach the ESP_AsyncWebServer to the WifiManager to register the RESTful API
 * @param srv AsyncWebServer object
 */
void WIFIMANAGER::attachWebServer(AsyncWebServer * srv) {
  webServer = srv; // store it in the class for later use

  webServer->on((apiPrefix + "/add").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {

    DynamicJsonDocument jsonBuffer(256);
    deserializeJson(jsonBuffer, (const char*)data);

    if (!jsonBuffer["apName"].is<String>() || !jsonBuffer["apPass"].is<String>()) {
      request->send(422, "text/plain", "Invalid data");
      return;
    }
    if (!addWifi(jsonBuffer["apName"].as<String>(), jsonBuffer["apPass"].as<String>())) {
      request->send(500, "application/json", "{\"message\":\"Unable to process data\"}");
    } else request->send(200, "application/json", "{\"message\":\"New AP added\"}");
  });

  webServer->on((apiPrefix + "/id").c_str(), HTTP_DELETE, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {

    DynamicJsonDocument jsonBuffer(128);
    deserializeJson(jsonBuffer, (const char*)data);

    if (!jsonBuffer["id"].is<uint8_t>() || jsonBuffer["id"].as<uint8_t>() >= WIFIMANAGER_MAX_APS) {
      request->send(422, "text/plain", "Invalid data");
      return;
    }
    if (!delWifi(jsonBuffer["id"].as<uint8_t>())) {
      request->send(500, "application/json", "{\"message\":\"Unable to delete entry\"}");
    } else request->send(200, "application/json", "{\"message\":\"AP deleted\"}");
  });

  webServer->on((apiPrefix + "/apName").c_str(), HTTP_DELETE, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    
    DynamicJsonDocument jsonBuffer(128);
    deserializeJson(jsonBuffer, (const char*)data);

    if (!jsonBuffer["apName"].is<String>()) {
      request->send(422, "text/plain", "Invalid data");
      return;
    }
    if (!delWifi(jsonBuffer["apName"].as<String>())) {
      request->send(500, "application/json", "{\"message\":\"Unable to delete entry\"}");
    } else request->send(200, "application/json", "{\"message\":\"AP deleted\"}");
  });

  webServer->on((apiPrefix + "/configlist").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument jsonDoc(2048);

    for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
      if (apList[i].apName.length() > 0) {
        JsonObject wifiNet = jsonDoc.createNestedObject();
        wifiNet["id"] = i;
        wifiNet["apName"] = apList[i].apName;
        wifiNet["apPass"] = apList[i].apPass.length() > 0 ? true : false;
      }
    }

    serializeJson(jsonDoc, *response);

    response->setCode(200);
    response->setContentLength(measureJson(jsonDoc));
    request->send(response);
  });

  webServer->on((apiPrefix + "/scan").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument jsonDoc(4096);

    int scanResult;
    String ssid;
    uint8_t encryptionType;
    int32_t rssi;
    uint8_t* bssid;
    int32_t channel;

    scanResult = WiFi.scanNetworks(false, true);
    for (int8_t i = 0; i < scanResult; i++) {
      WiFi.getNetworkInfo(i, ssid, encryptionType, rssi, bssid, channel);

      JsonObject wifiNet = jsonDoc.createNestedObject();
      wifiNet["ssid"] = ssid;
      wifiNet["encryptionType"] = encryptionType;
      wifiNet["rssi"] = rssi;
      wifiNet["channel"] = channel;
      yield();
    }

    serializeJson(jsonDoc, *response);

    response->setCode(200);
    response->setContentLength(measureJson(jsonDoc));
    request->send(response);
  });

  webServer->on((apiPrefix + "/status").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    DynamicJsonDocument jsonDoc(1024);

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

    serializeJson(jsonDoc, *response);

    response->setCode(200);
    response->setContentLength(measureJson(jsonDoc));
    request->send(response);
  });
}
