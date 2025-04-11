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
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <Preferences.h>
#include <DNSServer.h>

DNSServer dnsServer;

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
void dnsTask(void* param) {
  yield();
  delay(500); // wait a short time until everything is setup before executing the loop forever
  yield();
  const TickType_t xDelay = 1 / portTICK_PERIOD_MS;

  for(;;) {
    yield();
    dnsServer.processNextRequest();
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

  xTaskCreatePinnedToCore(
    dnsTask,
    "WifiManagerDNS",
    4096,   // Stack size in words
    this,   // Task input parameter
    1,      // Priority of the task
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

  // AP on/off
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI] onEvent() AP mode started!\n");
    softApRunning = true;
    }, ARDUINO_EVENT_WIFI_AP_START
  );

  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI] onEvent() AP mode stopped!\n");
    softApRunning = false;
    }, ARDUINO_EVENT_WIFI_AP_STOP
  );

  // AP client join/leave
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI] onEvent() new client connected to softAP!\n");
    }, ARDUINO_EVENT_WIFI_AP_STACONNECTED
  );

  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI] onEvent() Client disconnected from softAP!\n");
    }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED
  );
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

  /* Requires IDF >= 5.4
  const char* captivePortalUrl = "http://192.168.4.1/portal";
  // --- Configure DHCP Option 114 ---
  // Stop the DHCP server on the AP interface
  tcpip_adapter_dhcps_stop(TCPIP_ADAPTER_IF_AP);
  // Set Option 114 (Captive Portal URL)
  tcpip_adapter_dhcp_option_mode_t opt_info;
  opt_info.ip = 0; // Not used for vendor specific options like 114
  esp_err_t result = tcpip_adapter_dhcps_option(
    ESP_NETIF_OP_SET,          // Operation: SET the option
    TCPIP_ADAPTER_IF_AP,             // Option ID: 114 (Captive Portal)
    114,                       // Option ID: 114 (Captive Portal)
    (void*)captivePortalUrl,   // Option Value: Pointer to the URL string
    strlen(captivePortalUrl)   // Option Length: Length of the URL string
  );
  if (result == ESP_OK) {
    Serial.println("Successfully set DHCP Option 114.");
  } else {
      Serial.printf("Failed to set DHCP Option 114, error: %d\n", result);
  }
  // Start the DHCP server again on the AP interface
  tcpip_adapter_dhcps_start(TCPIP_ADAPTER_IF_AP);
  // --- End DHCP Option Configuration ---
*/

  bool state = WiFi.softAP(this->softApName.c_str(), (this->softApPass.length() ? this->softApPass.c_str() : NULL));
  if (state) {
    IPAddress IP = WiFi.softAPIP();
    char ipString[16]; // Genug Platz für "xxx.xxx.xxx.xxx\0"
    sprintf(ipString, "%d.%d.%d.%d", IP[0], IP[1], IP[2], IP[3]);

    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.setTTL(300);
    dnsServer.start(53, "*", IP);
    attachCaptivePortal();

    logMessage("[WIFI] AP created. My IP is: " + String(ipString) + "\n");
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
  dnsServer.stop();
  detachCaptivePortal();
  // WiFi.softAPdisconnect(); not required as mode(WIFI_STA) will stop the softAP
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

void WIFIMANAGER::attachCaptivePortal() {
  /*captivePortalWebHandlers[captivePortalWebHandlerCount++] = webServer->on("/index.htm", HTTP_GET, [&](AsyncWebServerRequest * request) {
    logMessage("[WIFI][INFO] Captive portal requested on /index.html\n");
    request->redirect(uiPrefix.c_str());
  });
  captivePortalWebHandlers[captivePortalWebHandlerCount++] = webServer->on("/generate_204", HTTP_GET, [&](AsyncWebServerRequest * request) {
    logMessage("[WIFI][INFO] Captive portal requested on /generate_204\n");
    request->redirect(uiPrefix.c_str());
  });
  captivePortalWebHandlers[captivePortalWebHandlerCount++] = webServer->on("/success.txt", HTTP_GET, [&](AsyncWebServerRequest * request) {
    logMessage("[WIFI][INFO] Captive portal requested on /success.txt\n");
    request->redirect(uiPrefix.c_str());
  });
  captivePortalWebHandlers[captivePortalWebHandlerCount++] = webServer->on("/connecttest.txt", HTTP_GET, [&](AsyncWebServerRequest * request) {
    logMessage("[WIFI][INFO] Captive portal requested on /connecttest.txt\n");
    request->redirect(uiPrefix.c_str());
  });

  webServer->onNotFound([](AsyncWebServerRequest *request) {
    Serial.print("inside onNotFound\n");
    
    if (request->url().endsWith("favicon.ico")) {
      request->send(404, "text/plain", "Not found");
    } else {
      Serial.print("[WIFI][INFO] Captive portal requested on " + request->url() + "\n");
      request->redirect("/wifi");
    }
  });*/
}

void WIFIMANAGER::detachCaptivePortal() {
  for (int i = 0; i < captivePortalWebHandlerCount; i++) {
    logMessage("[WIFI] Removing WebServer handler: DNS#" + String(i) + "\n");
    webServer->removeHandler(captivePortalWebHandlers[i]);
  }
  captivePortalWebHandlerCount = 0;  
}

/**
 * @brief Remove all registered WebServer handlers for API, UI and DNS
 * @details This is used to detach the WebServer from the WifiManager
 *          when the WifiManager is deleted
 */
void WIFIMANAGER::detachWebServer() {
  for (int i = 0; i < apiWebHandlerCount; i++) {
    logMessage("[WIFI] Removing WebServer handler: API#" + String(i) + "\n");
    webServer->removeHandler(apiWebHandlers[i]);
  }
  apiWebHandlerCount = 0;

  detachUI();
  detachCaptivePortal();
}

/**
 * @brief Attach the WebServer to the WifiManager to register the RESTful API
 * @param srv WebServer object
 */
void WIFIMANAGER::attachWebServer(AsyncWebServer * srv) {
  webServer = srv; // store it in the class for later use

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/softap/start").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request) {
    request->send(200, "application/json", "{\"message\":\"Soft AP stopped\"}");
    yield();
    delay(250);
    runSoftAP();
  });
  
  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/softap/stop").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request) {
    request->send(200, "application/json", "{\"message\":\"Soft AP stopped\"}");
    yield();
    delay(250); // It's likely that this message won't go trough, but we give it a short time
    stopSoftAP();
  });

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/client/stop").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request) {
    request->send(200, "application/json", "{\"message\":\"Terminating current Wifi connection\"}");
    yield();
    delay(500); // It's likely that this message won't go trough, but we give it a short time
    stopClient();
  });

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/add").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, (const char*)data);
    auto resp = request;
    if (!jsonBuffer["apName"].is<String>() || !jsonBuffer["apPass"].is<String>()) {
      resp->send(422, "application/json", "{\"message\":\"Invalid data\"}");
      return;
    }
    if (!addWifi(jsonBuffer["apName"].as<String>(), jsonBuffer["apPass"].as<String>())) {
      resp->send(500, "application/json", "{\"message\":\"Unable to process data\"}");
    } else resp->send(200, "application/json", "{\"message\":\"New AP added\"}");
  });

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/id").c_str(), HTTP_DELETE, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, (const char*)data);
    auto resp = request;
    if (!jsonBuffer["id"].is<uint8_t>() || jsonBuffer["id"].as<uint8_t>() >= WIFIMANAGER_MAX_APS) {
      resp->send(422, "application/json", "{\"message\":\"Invalid data\"}");
      return;
    }
    if (!delWifi(jsonBuffer["id"].as<uint8_t>())) {
      resp->send(500, "application/json", "{\"message\":\"Unable to delete entry\"}");
    } else resp->send(200, "application/json", "{\"message\":\"AP deleted\"}");
  });

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/apName").c_str(), HTTP_DELETE, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    JsonDocument jsonBuffer;
    deserializeJson(jsonBuffer, (const char*)data);
    auto resp = request;
    if (!jsonBuffer["apName"].is<String>()) {
      resp->send(422, "application/json", "{\"message\":\"Invalid data\"}");
      return;
    }
    if (!delWifi(jsonBuffer["apName"].as<String>())) {
      resp->send(500, "application/json", "{\"message\":\"Unable to delete entry\"}");
    } else resp->send(200, "application/json", "{\"message\":\"AP deleted\"}");
  });

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/configlist").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
    JsonDocument jsonDoc;
    auto jsonArray = jsonDoc.to<JsonArray>();
    for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
      if (apList[i].apName.length() > 0) {
        JsonObject wifiNet = jsonArray.add<JsonObject>();
        wifiNet["id"] = i;
        wifiNet["apName"] = apList[i].apName;
        wifiNet["apPass"] = apList[i].apPass.length() > 0 ? true : false;
      }
    }
    serializeJson(jsonArray, *response);
    response->setCode(200);
    response->setContentLength(measureJson(jsonDoc));
    request->send(response);
  });

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/scan").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
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

        JsonObject wifiNet = jsonDoc.add<JsonObject>();
        wifiNet["ssid"] = ssid;
        wifiNet["encryptionType"] = encryptionType;
        wifiNet["rssi"] = rssi;
        wifiNet["channel"] = channel;
        yield();
      }
      WiFi.scanDelete();
    }
    serializeJson(jsonDoc, *response);
    response->setCode(200);
    response->setContentLength(measureJson(jsonDoc));
    request->send(response);
  });

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/status").c_str(), HTTP_GET, [&](AsyncWebServerRequest *request) {
    AsyncResponseStream *response = request->beginResponseStream("application/json");
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

    serializeJson(jsonDoc, *response);
    response->setCode(200);
    response->setContentLength(measureJson(jsonDoc));
    request->send(response);
  });
}

/**
 * @brief Detach all UI web handlers from the web server
 * @details Iterates through the list of UI web handlers, removes each one from the web server,
 * and resets the count of UI web handlers to zero.
 */
void WIFIMANAGER::detachUI() {
  for (int i = 0; i < uiWebHandlerCount; i++) {
    logMessage("[WIFI] Removing WebServer handler: UI#" + String(i) + "\n");
    webServer->removeHandler(uiWebHandlers[i]);
  }
  uiWebHandlerCount = 0;
}

/**
 * @brief Attach the WebServer to the WifiManager to register the RESTful API
 * @param srv WebServer object
 */
void WIFIMANAGER::attachUI() {
  uiWebHandlers[uiWebHandlerCount++] = &webServer->on((uiPrefix).c_str(), HTTP_GET, [](AsyncWebServerRequest* request) {
    String html = R"html(
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>ESP32 WiFi Manager</title>
    <style>
        :root {
            --primary-color: #2563eb;
            --bg-color: #f8fafc;
            --card-bg: #ffffff;
            --text-color: #1e293b;
            --border-color: #e2e8f0;
        }

        body {
            font-family: system-ui, -apple-system, sans-serif;
            background: var(--bg-color);
            color: var(--text-color);
            margin: 0;
            padding: 16px;
            line-height: 1.5;
        }

        .container {
            max-width: 600px;
            margin: 0 auto;
        }

        .card {
            background: var(--card-bg);
            border-radius: 8px;
            padding: 16px;
            margin-bottom: 16px;
            box-shadow: 0 1px 3px rgba(0,0,0,0.1);
            border: 1px solid var(--border-color);
        }

        h1, h2 {
            margin: 0 0 16px 0;
            color: var(--text-color);
        }

        .network-list {
            list-style: none;
            padding: 0;
            margin: 0;
        }

        .network-item {
            display: flex;
            align-items: center;
            padding: 12px;
            border-bottom: 1px solid var(--border-color);
            cursor: pointer;
            transition: background-color 0.2s;
        }

        .network-item:last-child {
            border-bottom: none;
        }

        .network-item:hover {
            background-color: var(--bg-color);
        }

        .network-info {
            flex-grow: 1;
        }

        .network-info div {
          float: left;
          width: 70%;
        }

        .network-info button {
          float: right;
          width: 30%;
        }

        .ssid {
            font-weight: 500;
            margin-bottom: 4px;
        }

        .signal {
            font-size: 0.875rem;
            color: #64748b;
        }

        button {
            background: var(--primary-color);
            color: white;
            border: none;
            padding: 8px 16px;
            border-radius: 4px;
            cursor: pointer;
            font-size: 0.875rem;
            transition: opacity 0.2s;
        }

        button:hover {
            opacity: 0.9;
        }

        button:disabled {
            opacity: 0.5;
            cursor: not-allowed;
        }

        .status {
            padding: 8px;
            border-radius: 4px;
            margin-bottom: 16px;
            display: none;
        }

        .status.error {
            background: #fee2e2;
            color: #991b1b;
            display: block;
        }

        .status.success {
            background: #dcfce7;
            color: #166534;
            display: block;
        }

        .status.info {
            background: #e0f2fe;
            color: #075985;
            display: block;
        }

        .modal {
            display: none;
            position: fixed;
            top: 0;
            left: 0;
            width: 100%;
            height: 100%;
            background: rgba(0,0,0,0.5);
            align-items: center;
            justify-content: center;
        }

        .modal-content {
            background: var(--card-bg);
            padding: 24px;
            border-radius: 8px;
            width: 90%;
            max-width: 400px;
        }

        input {
            width: 100%;
            padding: 8px;
            margin: 8px 0 16px;
            border: 1px solid var(--border-color);
            border-radius: 4px;
            box-sizing: border-box;
        }

        .button-group {
            display: flex;
            gap: 8px;
            justify-content: flex-end;
        }

        .button-secondary {
            background: var(--bg-color);
            color: var(--text-color);
            border: 1px solid var(--border-color);
        }

        .saved-networks {
            margin-top: 8px;
            padding-top: 8px;
            border-top: 1px solid var(--border-color);
        }
    </style>
</head>
<body>
    <div class="container">
        <div class="card">
            <h1>ESP32 WiFi Manager</h1>
            <div id="status"></div>
            <button onclick="scanNetworks()">Scan for Networks</button>
            <button onclick="showConnectModal()">Manual Connect</button>
        </div>

        <div class="card">
            <h2>Available Networks</h2>
            <div id="networkList" class="network-list"></div>
        </div>

        <div class="card">
            <h2>Saved Networks</h2>
            <div id="savedNetworks" class="network-list"></div>
        </div>
    </div>

    <div id="connectModal" class="modal">
        <div class="modal-content">
            <h2>Connect to Network</h2>
            <form id="connectForm" onsubmit="connectToNetwork(event)">
                <label for="apName">Network Name:</label>
                <input type="text" id="apName" required>
                
                <label for="apPass">Password:</label>
                <input type="password" id="apPass" required>
                
                <div class="button-group">
                    <button type="button" class="button-secondary" onclick="closeModal()">Cancel</button>
                    <button type="submit">Connect</button>
                </div>
            </form>
        </div>
    </div>

    <script>
        const API_BASE = '/api';
        let networks = {};

        // Load saved networks when page loads
        window.addEventListener('load', () => {
            loadSavedNetworks();
            scanNetworks();
        });

        async function loadSavedNetworks() {
            try {
                const response = await fetch(`${API_BASE}/wifi/configlist`);
                if (!response.ok) throw new Error('Failed to fetch saved networks');
                
                const savedNetworks = await response.json();
                displaySavedNetworks(savedNetworks);
            } catch (error) {
                showStatus('Failed to load saved networks: ' + error.message, 'error');
            }
        }

        function displaySavedNetworks(networks) {
            const networkList = document.getElementById('savedNetworks');
            const networkArray = Object.values(networks);
            
            if (networkArray.length === 0) {
                networkList.innerHTML = '<div class="network-item">No saved networks</div>';
                return;
            }

            networkList.innerHTML = networkArray.map(network => `
                <div class="network-item">
                    <div class="network-info">
                        <div class="ssid">${network.apName}</div>
                        <button onclick="deleteNetwork('${network.id}')">delete</button>
                    </div>
                </div>
            `).join('');
        }

        async function scanNetworks() {
            try {
                showStatus('Scanning for networks...', 'info');
                const response = await fetch(`${API_BASE}/wifi/scan`);
                if (!response.ok) throw new Error('Network scan failed');
                
                // Wait 5 seconds for the scan to complete
                await new Promise(resolve => setTimeout(resolve, 5000));
                
                const scanResponse = await fetch(`${API_BASE}/wifi/scan/results`);
                if (!scanResponse.ok) throw new Error('Failed to fetch scan results');
                
                networks = await scanResponse.json();
                displayNetworks(networks);
                showStatus('Networks found', 'success');
            } catch (error) {
                showStatus(error.message, 'error');
            }
        }

        async function scanNetworks() {
          const MAX_RETRIES = 6; // 30 seconds / 5 seconds per retry
          let retryCount = 0;
          let networks = [];

          showStatus('Scanning for networks...', 'info');

          while (retryCount < MAX_RETRIES) {
              try {
                  const response = await fetch(`${API_BASE}/wifi/scan`);
                  if (!response.ok) {
                      throw new Error(`Network scan request failed with status: ${response.status}`);
                  }

                  const data = await response.json();

                  if (Array.isArray(data)) {
                      networks = data;
                      displayNetworks(networks);
                      showStatus('Networks found', 'success');
                      return; // Exit the function on success
                  } else if (data && data.status === 'scanning') {
                      showStatus('Scanning in progress...', 'info');
                      await new Promise(resolve => setTimeout(resolve, 5000));
                      retryCount++;
                  } else {
                      throw new Error('Unexpected response format');
                  }
              } catch (error) {
                  showStatus(`Error during scan: ${error.message}`, 'error');
                  await new Promise(resolve => setTimeout(resolve, 5000));
                  retryCount++;
              }
          }

          // Timeout reached, return an empty list
          displayNetworks([]);
          showStatus('Scan timed out, no networks found.', 'warning');
      }

      function displayNetworks(networks) {
        const networkList = document.getElementById('networkList');
        const networkArray = Object.values(networks)
          .filter(network => network.ssid.length > 0);
        
        if (networkArray.length === 0) {
          networkList.innerHTML = '<div class="network-item">No networks found</div>';
          return;
        }

        // Sort networks by RSSI
        networkArray.sort((a, b) => b.rssi - a.rssi);

        networkList.innerHTML = networkArray
          .map(network => `
            <div class="network-item" onclick="showConnectModal('${network.ssid}')">
              <div class="network-info">
                <div class="ssid">${network.ssid}</div>
                <div class="signal">
                    Signal: ${getSignalStrength(network.rssi)}
                    ${network.encryptionType > 0 ? '🔒' : ''}
                </div>
              </div>
            </div>
          `).join('');
        }

        function getSignalStrength(rssi) {
            if (rssi >= -50) return 'Excellent';
            if (rssi >= -60) return 'Very Good';
            if (rssi >= -70) return 'Good';
            if (rssi >= -80) return 'Fair';
            return 'Poor';
        }

        function showConnectModal(apName = '') {
            document.getElementById('apName').value = apName;
            document.getElementById('apName').readOnly = !!apName;
            document.getElementById('apPass').value = '';
            document.getElementById('connectModal').style.display = 'flex';
        }

        function closeModal() {
            document.getElementById('connectModal').style.display = 'none';
        }

        async function deleteNetwork(deleteId) {
            try {
                const response = await fetch(`${API_BASE}/wifi/id`, {
                    method: 'DELETE',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({ id: parseInt(deleteId, 10) }),
                });
                
                if (!response.ok) throw new Error('Failed to delete network');
                
                showStatus('Network deleted successfully', 'success');
                await loadSavedNetworks(); // Refresh the list
            } catch (error) {
                showStatus('Failed to delete network: ' + error.message, 'error');
            }
        }

        async function connectToNetwork(event) {
            event.preventDefault();
            const apName = document.getElementById('apName').value;
            const apPass = document.getElementById('apPass').value;

            try {
                showStatus('Connecting to network...', 'info');
                const response = await fetch(`${API_BASE}/wifi/add`, {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({ apName, apPass }),
                });

                if (!response.ok) throw new Error('Connection failed');

                closeModal();
                showStatus('Successfully connected!', 'success');
                
                // Refresh saved networks list
                await loadSavedNetworks();
            } catch (error) {
                showStatus(error.message, 'error');
            }
        }

        function showStatus(message, type) {
            const statusElement = document.getElementById('status');
            statusElement.innerHTML = message;
            statusElement.className = `status ${type}`;
        }
    </script>
</body>
</html>
)html";
    request->send(200, "text/html", html);
  });
}