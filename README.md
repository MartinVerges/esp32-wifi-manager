# ESP32 Wifi Manager 

This multi wifi manager runs your ESP32 in a AP+STA mode in a non blocking mode on core 0.
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

<img src="documentation/flow-diagram.png?raw=true" alt="Flow diagram" width="40%" style="background-color: white">

## What do I need to do?

If you want to use this wifi manager, you can create your own Web UI or use a prebuild one available at `/wifi`!

## Build in UI

The UI route is only loaded to `/wifi` if you execute `attachUI()` inside your script.

<img src="documentation/ui.png?raw=true" alt="Build in UI" width="40%">


## APIs

| Method | Request URL             | Json Body                                    | Info                                                            |
| ------ | ----------------------- | -------------------------------------------- | --------------------------------------------------------------- |
| GET    | /api/wifi/configlist    | none                                         | Get the configured SSID AP list                                 |
| GET    | /api/wifi/scan          | none                                         | Async Scan for Networks in Range.                               |
| GET    | /api/wifi/status        | none                                         | Show Status of the ESP32                                        |
| POST   | /api/wifi/add           | `{ "apName": "mySSID", "apPass": "secret" }` | Add a new SSID to the AP list                                   |
| DELETE | /api/wifi/id            | `{ "id": 1 }`                                | Drop the AP list entry using the ID                             |
| DELETE | /api/wifi/apName        | `{ "apName": "mySSID" }`                     | Drop the AP list entries identified by the AP (SSID) Name       |
| POST   | /api/wifi/softap/start  | none                                         | Open/Create a softAP. Used to switch from client to AP mode     |
| POST   | /api/wifi/softap/stop   | none                                         | Disconnect the softAP and start to connect to known SSIDs       |
| POST   | /api/wifi/client/stop   | none                                         | Disconnect current wifi connection, start to search and connect |

### Async scanning

Due to the way scanning is done on a ESP32, it looks like it's common that your client disconnects.
So wait long enough between first and second scan.
On the 1st request a response body containing `{"status":"scanning"}` will be send to the client.
When you do a scan after long enough wait time, you receive an array of objects.
Example: 
```
[ 
   {"channel": 1, "encryptionType": 3, "rssi": -80, "ssid": "My Wifi Network"},
   {"channel": 11, "encryptionType": 0, "rssi": -50, "ssid": "My Public Network"}
]
```
PS: If you find a way to prevent the client disconnect on scan, please let me know!

## Dependencies

This Wifi manager depends on some external libraries to provide the functionality.
These are:

* Arduino 
* Preferences
* ArduinoJson
* ESPAsyncWebServer

# License

esp32-wifi-manager (c) by Martin Verges.

This project is licensed under a Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License.

You should have received a copy of the license along with this work.
If not, see <http://creativecommons.org/licenses/by-nc-sa/4.0/>.

## Commercial Licenses 

If you want to use this software on a comercial product, you can get an commercial license on request.
