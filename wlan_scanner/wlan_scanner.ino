/*
  WLAN-Scanner mit Anmeldeportal - ESP32-C3 mit rundem GC9A01A 240x240

  Das Board oeffnet einen eigenen Zugangspunkt "WLAN-Scanner" (offen, ohne
  Passwort). Wer sich damit verbindet, bekommt im Browser die Liste aller
  gehoerten Netze, waehlt eins aus, gibt das Passwort ein - und sieht danach,
  ob die Anmeldung geklappt hat. Auf dem Display laeuft dasselbe mit.

  Gleiche Pinbelegung wie das Anker-Display, laeuft also auf demselben Board.

  Im seriellen Monitor (115200) steht die vollstaendige Liste mit BSSID.
  Das eingegebene Passwort wird bewusst NICHT protokolliert.
*/
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

class LGFX : public lgfx::LGFX_Device {
  lgfx::Panel_GC9A01 _panel;
  lgfx::Bus_SPI      _bus;
  lgfx::Light_PWM    _light;
public:
  LGFX() {
    { auto c=_bus.config(); c.spi_host=SPI2_HOST; c.spi_mode=0;
      c.freq_write=40000000; c.freq_read=16000000;
      c.spi_3wire=true; c.use_lock=true; c.dma_channel=SPI_DMA_CH_AUTO;
      c.pin_sclk=6; c.pin_mosi=7; c.pin_miso=-1; c.pin_dc=2;
      _bus.config(c); _panel.setBus(&_bus); }
    { auto c=_panel.config(); c.pin_cs=10; c.pin_rst=1; c.pin_busy=-1;
      c.panel_width=240; c.panel_height=240; c.invert=true; c.rgb_order=false;
      _panel.config(c); }
    { auto c=_light.config(); c.pin_bl=3; c.invert=false;
      c.freq=44100; c.pwm_channel=7;
      _light.config(c); _panel.setLight(&_light); }
    setPanel(&_panel);
  }
};
static LGFX lcd;
static LGFX_Sprite spr(&lcd);

#define C_WHITE  lcd.color888(255,255,255)
#define C_GRAY   lcd.color888(120,120,120)
#define C_RED    lcd.color888(255, 60, 60)
#define C_GREEN  lcd.color888(  0,255,120)
#define C_YELLOW lcd.color888(255,210,  0)
#define C_ORANGE lcd.color888(255,140,  0)
#define C_BLUE   lcd.color888(  0,170,255)
#define C_BLACK  lcd.color888(  0,  0,  0)

#define AP_SSID   "WLAN-Scanner"
#define AP_IP     "192.168.4.1"
#define MAX_NETS   40
#define ROWS        6
#define SCAN_MS  8000UL
#define PAGE_MS  3000UL
#define TRY_MS  20000UL     // so lange warten wir auf die Anmeldung

struct Net {
  String   ssid;
  int32_t  rssi;
  int32_t  ch;
  bool     open;
  int      idx;
};
static Net  gNets[MAX_NETS];
static int  gCount    = 0;
static int  gPage     = 0;
static bool gScanning = false;
static unsigned long gLastScan = 0, gLastPage = 0;

// Zustand der Anmeldung. Der Versuch selbst laeuft in loop(), nicht im
// Webserver-Handler: dort wuerde er die Seite blockieren, und genau waehrend
// des Verbindens wechselt der Funkkanal - die Antwort kaeme nie an.
enum TryState { T_IDLE, T_WANTED, T_RUNNING, T_OK, T_FAIL };
static TryState gTry = T_IDLE;
static String   gTrySsid, gTryPass, gTryMsg;
static unsigned long gTryStart = 0;
static uint8_t  gLastReason = 0;

static WebServer server(80);
static DNSServer dns;

// ── Anzeige ────────────────────────────────────────────────────────────────
static uint32_t rssiColor(int32_t r){
  if(r>=-60) return C_GREEN;
  if(r>=-75) return C_YELLOW;
  return C_RED;
}
static String shorten(const String& s, unsigned max){
  if(s.length()<=max) return s;
  return s.substring(0,max-2)+"..";
}
static void drawLines(const char* l1,uint32_t c1,const char* l2,uint32_t c2,
                      const char* l3,uint32_t c3){
  spr.fillScreen(C_BLACK);
  spr.setTextDatum(lgfx::TC_DATUM);
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextColor(c1,C_BLACK); spr.drawString(l1,120,78);
  spr.setFont(&fonts::FreeSans9pt7b);
  spr.setTextColor(c2,C_BLACK); spr.drawString(l2,120,112);
  spr.setTextColor(c3,C_BLACK); spr.drawString(l3,120,140);
  spr.pushSprite(0,0);
}

static void drawList(){
  // Waehrend eines Anmeldeversuchs zeigt das Display dessen Stand, nicht die
  // Liste - sonst waere der wichtigere Vorgang unsichtbar.
  if(gTry==T_WANTED || gTry==T_RUNNING){
    drawLines("Verbinde...",C_ORANGE,gTrySsid.c_str(),C_WHITE,"",C_GRAY); return;
  }
  if(gTry==T_OK){
    drawLines("Verbunden",C_GREEN,gTrySsid.c_str(),C_WHITE,
              WiFi.localIP().toString().c_str(),C_BLUE); return;
  }
  if(gTry==T_FAIL){
    drawLines("Fehlgeschlagen",C_RED,gTrySsid.c_str(),C_WHITE,
              gTryMsg.c_str(),C_GRAY); return;
  }

  spr.fillScreen(C_BLACK);
  spr.setTextDatum(lgfx::TC_DATUM);
  spr.setFont(&fonts::FreeSansBold12pt7b);
  spr.setTextColor(C_ORANGE,C_BLACK);
  char h[24]; snprintf(h,sizeof(h),"%d Netze",gCount);
  spr.drawString(h,120,20);

  int pages=(gCount+ROWS-1)/ROWS; if(pages<1) pages=1;
  if(gPage>=pages) gPage=0;

  spr.setFont(&fonts::FreeSans9pt7b);
  for(int r=0;r<ROWS;r++){
    int idx=gPage*ROWS+r;
    if(idx>=gCount) break;
    Net& n=gNets[idx];
    int y=56+r*21;
    spr.setTextDatum(lgfx::TL_DATUM);
    spr.setTextColor(n.open?C_ORANGE:C_WHITE,C_BLACK);
    spr.drawString(shorten(n.ssid,12),34,y);
    spr.setTextDatum(lgfx::TR_DATUM);
    spr.setTextColor(rssiColor(n.rssi),C_BLACK);
    char v[10]; snprintf(v,sizeof(v),"%d",(int)n.rssi);
    spr.drawString(v,206,y);
  }
  // Fusszeile: der Weg zur Bedienung, damit man ihn nicht raten muss
  spr.setTextDatum(lgfx::TC_DATUM);
  spr.setTextColor(C_GRAY,C_BLACK);
  spr.drawString(AP_SSID,120,186);
  spr.pushSprite(0,0);
}

// ── Suchlauf ───────────────────────────────────────────────────────────────
static void takeResult(int n){
  gCount = n>MAX_NETS ? MAX_NETS : n;
  for(int i=0;i<gCount;i++){
    gNets[i].ssid = WiFi.SSID(i);
    if(gNets[i].ssid.length()==0) gNets[i].ssid="(versteckt)";
    gNets[i].rssi = WiFi.RSSI(i);
    gNets[i].ch   = WiFi.channel(i);
    gNets[i].open = (WiFi.encryptionType(i)==WIFI_AUTH_OPEN);
    gNets[i].idx  = i;
  }
  for(int i=1;i<gCount;i++){
    Net k=gNets[i]; int j=i-1;
    while(j>=0 && gNets[j].rssi<k.rssi){ gNets[j+1]=gNets[j]; j--; }
    gNets[j+1]=k;
  }
  Serial.printf("\n[SCAN] %d Netze gehoert (%d angezeigt)\n",n,gCount);
  Serial.println("  # Name                 dBm  Kanal  Verschluesselung  BSSID");
  for(int i=0;i<gCount;i++)
    Serial.printf("%3d %-20s %4d  %5d  %-16s  %s\n",
      i+1,gNets[i].ssid.c_str(),(int)gNets[i].rssi,(int)gNets[i].ch,
      gNets[i].open?"offen":"verschluesselt",
      WiFi.BSSIDstr(gNets[i].idx).c_str());
  WiFi.scanDelete();
}

// ── Weboberflaeche ─────────────────────────────────────────────────────────
static const char* CSS =
  "<style>*{box-sizing:border-box;margin:0;padding:0}html{font-size:18px}"
  "body{font-family:-apple-system,sans-serif;background:#0a0a0a;color:#eee;"
  "display:flex;justify-content:center;padding:20px}"
  ".card{background:#1a1a1a;border-radius:16px;padding:24px;width:100%;max-width:470px}"
  "h1{font-size:1.25rem;margin-bottom:4px}"
  ".sub{color:#888;font-size:.85rem;margin-bottom:18px}"
  "a.net{display:flex;justify-content:space-between;gap:10px;padding:12px 14px;"
  "margin-bottom:8px;background:#222;border-radius:10px;text-decoration:none;color:#eee}"
  "a.net small{color:#888;font-size:.75rem;white-space:nowrap}"
  "input{width:100%;padding:11px;margin:10px 0;background:#222;border:1px solid #333;"
  "border-radius:8px;color:#eee;font-size:1rem}"
  "button{width:100%;padding:13px;background:#f0a500;color:#000;border:none;"
  "border-radius:10px;font-weight:700;font-size:1rem;cursor:pointer}"
  "a{color:#f0a500}.ok{color:#4caf50}.bad{color:#f66}</style>";

static String htmlHead(const String& title){
  return "<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>"
         "<meta name='viewport' content='width=device-width,initial-scale=1'>"
         "<title>"+title+"</title>"+CSS+"</head><body><div class='card'>";
}
static String esc(const String& s){
  String o; o.reserve(s.length()+8);
  for(unsigned i=0;i<s.length();i++){
    char c=s[i];
    if(c=='<')o+="&lt;"; else if(c=='>')o+="&gt;";
    else if(c=='&')o+="&amp;"; else if(c=='\'')o+="&#39;";
    else if(c=='"')o+="&quot;"; else o+=c;
  }
  return o;
}

static void handleRoot(){
  String h=htmlHead("WLAN-Scanner");
  h+="<h1>&#128246; Gehoerte Netze</h1>";
  h+="<p class='sub'>"+String(gCount)+" gefunden &middot; "
     "<a href='/rescan'>neu suchen</a></p>";
  if(gTry==T_OK)
    h+="<p class='sub ok'>Verbunden mit "+esc(gTrySsid)+" &middot; "
       +WiFi.localIP().toString()+"</p>";
  for(int i=0;i<gCount;i++){
    h+="<a class='net' href='/pass?ssid="+esc(gNets[i].ssid)+"'>"
       "<span>"+esc(gNets[i].ssid)+"</span>"
       "<small>"+String((int)gNets[i].rssi)+" dBm &middot; K"+String((int)gNets[i].ch)
       +(gNets[i].open?" &middot; offen":"")+"</small></a>";
  }
  if(!gCount) h+="<p class='sub'>Noch nichts gefunden &ndash; der erste "
                 "Suchlauf dauert einen Moment.</p>";
  h+="</div></body></html>";
  server.send(200,"text/html; charset=utf-8",h);
}

static void handlePass(){
  String ssid=server.arg("ssid");
  String h=htmlHead("Anmelden");
  h+="<h1>"+esc(ssid)+"</h1>";
  h+="<p class='sub'>Passwort eingeben und anmelden.</p>";
  h+="<form action='/try' method='POST'>";
  h+="<input type='hidden' name='ssid' value='"+esc(ssid)+"'>";
  h+="<input type='password' name='pass' placeholder='Passwort' autofocus>";
  h+="<button type='submit'>Anmelden</button></form>";
  h+="<p class='sub' style='margin-top:14px'><a href='/'>&larr; zurueck</a></p>";
  h+="</div></body></html>";
  server.send(200,"text/html; charset=utf-8",h);
}

static void handleTry(){
  gTrySsid=server.arg("ssid");
  gTryPass=server.arg("pass");
  gTry=T_WANTED;                 // loop() faengt gleich an
  gTryMsg="";
  gLastReason=0;
  Serial.printf("[TRY] Anmeldung bei '%s' angefordert\n",gTrySsid.c_str());
  server.sendHeader("Location","/status"); server.send(302);
}

static void handleStatus(){
  String h="<!DOCTYPE html><html lang='de'><head><meta charset='UTF-8'>"
           "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  // Solange es laeuft, laedt die Seite sich selbst nach. Steht das Ergebnis
  // fest, hoert sie damit auf - sonst blinkt die Meldung im Sekundentakt.
  if(gTry==T_WANTED||gTry==T_RUNNING) h+="<meta http-equiv='refresh' content='2'>";
  h+="<title>Anmeldung</title>"+String(CSS)+"</head><body><div class='card'>";
  if(gTry==T_OK){
    h+="<h1 class='ok'>&#10003; Verbunden</h1>";
    h+="<p class='sub'>Das Passwort war richtig.</p>";
    h+="<p>Netz: <b>"+esc(gTrySsid)+"</b><br>";
    h+="IP-Adresse: <b>"+WiFi.localIP().toString()+"</b><br>";
    h+="Signal: <b>"+String((int)WiFi.RSSI())+" dBm</b></p>";
  } else if(gTry==T_FAIL){
    h+="<h1 class='bad'>&#10007; Fehlgeschlagen</h1>";
    h+="<p class='sub'>"+esc(gTryMsg)+"</p>";
    h+="<p>Netz: <b>"+esc(gTrySsid)+"</b></p>";
  } else {
    h+="<h1>Verbinde&hellip;</h1>";
    h+="<p class='sub'>Anmeldung bei "+esc(gTrySsid)+" laeuft. Das dauert "
       "einige Sekunden; die Seite frischt sich von selbst auf.</p>";
  }
  h+="<p class='sub' style='margin-top:16px'><a href='/'>&larr; zur Liste</a></p>";
  h+="</div></body></html>";
  server.send(200,"text/html; charset=utf-8",h);
}

static void handleRescan(){
  gLastScan=0;                   // beim naechsten Durchlauf sofort suchen
  server.sendHeader("Location","/"); server.send(302);
}

// Anmeldeseiten der Betriebssysteme: ohne diese Antworten oeffnet sich das
// Portal beim Verbinden nicht von selbst.
static void handleNotFound(){
  server.sendHeader("Location",String("http://")+AP_IP+"/",true);
  server.send(302,"text/plain","");
}

// ── WLAN-Ereignisse ────────────────────────────────────────────────────────
// Der Grund der Ablehnung steht nur im Ereignis, nicht in WiFi.status().
// Ohne ihn liesse sich "Passwort falsch" nicht von "Netz weg" unterscheiden.
static void onWiFiEvent(WiFiEvent_t ev, WiFiEventInfo_t info){
  if(ev!=ARDUINO_EVENT_WIFI_STA_DISCONNECTED) return;
  uint8_t r=info.wifi_sta_disconnected.reason;
  Serial.printf("[WIFI] getrennt, Grund %u\n",(unsigned)r);
  // 8 und 36 meldet das Board, wenn es sich SELBST abmeldet - also genau
  // das, was disconnect() vor jedem Versuch tut. Als Fehlergrund gemerkt,
  // wuerden sie den echten Grund ueberdecken.
  if(r==WIFI_REASON_ASSOC_LEAVE || r==WIFI_REASON_STA_LEAVING) return;
  gLastReason=r;
}
static String reasonText(uint8_t r){
  switch(r){
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
                                             return "Netz nicht gefunden";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: return "Passwort falsch";
    case WIFI_REASON_ASSOC_FAIL:             return "Netz hat abgelehnt";
    case WIFI_REASON_CONNECTION_FAIL:        return "Verbindung kam nicht zustande";
    case WIFI_REASON_BEACON_TIMEOUT:         return "Netz nicht mehr zu hoeren";
    case WIFI_REASON_STA_LEAVING:
    case WIFI_REASON_ASSOC_LEAVE:            return "vom Board selbst getrennt";
    case 0:                                  return "Zeit abgelaufen, kein Grund gemeldet";
    default: return String("abgelehnt, Grund ")+String((int)r);
  }
}

void setup(){
  Serial.begin(115200); delay(300);
  Serial.println("\n[BOOT] WLAN-Scanner mit Portal");

  lcd.init(); lcd.setRotation(0); lcd.setBrightness(180);
  lcd.fillScreen(C_BLACK);
  spr.setColorDepth(8);
  if(!spr.createSprite(240,240)) Serial.println("[SPR] RAM zu wenig");
  drawLines("Suche...",C_ORANGE,AP_SSID,C_WHITE,AP_IP,C_GRAY);

  WiFi.onEvent(onWiFiEvent);
  // AP und Station gleichzeitig: der Zugangspunkt bleibt bestehen, waehrend
  // sich das Board bei einem fremden Netz anmeldet.
  WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  IPAddress ip(192,168,4,1), gw(192,168,4,1), sn(255,255,255,0);
  WiFi.softAPConfig(ip,gw,sn);
  WiFi.softAP(AP_SSID);          // offen, ohne Passwort
  delay(200);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dns.start(53,"*",ip);

  server.on("/",       HTTP_GET,  handleRoot);
  server.on("/pass",   HTTP_GET,  handlePass);
  server.on("/try",    HTTP_POST, handleTry);
  server.on("/status", HTTP_GET,  handleStatus);
  server.on("/rescan", HTTP_GET,  handleRescan);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("[AP] %s -> http://%s/\n",AP_SSID,AP_IP);
}

void loop(){
  unsigned long now=millis();
  dns.processNextRequest();
  server.handleClient();

  // Anmeldeversuch starten
  if(gTry==T_WANTED){
    // Ein noch laufender Suchlauf muss zuerst weg: er springt im
    // Hundertstelsekundentakt zwischen den Kanaelen, und die Anmeldung
    // scheitert daran zuverlaessig.
    if(gScanning){
      WiFi.scanDelete();
      gScanning=false;
      Serial.println("[TRY] laufenden Suchlauf abgebrochen");
    }
    gTry=T_RUNNING;
    gTryStart=now;
    WiFi.disconnect();
    delay(120);          // das eigene Abmelde-Ereignis noch abwarten
    gLastReason=0;       // ... und erst danach den Grund scharf stellen
    // Passwort leer lassen, wenn das Netz offen ist - begin() mit leerem
    // Passwort ist genau dafuer vorgesehen.
    WiFi.begin(gTrySsid.c_str(), gTryPass.length()?gTryPass.c_str():nullptr);
    drawList();
  }
  // ... und auswerten
  if(gTry==T_RUNNING){
    if(WiFi.status()==WL_CONNECTED){
      gTry=T_OK;
      Serial.printf("[TRY] OK - %s, IP %s, %d dBm\n",
        gTrySsid.c_str(),WiFi.localIP().toString().c_str(),(int)WiFi.RSSI());
      drawList();
    } else if(now-gTryStart>=TRY_MS){
      gTry=T_FAIL;
      gTryMsg=reasonText(gLastReason);
      WiFi.disconnect();
      Serial.printf("[TRY] fehlgeschlagen: %s\n",gTryMsg.c_str());
      drawList();
    }
  }

  // Suchlauf. Waehrend einer laufenden Anmeldung pausiert er: ein Scan
  // wechselt staendig den Kanal und wuerde die Anmeldung stoeren.
  if(!gScanning && gTry!=T_WANTED && gTry!=T_RUNNING && now-gLastScan>=SCAN_MS){
    WiFi.scanNetworks(true,true);
    gScanning=true;
  }
  if(gScanning){
    int n=WiFi.scanComplete();
    if(n>=0){
      takeResult(n);
      gScanning=false; gLastScan=millis(); gLastPage=gLastScan; gPage=0;
      drawList();
    } else if(n==WIFI_SCAN_FAILED){
      gScanning=false; gLastScan=millis();
      Serial.println("[SCAN] fehlgeschlagen");
    }
  }

  if(gTry==T_IDLE && gCount>ROWS && now-gLastPage>=PAGE_MS){
    gLastPage=now; gPage++; drawList();
  }

  delay(5);
}
