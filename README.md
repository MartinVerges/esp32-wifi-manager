# ESP32 Wifi Manager 

This multi wifi manager runs your ESP32 in a AP+STA mode.
This way you can easily start a softAP and scan for Wifi Networks while beeing conntected to some wifi.

You can store multiple SSIDs and it will try to connect, reconnect and optionally fallback to softAP.

## Why?

Because all the available solution that I found, where unable to fullfill my requirements.

## Focus of this Project

It is only made and tested it on my ESP32 Microcontroller.

## How is it working?

1) First of all, if there is no configuration of a known SSID, it starts an AP.
   Within the AP you can use your own WebUI using RESTful APIs to configure the AP.

2) When a initial SSID is stored into the local database, the background task will try to connect to it.
   On success, the AP will timeout and close it self.
   You can close the AP if you like to speed that up.

3) It's possible to add multiple SSIDs to connect to.
   If there are more known SSIDs, the WifiManager will scan for SSIDs in range and pick the strongest Signal to connect to.

4) The background task monitors the Wifi status and reconnect if required.

### More detailed flow diagram

<img src="documentation/flow-diagram.png?raw=true" alt="Flow diagram" width="40%">

## What do I need to do?

If you want to use this wifi manager, you have to create your own Web UI for it!
It won't give you a premade styled UI to configure the SSID credentials.

## APIs

| Method | Request URL             | Json Body                                    | Info                                                            |
| ------ | ----------------------- | -------------------------------------------- | --------------------------------------------------------------- |
| GET    | /api/wifi/configlist    | none                                         | Get the configured SSID AP list                                 |
| GET    | /api/wifi/scan          | none                                         | Scan for Networks in Range                                      |
| GET    | /api/wifi/status        | none                                         | Show Status of the ESP32                                        |
| POST   | /api/wifi/add           | `{ "apName": "mySSID", "apPass": "secret" }` | Add a new SSID to the AP list                                   |
| DELETE | /api/wifi/id            | `{ "id": 1 }`                                | Drop the AP list entry using the ID                             |
| DELETE | /api/wifi/apName        | `{ "apName": "mySSID" }`                     | Drop the AP list entries identified by the AP (SSID) Name       |
| POST   | /api/wifi/softAp/start  | none                                         | Open/Create a softAP. Used to switch from client to AP mode     |
| POST   | /api/wifi/softAp/stop   | none                                         | Disconnect the softAP and start to connect to known SSIDs       |
| POST   | /api/wifi/client/stop   | none                                         | Disconnect current wifi connection, start to search and connect |

## Dependencies

This Wifi manager depends on some external libraries to provide the functionality.
These are:

* Arduino 
* Preferences
* ArduinoJson

Optional:

* ESPAsyncWebServer - use compiler flag `-DASYNC_WEBSERVER=true` (default)

## ESPAsyncWebserver vs Arduino Standard Webserver

After it is not possible to use the `ESPAsyncWebserver.h` dependency in some projects, the simpler standard Arduino `WebServer.h` can be used. 
To switch to the legacy Webserver use the compiler flag `-DASYNC_WEBSERVER=false`.

# License

esp32-wifi-manager (c) by Martin Verges.

This project is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License.

You should have received a copy of the license along with this work.
If not, see <http://creativecommons.org/licenses/by-nc-sa/4.0/>.

## Commercial Licenses 

If you want to use this software on a comercial product, you can get an commercial license on request.
