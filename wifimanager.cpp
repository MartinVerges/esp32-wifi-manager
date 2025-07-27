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
void wifiBgTask(void* param) {
  yield();
  delay(500); // wait a short time until everything is setup before executing the loop forever
  yield();
  const TickType_t xDelay = 10000 / portTICK_PERIOD_MS;
  WIFIMANAGER * wifimanager = (WIFIMANAGER *) param;

  wifimanager->loop(true);  // force run, to skip wait time
  delay(500);

  for(;;) {
    yield();
    wifimanager->loop();
    yield();
    vTaskDelay(xDelay);
  }
}

void dnsBgTask(void* param) {
  yield();
  delay(500); // wait a short time until everything is setup before executing the loop forever
  yield();
  const TickType_t xDelay = 50 / portTICK_PERIOD_MS;
  WIFIMANAGER * wifimanager = (WIFIMANAGER *) param;

  for(;;) {
    yield();
    if (wifimanager->dnsServerActive) dnsServer.processNextRequest();
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
  // setMode(WIFI_STA);
  tryConnect();

  BaseType_t taskCreated = xTaskCreatePinnedToCore(
    wifiBgTask,
    "WifiManager",
    4096,   // Stack size in words
    this,   // Task input parameter
    1,      // Priority of the task
    &wifiTaskHandle,  // Task handle.
    0       // Core where the task should run
  );

  if (taskCreated != pdPASS) {
    logMessage("[ERROR] WifiManager: Error creating Wifi background task\n");
  }

  xTaskCreatePinnedToCore(
    dnsBgTask,
    "WifiManagerDNS",
    4096,   // Stack size in words
    this,   // Task input parameter
    1,      // Priority of the task
    &dnsTaskHandle,  // Task handle.
    0       // Core where the task should run
  );

  if (taskCreated != pdPASS) {
    logMessage("[ERROR] WifiManager: Error creating DNS background task\n");
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
    logMessage("[WIFI][EVENT] AP mode started!\n");
    }, ARDUINO_EVENT_WIFI_AP_START
  );

  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI][EVENT] AP mode stopped!\n");
    }, ARDUINO_EVENT_WIFI_AP_STOP
  );

  // AP client join/leave
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage(String("[WIFI][EVENT] Client connected - MAC: ") +
      String(info.wifi_ap_staconnected.mac[0], HEX) + ":" +
      String(info.wifi_ap_staconnected.mac[1], HEX) + ":" +
      String(info.wifi_ap_staconnected.mac[2], HEX) + ":" +
      String(info.wifi_ap_staconnected.mac[3], HEX) + ":" +
      String(info.wifi_ap_staconnected.mac[4], HEX) + ":" +
      String(info.wifi_ap_staconnected.mac[5], HEX) + "\n");
    }, ARDUINO_EVENT_WIFI_AP_STACONNECTED
  );

  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage(String("[WIFI][EVENT] Client disconnected - MAC: ") +
      String(info.wifi_ap_stadisconnected.mac[0], HEX) + ":" +
      String(info.wifi_ap_stadisconnected.mac[1], HEX) + ":" +
      String(info.wifi_ap_stadisconnected.mac[2], HEX) + ":" +
      String(info.wifi_ap_stadisconnected.mac[3], HEX) + ":" +
      String(info.wifi_ap_stadisconnected.mac[4], HEX) + ":" +
      String(info.wifi_ap_stadisconnected.mac[5], HEX) + "\n");
    }, ARDUINO_EVENT_WIFI_AP_STADISCONNECTED
  );

  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI][EVENT] *** IP ASSIGNED TO CLIENT: " + 
      IPAddress(info.wifi_ap_staipassigned.ip.addr).toString() + " ***\n");
    }, ARDUINO_EVENT_WIFI_AP_STAIPASSIGNED
  );

  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI][EVENT] Disconnected from STA\n");
    }, ARDUINO_EVENT_WIFI_STA_DISCONNECTED
  );
  WiFi.onEvent([&](WiFiEvent_t event, WiFiEventInfo_t info) {
    logMessage("[WIFI][EVENT] Connected to STA\n");
    }, ARDUINO_EVENT_WIFI_STA_CONNECTED
  );
}

/**
 * @brief Destroy the WIFIMANAGER::WIFIMANAGER object
 * @details will stop the background task as well but not cleanup the AsyncWebserver
 */
WIFIMANAGER::~WIFIMANAGER() {
  dnsServerActive = false;

  // Give DNS task time to stop processing
  delay(100);
  yield();

  if (wifiTaskHandle != NULL) {
    vTaskDelete(wifiTaskHandle);
    wifiTaskHandle = NULL;
  }
  if (dnsTaskHandle != NULL) {
    vTaskDelete(dnsTaskHandle);
    dnsTaskHandle = NULL;
  }
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
  if (num > 0) return writeToNVS();
  return false;
}

String _wifiModeAsString(wifi_mode_t mode) {
    if (mode == WIFI_MODE_STA) return "WIFI_MODE_STA";
    if (mode == WIFI_MODE_AP) return "WIFI_MODE_AP";
    if (mode == WIFI_MODE_APSTA) return "WIFI_MODE_APSTA";
    if (mode == WIFI_MODE_NULL) return "WIFI_MODE_NULL";
    if (mode == WIFI_MODE_MAX) return "WIFI_MODE_MAX";
    return "UNKNOWN";
}

bool WIFIMANAGER::setMode(wifi_mode_t mode) {
  bool result = WiFi.mode(mode);
  if (!result) return false;

  unsigned long startTime = millis();
  const unsigned long timeout = 10000; // 10 Sekunden

  logMessage("[WIFI] Switching WiFi mode to " + _wifiModeAsString(mode) + " ...");
  while (WiFi.getMode() != mode && millis() - startTime < timeout) {
    delay(10);
    logMessage(".");
  }
  if (WiFi.getMode() == mode) {
    logMessage(" success\n");
  } else {
    logMessage(" timeout\n");
  }
  return true;
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
 * @return uint8_t apList element id or WIFIMANAGER_MAX_APS on error
 */
uint8_t WIFIMANAGER::getApEntry() {
  for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
    if (apList[i].apName.length()) return i;
  }
  logMessage("[WIFI][ERROR] We did not find a valid entry!\n");
  logMessage("[WIFI][ERROR] Make sure to not call this function if not configuredSSIDs > 0.\n");
  return WIFIMANAGER_MAX_APS;
}

/**
 * @brief Return the time in seconds until the SoftAP times out.
 * @return uint32_t time in seconds until timeout
 */
uint32_t WIFIMANAGER::getSoftApTimeRemaining() {
  auto time = (timeoutApMillis - (millis() - startApTimeMillis)) / 1000;
  if (time > timeoutApMillis) time = 0;
  return time;
}

/**
 * @brief Background loop function running inside the task
 * @details regulary check if the connection is up&running, try to reconnect or create a fallback AP
 */
void WIFIMANAGER::loop(bool force) {
  if (!force && millis() - lastWifiCheckMillis < intervalWifiCheckMillis) return;
  lastWifiCheckMillis = millis();

  if (WiFi.getMode() == WIFI_AP) {
    logMessage("[WIFI] Operating in softAP (" + WiFi.softAPIP().toString() + ") mode with " + String(WiFi.softAPgetStationNum()) + " client(s). "
              + "Next connection attempt in " + String(getSoftApTimeRemaining()) + " seconds\n");

    if (getSoftApTimeRemaining() == 0) {
      if (WiFi.softAPgetStationNum() > 0) {
        logMessage("[WIFI] SoftAP has " + String(WiFi.softAPgetStationNum()) + " clients connected! Resetting timeout\n");
        startApTimeMillis = millis(); // reset timeout as someone is connected
        return;
      }
      logMessage("[WIFI] Running in softAP mode but timeout reached. Closing softAP!\n");
      stopSoftAP();
      delay(100);
    }
  } else if (WiFi.getMode() == WIFI_STA) {
    if (WiFi.waitForConnectResult() == WL_CONNECTED && !WiFi.SSID().isEmpty()) {
      // Check if we are connected to a well known SSID
      for(uint8_t i=0; i<WIFIMANAGER_MAX_APS; i++) {
        if (WiFi.SSID() == apList[i].apName) {
          logMessage(String("[WIFI][STATUS] Connected to known SSID: '") + WiFi.SSID() + "' with IP " + WiFi.localIP().toString() + "\n");
          return;
        }
      }
    }
    // looks like we are connected to something else, strange!?
    logMessage("[WIFI] Connected to an unknown SSID, ignoring. Currently connected to: " + WiFi.SSID() + "\n");
  } else {
    // let's try to connect to some WiFi in Range
    if (!tryConnect()) {
      if (createFallbackAP) startSoftAP();
      else logMessage("[WIFI] Auto creation of softAP is disabled. SoftAP won't start.\n");
    }
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
    logMessage("[WIFI] No SSIDs configured in NVS, unable to connect.\n");
    if (createFallbackAP) startSoftAP();
    return false;
  }

  if (WiFi.getMode() == WIFI_AP) {
    logMessage("[WIFI] SoftAP running with " + String(WiFi.softAPgetStationNum()) + " client(s) connected.\n");
  }

  int choosenAp = INT_MIN;
  if (configuredSSIDs == 1) {
    // only one configured SSID, skip scanning and try to connect to this specific one.
    choosenAp = getApEntry();
  } else {
    logMessage("[WIFI][CONNECT] Scanning for WIFI networks...\n");
    int8_t scanResult = WiFi.scanNetworks(true, true);
    while (true) {
      scanResult = WiFi.scanComplete();
      if (scanResult == WIFI_SCAN_RUNNING) {
        delay(50);
        continue;
      }
      if (scanResult <= 0) {
        setMode(WIFI_OFF); // scan changes mode, but won't return to the last state
        logMessage("[WIFI][CONNECT] Unable to find WIFI networks in range to this device!\n");
        return false;
      }
      break;
    }
    logMessage(String("[WIFI][CONNECT] Found ") + String(scanResult) + " networks in range\n");
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
    logMessage("[WIFI][CONNECT] Unable to find an SSID to connect to!\n");
    return false;
  } else {
    logMessage(String("[WIFI][CONNECT] Trying to connect to SSID ") + apList[choosenAp].apName 
      + " " + (apList[choosenAp].apPass.length() > 0 ? "with password '***'" : "without password") + "\n"
    );
    return tryConnectSpecific(choosenAp);
  }
}

/**
 * @brief Try to connect to a specific network ID
 * @param networkId The specific network ID to connect to
 * @return true on success, false on failure
 */
bool WIFIMANAGER::tryConnectSpecific(uint8_t networkId) {
  if (networkId >= WIFIMANAGER_MAX_APS) {
    logMessage("[WIFI][CONNECT] Invalid network ID: " + String(networkId) + "\n");
    return false;
  }

  if (WiFi.getMode() == WIFI_AP) stopSoftAP();

  setMode(WIFI_STA);
  WiFi.begin(apList[networkId].apName.c_str(), apList[networkId].apPass.c_str());
  wl_status_t status = (wl_status_t)WiFi.waitForConnectResult(5000UL);

  auto startTime = millis();
  // wait for connection, fail, or timeout
  while(status != WL_CONNECTED && status != WL_NO_SSID_AVAIL && status != WL_CONNECT_FAILED && (millis() - startTime) <= 10000) {
      delay(10);
      status = (wl_status_t)WiFi.waitForConnectResult(5000UL);
  }
  switch(status) {
    case WL_IDLE_STATUS:
      logMessage("[WIFI][CONNECT] Connecting failed (0): Idle\n");
      break;
    case WL_NO_SSID_AVAIL:
      logMessage("[WIFI][CONNECT] Connecting failed (1): The AP can't be found\n");
      break;
    case WL_SCAN_COMPLETED:
      logMessage("[WIFI][CONNECT] Connecting failed (2): Scan completed\n");
      break;
    case WL_CONNECTED: // 3
      logMessage("[WIFI][CONNECT] Connection successful\n");
      logMessage("[WIFI][CONNECT] SSID   : " + WiFi.SSID() + "\n");
      logMessage("[WIFI][CONNECT] IP     : " + WiFi.localIP().toString() + "\n");
      logMessage("[WIFI][CONNECT] Gateway: " + WiFi.gatewayIP().toString() + "\n");
      logMessage("[WIFI][CONNECT] Subnet : " + WiFi.subnetMask().toString() + "\n");
      logMessage("[WIFI][CONNECT] WebServer should be accessible at http://" + WiFi.localIP().toString() + "/wifi\n");
      return true;
      break;
    case WL_CONNECT_FAILED:
      logMessage("[WIFI][CONNECT] Connecting failed (4): Unknown reason\n");
      break;
    case WL_CONNECTION_LOST:
      logMessage("[WIFI][CONNECT] Connecting failed (5): Connection lost\n");
      break;
    case WL_DISCONNECTED:
      logMessage("[WIFI][CONNECT] Connecting failed (6): Disconnected\n");
      break;
    case WL_NO_SHIELD:
      logMessage("[WIFI][CONNECT] Connecting failed (7): No Wifi shield found\n");
      break;
    default:
      logMessage("[WIFI][CONNECT] Connecting failed (" + String(status) + "): Unknown status code\n");
      break;
  }
  // clean up IP config after failed connection to avoid issues with softAp detection
  WiFi.disconnect(true);
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
bool WIFIMANAGER::startSoftAP(String apName, String apPass) {
  if (apName.length()) this->softApName = apName;
  if (apPass.length()) this->softApPass = apPass;

  if (WiFi.getMode() == WIFI_AP) return true; // already running
  startApTimeMillis = millis();

  if (this->softApName == "") this->softApName = "ESP_" + String((uint32_t)ESP.getEfuseMac());
  logMessage("[WIFI] Starting configuration portal on AP SSID " + this->softApName + "\n");

  // setMode(WIFI_AP); // done by WiFi.softAP
  bool state = WiFi.softAP(this->softApName.c_str(), (this->softApPass.length() ? this->softApPass.c_str() : NULL));
  if (state) {
    dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
    dnsServer.setTTL(60);
    
    // Start DNS server with better error handling
    if (!dnsServer.start(53, "*", WiFi.softAPIP())) {
      logMessage("[WIFI] DNS server failed to start, retrying...\n");
      delay(200);
      dnsServer.start(53, "*", WiFi.softAPIP());
    }

    dnsServerActive = true;
    delay(100);

    attachCaptivePortal();

    logMessage("[WIFI][SOFTAP] SoftAP successfully started\n");
    logMessage("[WIFI][SOFTAP] SSID:        " + this->softApName  + "\n");
    logMessage(String("[WIFI][SOFTAP] Password:    ") + (this->softApPass.length() > 0 ? "***" : "OPEN") + "\n");
    logMessage("[WIFI][SOFTAP] IP Address:  " + WiFi.softAPIP().toString() + "\n");
    logMessage("[WIFI][SOFTAP] IP Subnet:   " + WiFi.softAPSubnetMask().toString() + "\n");
    logMessage("[WIFI][SOFTAP] MAC Address: " + WiFi.softAPmacAddress() + "\n");
    logMessage("[WIFI][SOFTAP] Channel:     " + String(WiFi.channel()) + "\n");
    logMessage(String("[WIFI][SOFTAP] Encryption:  ") + (this->softApPass.length() > 0 ? "WPA2" : "OPEN") + "\n");
    logMessage("[WIFI][SOFTAP] WiFi Power:  " + String(WiFi.getTxPower()) + " dBm\n");
    logMessage("[WIFI][SOFTAP] WiFi Mode:   " + String(WiFi.getMode()) + " (1=STA, 2=AP, 3=AP_STA)\n");

    if (configAvailable()) {
      logMessage("[WIFI][SOFTAP] Will timeout in " + String(timeoutApMillis/1000) + " seconds if no clients connect (saved networks available)\n");
    } else {
      logMessage("[WIFI][SOFTAP] Will run indefinitely (no saved networks configured)\n");
    }
    return true;
  } else {
    logMessage("[WIFI] Unable to create softAP!\n");
    return false;
  }
}

/**
 * @brief Stop/Disconnect a current running SoftAP
 */
void WIFIMANAGER::stopSoftAP() {
  dnsServer.stop();
  dnsServerActive = false;
  delay(100);

  detachCaptivePortal();
  WiFi.softAPdisconnect(false);
  setMode(WIFI_OFF);

  delay(500); // Give more time for interface to shut down completely
  logMessage("[WIFI] SoftAP stopped and DNS server deactivated\n");
}

/**
 * @brief Stop/Disconnect a current wifi connection
 */
void WIFIMANAGER::stopClient() {
  WiFi.disconnect();
  setMode(WIFI_OFF);
}

/**
 * @brief Stop/Disconnect all running wifi activies and optionally kill the background task as well
 * @param killTask true to kill the background task to prevent reconnects
 */
void WIFIMANAGER::stopWifi(bool killTask) {

  dnsServerActive = false;

  if (killTask) {
    vTaskDelete(wifiTaskHandle);
    vTaskDelete(dnsTaskHandle);
  }

  stopSoftAP();
  stopClient();
  setMode(WIFI_OFF);
}

void WIFIMANAGER::attachCaptivePortal() {
  // Check if webServer is initialized before registering handlers
  if (webServer == nullptr) {
    logMessage("[WIFI][WARNING] WebServer not initialized yet, skipping captive portal registration\n");
    return;
  }

  // Reset handler count to prevent overflow
  captivePortalWebHandlerCount = 0;

  // Android Captive Portal Detection - Multiple URLs for better compatibility
  if (captivePortalWebHandlerCount < CAPTIVEPORTAL_MAX_HANDLERS) {
    captivePortalWebHandlers[captivePortalWebHandlerCount++] = &webServer->on("/generate_204", HTTP_GET, [&](AsyncWebServerRequest * request) {
      String host = request->hasHeader("Host") ? request->getHeader("Host")->value() : "unknown";
      String userAgent = request->hasHeader("User-Agent") ? request->getHeader("User-Agent")->value() : "unknown";
      logMessage("[WIFI][CAPTIVE] Android captive portal detection: /generate_204 from host: " + host + ", User-Agent: " + userAgent + "\n");
      
      // Check if this is an Android connectivity check - if so, redirect to trigger captive portal
      if (host.indexOf("connectivitycheck") >= 0 || 
          host.indexOf("clients3.google.com") >= 0 ||
          host.indexOf("clients1.google.com") >= 0 ||
          host.indexOf("android.com") >= 0) {
        
        // Samsung devices need 200 + HTML meta-refresh instead of 302 redirect
        if (userAgent.indexOf("Samsung") >= 0 || userAgent.indexOf("SM-") >= 0 || userAgent.indexOf("GT-") >= 0) {
          logMessage("[WIFI][CAPTIVE] Samsung device detected - sending 200 + HTML meta-refresh\n");
          String htmlResponse = "<html><head><title>Redirecting</title><meta http-equiv='refresh' content='0; url=" + uiPrefix + "'></head><body>Please wait, redirecting to WiFi setup...<br><a href='" + uiPrefix + "'>Click here if not redirected</a></body></html>";
          request->send(200, "text/html", htmlResponse);
          return;
        } else {
          logMessage("[WIFI][CAPTIVE] Standard Android device - sending 302 redirect\n");
          request->redirect(uiPrefix.c_str());
          return;
        }
      } else {
        logMessage("[WIFI][CAPTIVE] Android device using host (" + host + ") not detected - sending 204\n");
      }
      
      // For other requests, send 204 (compatibility)
      request->send(204);
    });
  }

  if (captivePortalWebHandlerCount < CAPTIVEPORTAL_MAX_HANDLERS) {
    captivePortalWebHandlers[captivePortalWebHandlerCount++] = &webServer->on("/gen_204", HTTP_GET, [&](AsyncWebServerRequest * request) {
      logMessage("[WIFI][CAPTIVE] Android captive portal detection: /gen_204\n");
      request->send(204); // Alternative Android endpoint expects 204 No Content (Fallback URL of older devices)
    });
  }

  // Microsoft Captive Portal Detection
  if (captivePortalWebHandlerCount < CAPTIVEPORTAL_MAX_HANDLERS) {
    captivePortalWebHandlers[captivePortalWebHandlerCount++] = &webServer->on("/fwlink", HTTP_GET, [&](AsyncWebServerRequest * request) {
      logMessage("[WIFI][CAPTIVE] Microsoft captive portal detection: /connecttest.txt\n");
      request->redirect(uiPrefix.c_str());
    });
  }

  // Windows Captive Portal Detection
  if (captivePortalWebHandlerCount < CAPTIVEPORTAL_MAX_HANDLERS) {
    captivePortalWebHandlers[captivePortalWebHandlerCount++] = &webServer->on("/connecttest.txt", HTTP_GET, [&](AsyncWebServerRequest * request) {
      logMessage("[WIFI][CAPTIVE] Windows captive portal detection: /connecttest.txt\n");
      request->redirect(uiPrefix.c_str());
    });
  }

  // iOS Captive Portal Detection
  if (captivePortalWebHandlerCount < CAPTIVEPORTAL_MAX_HANDLERS) {
    captivePortalWebHandlers[captivePortalWebHandlerCount++] = &webServer->on("/hotspot-detect.html", HTTP_GET, [&](AsyncWebServerRequest * request) {
      logMessage("[WIFI][CAPTIVE] iOS captive portal detection: /hotspot-detect.html\n");
      request->redirect(uiPrefix.c_str());
    });
  }

  // Ubuntu/Linux Captive Portal Detection
  if (captivePortalWebHandlerCount < CAPTIVEPORTAL_MAX_HANDLERS) {
    captivePortalWebHandlers[captivePortalWebHandlerCount++] = &webServer->on("/connectivity-check", HTTP_GET, [&](AsyncWebServerRequest * request) {
      logMessage("[WIFI][CAPTIVE] Ubuntu captive portal detection: /connectivity-check\n");
      request->redirect(uiPrefix.c_str());
    });
  }

  // Catch-all handler for any unmatched requests (most important for captive portal)
  webServer->onNotFound([&](AsyncWebServerRequest *request) {
    String url = request->url();
    String host = request->hasHeader("Host") ? request->getHeader("Host")->value() : "unknown";

    // Ignore favicon and other assets to reduce log spam
    if (url.endsWith("favicon.ico") || url.endsWith(".png") || url.endsWith(".jpg") || url.endsWith(".js") || url.endsWith(".css")) {
      request->send(404);
      return;
    }

    logMessage("[WIFI][CAPTIVE] Captive portal catch-all: " + host + url + "\n");
    request->redirect("http://" + WiFi.softAPIP().toString() + "/" + uiPrefix.c_str(), 302);
  });
}


void WIFIMANAGER::detachCaptivePortal() {
  // Only remove captive portal detection handlers, keep UI and API handlers
  for (int i = 0; i < captivePortalWebHandlerCount; i++) {
    logMessage("[WIFI] Removing Captive Portal handler: #" + String(i) + "\n");
    webServer->removeHandler(captivePortalWebHandlers[i]);
  }
  captivePortalWebHandlerCount = 0;
  // Note: UI and API handlers remain active for WiFi-connected access
  logMessage("[WIFI] Captive Portal handlers removed, UI/API remain available\n");
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

// Rate limiting for scan endpoint - more permissive
static uint64_t lastScanRequest = 0;
static const uint32_t SCAN_COOLDOWN_MS = 2000;

/**
 * @brief Attach the WebServer to the WifiManager to register the RESTful API
 * @param srv WebServer object
 */
void WIFIMANAGER::attachWebServer(AsyncWebServer * srv) {
  webServer = srv; // store it in the class for later use

  // If SoftAP is already running, register captive portal handlers now
  if (WiFi.getMode() == WIFI_AP && dnsServerActive) {
    logMessage("[WIFI] Registering captive portal handlers for existing SoftAP\n");
    attachCaptivePortal();
  }

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/softap/start").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request) {
    request->send(200, "application/json", "{\"message\":\"Soft AP stopped\"}");
    yield();
    delay(250);
    startSoftAP();
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
    // Validate Content-Type header
    if (!request->hasHeader("Content-Type") || !request->getHeader("Content-Type")->value().startsWith("application/json")) {
      request->send(400, "application/json", "{\"error\":\"Content-Type must be application/json\"}");
      return;
    }

    // Validate request size limits
    if (len == 0 || len > 512) {
      request->send(400, "application/json", "{\"error\":\"Invalid request size (max 512 bytes)\"}");
      return;
    }

    JsonDocument jsonBuffer;
    DeserializationError error = deserializeJson(jsonBuffer, (const char*)data, len);

    if (error) {
      request->send(400, "application/json", "{\"error\":\"Invalid JSON format\"}");
      return;
    }

    // Validate required fields exist and have correct types
    if (!jsonBuffer["apName"].is<String>() || !jsonBuffer["apPass"].is<String>()) {
      request->send(422, "application/json", "{\"error\":\"Missing or invalid required fields: apName, apPass\"}");
      return;
    }

    String apName = jsonBuffer["apName"].as<String>();
    String apPass = jsonBuffer["apPass"].as<String>();

    // Validate SSID length (WiFi standard: 1-32 bytes, ESP32 uses 1-31)
    if (apName.length() < 1 || apName.length() > 31) {
      request->send(422, "application/json", "{\"error\":\"SSID must be 1-31 characters long\"}");
      return;
    }

    // Validate password length (WiFi standard: max 63 characters)
    if (apPass.length() > 63) {
      request->send(422, "application/json", "{\"error\":\"Password must not exceed 63 characters\"}");
      return;
    }

    // Input sanitization - remove dangerous characters
    apName.replace('\0', ' ');
    apPass.replace('\0', ' ');
    apName.trim();
    apPass.trim();

    // Final validation after sanitization
    if (apName.length() == 0) {
      request->send(422, "application/json", "{\"error\":\"SSID cannot be empty after sanitization\"}");
      // Attempt to add WiFi with proper error handling
      return;
    }

    if (!addWifi(apName, apPass)) {
      request->send(500, "application/json", "{\"error\":\"Unable to add WiFi network - storage full or duplicate entry\"}");
    } else {
      request->send(200, "application/json", "{\"message\":\"WiFi network added successfully\"}");
    }
  });

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/id").c_str(), HTTP_DELETE, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    // Validate Content-Type
    if (!request->hasHeader("Content-Type") || !request->getHeader("Content-Type")->value().startsWith("application/json")) {
      request->send(400, "application/json", "{\"error\":\"Content-Type must be application/json\"}");
      return;
    }

    // Validate request size
    if (len == 0 || len > 256) {
      request->send(400, "application/json", "{\"error\":\"Invalid request size (max 256 bytes)\"}");
      return;
    }
    
    JsonDocument jsonBuffer;
    DeserializationError error = deserializeJson(jsonBuffer, (const char*)data, len);

    if (error) {
      request->send(400, "application/json", "{\"error\":\"Invalid JSON format\"}");
      return;
    }

    // Validate required field exists and is valid integer
    if (!jsonBuffer["id"].is<int>()) {
      request->send(422, "application/json", "{\"error\":\"Missing or invalid required field: id (must be integer)\"}");
      return;
    }

    int id = jsonBuffer["id"].as<int>();

    // Validate bounds
    if (id < 0 || id >= WIFIMANAGER_MAX_APS) {
      request->send(422, "application/json", "{\"error\":\"ID out of valid range (0-" + String(WIFIMANAGER_MAX_APS-1) + ")\"}");
      return;
    }

    if (apList[id].apName.isEmpty()) {
      request->send(404, "application/json", "{\"error\":\"No WiFi network found at specified ID\"}");
      return;
    }

    if (!delWifi((uint8_t)id)) {
      request->send(500, "application/json", "{\"error\":\"Unable to delete network entry\"}");
    } else {
      request->send(200, "application/json", "{\"message\":\"WiFi network deleted successfully\"}");
    }
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
    // Rate limiting check
    uint64_t currentTime = millis();
    if (currentTime - lastScanRequest < SCAN_COOLDOWN_MS) {
      uint32_t remainingTime = SCAN_COOLDOWN_MS - (currentTime - lastScanRequest);
      request->send(429, "application/json", 
        "{\"error\":\"Rate limit exceeded. Please wait " + String(remainingTime/1000) + " seconds before scanning again\"}");
      return;
    }
    lastScanRequest = currentTime;

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

  apiWebHandlers[apiWebHandlerCount++] = &webServer->on((apiPrefix + "/connect").c_str(), HTTP_POST, [&](AsyncWebServerRequest * request){}, NULL,
    [&](AsyncWebServerRequest * request, uint8_t *data, size_t len, size_t index, size_t total) {
    // Validate Content-Type
    if (!request->hasHeader("Content-Type") || !request->getHeader("Content-Type")->value().startsWith("application/json")) {
      request->send(400, "application/json", "{\"error\":\"Content-Type must be application/json\"}");
      return;
    }

    if (len == 0 || len > 256) {
      request->send(400, "application/json", "{\"error\":\"Invalid request size\"}");
      return;
    }

    JsonDocument jsonBuffer;
    DeserializationError error = deserializeJson(jsonBuffer, (const char*)data, len);

    if (error) {
      request->send(400, "application/json", "{\"error\":\"Invalid JSON format\"}");
      return;
    }

    if (!jsonBuffer["id"].is<int>()) {
      request->send(422, "application/json", "{\"error\":\"Missing or invalid field: id\"}");
      return;
    }

    int networkId = jsonBuffer["id"].as<int>();
    if (networkId < 0 || networkId >= WIFIMANAGER_MAX_APS) {
      request->send(422, "application/json", "{\"error\":\"Invalid network ID\"}");
      return;
    }

    // Get network name for logging
    String networkName = "Unknown";
    if (!apList[networkId].apName.isEmpty()) networkName = apList[networkId].apName;

    request->send(200, "application/json", "{\"message\":\"Connecting to " + networkName + "\"}");
    yield();
    
    logMessage("[WIFI][API] Starting direct specific connection attempt\n");
    bool connectionResult = tryConnectSpecific(networkId);
    if (connectionResult) {
      logMessage("[WIFI][API] Direct connection successful to " + networkName + "\n");
    } else {
      logMessage("[WIFI][API] Direct connection to " + networkName + " failed, resuming normal WiFi management\n");
    }
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
            display: flex;
            justify-content: space-between;
            align-items: center;
        }

        .network-actions {
            display: flex;
            gap: 8px;
            margin-left: 8px;
        }
        .network-actions button {
            padding: 6px 12px;
            font-size: 0.75rem;
            min-width: 60px;
        }
        
        .btn-connect {
            background: #16a34a;
        }
        .btn-connect:hover {
            background: #15803d;
        }
        .btn-connect-disabled {
            background: #9ca3af;
            cursor: not-allowed;
        }
        .btn-connect-disabled:hover {
            background: #9ca3af;
        }
        .btn-delete {
            background: #dc2626;
        }
        .btn-delete:hover {
            background: #b91c1c;
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

        .password-field {
            position: relative;
        }
        .password-toggle {
            position: absolute;
            right: 8px;
            top: 50%;
            transform: translateY(-50%);
            background: none;
            border: none;
            color: #64748b;
            cursor: pointer;
            padding: 4px;
            font-size: 0.875rem;
        }
        .password-toggle:hover {
            color: var(--primary-color);
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

        <div class="card" id="networkListContainer" style="display: none;">
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
                <div class="password-field">
                    <input type="password" id="apPass">
                    <button type="button" class="password-toggle" onclick="togglePasswordVisibility()" id="passwordToggle">👁️</button>
                </div>
                
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
            // scanNetworks(); // prevent unneccessary disconnects
        });

        async function loadSavedNetworks() {
            try {
                const response = await fetch(`${API_BASE}/wifi/configlist`);
                if (!response.ok) throw new Error('Failed to fetch saved networks');
                
                const savedNetworks = await response.json();

                // Also get current WiFi status to check which network is connected
                let currentSSID = '';
                try {
                    const statusResponse = await fetch(`${API_BASE}/wifi/status`);
                    if (statusResponse.ok) {
                        const status = await statusResponse.json();
                        currentSSID = status.ssid || '';
                    }
                } catch (error) {
                    console.log('Could not fetch current WiFi status:', error);
                }
                
                displaySavedNetworks(savedNetworks, currentSSID);
            } catch (error) {
                showStatus('Failed to load saved networks: ' + error.message, 'error');
            }
        }

        function displaySavedNetworks(networks, currentSSID = '') {
            const networkList = document.getElementById('savedNetworks');
            const networkArray = Object.values(networks);
            
            if (networkArray.length === 0) {
                networkList.innerHTML = '<div class="network-item">No saved networks</div>';
                return;
            }

            networkList.innerHTML = networkArray.map(network => {
                const isConnected = currentSSID && currentSSID === network.apName;
                const connectButtonClass = isConnected ? 'btn-connect-disabled' : 'btn-connect';
                const connectButtonText = isConnected ? 'Connected' : 'Connect';
                const connectButtonDisabled = isConnected ? 'disabled' : '';
                
                return `
                    <div class="network-item">
                        <div class="network-info">
                            <div class="ssid">${network.apName}${isConnected ? ' ✓' : ''}</div>
                        </div>
                        <div class="network-actions">
                            <button class="${connectButtonClass}" 
                                    onclick="connectToSavedNetwork('${network.id}', '${network.apName}')" 
                                    ${connectButtonDisabled}>
                                ${connectButtonText}
                            </button>
                            <button class="btn-delete" onclick="deleteNetwork('${network.id}')">Delete</button>
                        </div>
                    </div>`;
            }).join('');
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
        const networkListContainer = document.getElementById('networkListContainer')
        networkListContainer.style.display = 'block';
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
            <div class="network-item" onclick="showConnectModal('${network.ssid}', ${network.encryptionType === 0})">
              <div class="network-info">
                <div class="ssid">${network.ssid}</div>
                <div class="signal">
                    Signal: ${getSignalStrength(network.rssi)}
                    ${network.encryptionType > 0 ? '🔒' : '🔓'}
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

        function showConnectModal(apName = '', isOpen = false) {
            document.getElementById('apName').value = apName;
            document.getElementById('apName').readOnly = !!apName;
            const passField = document.getElementById('apPass');
            passField.value = '';
            
            // If it's an open network, show a hint and make password optional
            if (isOpen && apName) {
                passField.placeholder = 'No password required (leave empty)';
                passField.style.backgroundColor = '#f0f9ff';
            } else {
                passField.placeholder = '';
                passField.style.backgroundColor = '';
            }
            document.getElementById('connectModal').style.display = 'flex';
        }

        function closeModal() {
            document.getElementById('connectModal').style.display = 'none';
        }

        async function connectToSavedNetwork(networkId, networkName) {
            // Ignore clicks on disabled buttons
            const button = event.target;
            if (button.disabled || button.classList.contains('btn-connect-disabled')) {
                return;
            }
            
            try {
                showStatus(`Connecting to ${networkName}...`, 'info');
                
                // Send specific network ID to connect endpoint
                const response = await fetch(`${API_BASE}/wifi/connect`, {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json',
                    },
                    body: JSON.stringify({ id: parseInt(networkId, 10) })
                });
                
                if (!response.ok) {
                    throw new Error(`HTTP ${response.status}`);
                }
                
                const result = await response.json();
                showStatus(result.message || `Connection initiated for ${networkName}`, 'success');
                
                // Refresh after delay to allow reconnection
                setTimeout(() => {
                    loadSavedNetworks();
                }, 5000);
                
            } catch (error) {
                showStatus(`Failed to connect to ${networkName}: ` + error.message, 'error');
            }
        }
        function togglePasswordVisibility() {
            const passwordField = document.getElementById('apPass');
            const toggleButton = document.getElementById('passwordToggle');
            
            if (passwordField.type === 'password') {
                passwordField.type = 'text';
                toggleButton.innerHTML = '🙈';
                toggleButton.title = 'Hide password';
            } else {
                passwordField.type = 'password';
                toggleButton.innerHTML = '👁️';
                toggleButton.title = 'Show password';
            }
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