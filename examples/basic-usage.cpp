/**
 * Wifi Manager
 * (c) 2022 Martin Verges
 *
 * Licensed under CC BY-NC-SA 4.0
 * (Attribution-NonCommercial-ShareAlike 4.0 International)
**/

#include <Arduino.h>
#include "wifimanager.h"

// Create a instance of the WifiManager
WIFIMANAGER WifiManager;

void setup() {
  Serial.begin(9600);

  // Load well known Wifi AP credentials from NVS
  WifiManager.startBackgroundTask();

  // We do need the Webserver to attach our RESTful API
  AsyncWebServer webServer(80);
  // Attach our API to the Webserver
  WifiManager.attachWebServer(&webServer);
  // Run the Webserver
  webServer.begin();
}

void loop() {
  // your special code to do some good stuff
  delay(100);

  // You can use a GPIO or other event to start a softAP, please replace false with a meaningful event
  if (false) {
    // While we are connected to some WIFI, we can start up a softAP
    // The AP will automatically shut down after some time if no client is connected
    WifiManager.runSoftAP();
  }
}

