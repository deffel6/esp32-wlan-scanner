# ESP32 WLAN-Scanner

Zeigt alle gehörten WLANs auf einem runden Display und prüft auf Wunsch, ob ein
Passwort stimmt. Für **ESP32-C3 mit GC9A01A 240×240** — dieselbe Pinbelegung wie
das [Anker Solix Display](https://github.com/deffel6/anker-solix-display), beide
Sketche laufen also auf demselben Board.

**Aufspielen ohne Werkzeug:** <https://deffel6.github.io/esp32-wlan-scanner/>
(Chrome oder Edge am Rechner, USB-Kabel)

## Was es kann

- Laufender Suchlauf, versteckte Netze eingeschlossen
- Display: Name und Signalstärke, nach Stärke sortiert, seitenweise geblättert;
  grün ab −60 dBm, gelb bis −75, darunter rot; offene Netze orange
- Eigener Zugangspunkt **`WLAN-Scanner`** (offen) mit Weboberfläche unter
  `192.168.4.1`: Netz auswählen, Passwort eingeben, Ergebnis ablesen
- Rückmeldung mit IP-Adresse und Signal — oder mit dem Grund der Ablehnung
  („Passwort falsch", „Netz nicht gefunden", „Netz hat abgelehnt")
- Serielle Ausgabe bei 115200 Baud mit BSSID. Das eingegebene Passwort wird
  **nicht** protokolliert.

## Selbst übersetzen

```
arduino-cli compile --fqbn "esp32:esp32:esp32c3:CDCOnBoot=cdc,CPUFreq=160,FlashFreq=80,FlashMode=qio,FlashSize=4M,PartitionScheme=huge_app" --export-binaries wlan_scanner
```

Gebraucht werden der esp32-Core und **LovyanGFX**.

## Zwei Eigenheiten

Der Anmeldeversuch läuft in `loop()`, nicht im Webserver-Handler: Beim Verbinden
wechselt der Funkkanal, eine dort blockierende Antwort käme nie beim Browser an.

„Passwort falsch" steht nicht in `WiFi.status()` — der Grund kommt nur über das
Ereignis `STA_DISCONNECTED`. Ohne diesen Haken ließe sich ein falsches Passwort
nicht von einem verschwundenen Netz unterscheiden.

## Lizenz

PolyForm Noncommercial 1.0.0
