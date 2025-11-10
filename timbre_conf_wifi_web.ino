#include <ESP8266WiFi.h>
#include <EEPROM.h>

extern "C" {
  #include "user_interface.h"   // wifi_set_country
}

/* ======================
   SOLO WIFI + RELÉ (ESP8266)
   ====================== */

// ----- CONFIGURA AQUÍ -----
#define RELAY_PIN               0       // ⚠️ D3/GPIO0. Mejor D5=14 o D6=12 si puedes.
#define RELAY_ACTIVE_LEVEL      LOW
#define WIFI_CONNECT_TIMEOUT_MS 30000
// --------------------------

#define EEPROM_SIZE 200
#define ADDR_SSID   0
#define ADDR_PASS   100

WiFiEventHandler ehDisc, ehConn, ehIP;
String wifi_ssid, wifi_pass;

/* ---------- País / Canales 12-13 ---------- */
void allowCh12_13() {
  wifi_country_t c = { "MX", 1, 13, WIFI_COUNTRY_POLICY_MANUAL };
  wifi_set_country(&c);
}

/* ---------- Relé ---------- */
void relayOn()  { digitalWrite(RELAY_PIN, RELAY_ACTIVE_LEVEL);  Serial.println("RELAY:ON");  }
void relayOff() { digitalWrite(RELAY_PIN, !RELAY_ACTIVE_LEVEL); Serial.println("RELAY:OFF"); }

/* ---------- Checksum simple ---------- */
uint16_t csum(const String& s){
  uint16_t x=0; for (size_t i=0;i<s.length();++i) x = (x*131) ^ (uint8_t)s[i]; return x;
}

/* ---------- EEPROM ---------- */
void saveWifiCredentials(const String &ssid, const String &pass) {
  EEPROM.begin(EEPROM_SIZE);
  for (int i = 0; i < 100; i++) EEPROM.write(ADDR_SSID + i, 0);
  for (int i = 0; i < 100; i++) EEPROM.write(ADDR_PASS + i, 0);
  for (int i = 0; i < (int)ssid.length() && i < 99; i++) EEPROM.write(ADDR_SSID + i, ssid[i]);
  for (int i = 0; i < (int)pass.length() && i < 99; i++) EEPROM.write(ADDR_PASS + i, pass[i]);
  EEPROM.write(ADDR_SSID + 99, 0);
  EEPROM.write(ADDR_PASS + 99, 0);
  EEPROM.commit();
}

void loadWifiCredentials() {
  EEPROM.begin(EEPROM_SIZE);
  char ssid_c[100]; char pass_c[100];
  for (int i = 0; i < 100; i++) ssid_c[i] = EEPROM.read(ADDR_SSID + i);
  for (int i = 0; i < 100; i++) pass_c[i] = EEPROM.read(ADDR_PASS + i);
  ssid_c[99]=0; pass_c[99]=0;
  wifi_ssid = String(ssid_c); wifi_ssid.trim();
  wifi_pass = String(pass_c); wifi_pass.trim();
}

/* ---------- Estado por Serial ---------- */
void printNetStatus() {
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("NET:OK ip=%s rssi=%d\n",
      WiFi.localIP().toString().c_str(), WiFi.RSSI());
  } else {
    Serial.println("NET:ERR");
  }
}

/* ---------- Escaneo y conexión dirigida ---------- */
bool scanForTarget(String target, int &bestIndex, int &bestCh, bool &hidden, uint8_t bestBSSID[6]) {
  bestIndex = -1; bestCh = 0; hidden = false;
  int n = WiFi.scanNetworks(false, true); // async=false, show_hidden=true
  Serial.printf("SCAN:found=%d\n", n);
  int bestRssi = -1000;
  for (int i=0; i<n; i++) {
    String ss = WiFi.SSID(i);
    int ch = WiFi.channel(i);
    int r = WiFi.RSSI(i);
    bool hid = false; // ESP8266 no expone isHidden(i) de forma estable; asumimos false
    Serial.printf("AP:%d ssid=%s ch=%d rssi=%d enc=%d\n", i, ss.c_str(), ch, r, WiFi.encryptionType(i));
    if (ss == target) {
      if (r > bestRssi) {
        bestRssi = r; bestIndex = i; bestCh = ch; hidden = hid;
        // BSSID de la red candidata
        // Nota: en algunas versiones BSSID(i) retorna puntero; copiamos por seguridad
        const uint8_t* p = WiFi.BSSID(i);
        if (p) { for (int k=0;k<6;k++) bestBSSID[k]=p[k]; } else { for(int k=0;k<6;k++) bestBSSID[k]=0; }
      }
    }
  }
  return (bestIndex >= 0);
}

bool connectWithParams(const String& ssid, const String& pass) {
  Serial.printf("NET:TRY ssid=%s\n", ssid.c_str());

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(120);

  // Escaneo primero
  int idx, ch; bool hid; uint8_t bssid[6]={0};
  bool found = scanForTarget(ssid, idx, ch, hid, bssid);

  if (found) {
    Serial.printf("[WIFI] Encontrado \"%s\" en canal %d. Probando con BSSID y canal...\n", ssid.c_str(), ch);
    WiFi.begin(ssid.c_str(), pass.c_str(), ch, bssid, false);
  } else {
    Serial.println("[WIFI] SSID no visto en escaneo. Intento genérico...");
    WiFi.begin(ssid.c_str(), pass.c_str());
  }

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250); Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() != WL_CONNECTED && found) {
    Serial.println("[WIFI] Reintentando como red OCULTA (forzando canal) ...");
    WiFi.disconnect(true);
    delay(120);
    WiFi.begin(ssid.c_str(), pass.c_str(), ch, bssid, true); // hidden=true
    start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
      delay(250); Serial.print(".");
    }
    Serial.println();
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WIFI] Conectado");
    Serial.printf("[WIFI] IP: %s\n", WiFi.localIP().toString().c_str());
    printNetStatus();
    relayOn();
    return true;
  } else {
    Serial.println("[WIFI] ERROR, no se pudo conectar.");
    printNetStatus();
    relayOff();
    return false;
  }
}

/* ---------- Parser de comandos ---------- */
void handleConfigJSON(const String& json) {
  int ss1 = json.indexOf("\"ssid\"");
  int ps1 = json.indexOf("\"pass\"");
  if (ss1 < 0 || ps1 < 0) { Serial.println("CFG:ERR parse"); return; }

  int ss_start = json.indexOf('"', json.indexOf(':', ss1) + 1) + 1;
  int ss_end   = json.indexOf('"', ss_start);
  int ps_start = json.indexOf('"', json.indexOf(':', ps1) + 1) + 1;
  int ps_end   = json.indexOf('"', ps_start);

  String ssid = json.substring(ss_start, ss_end);
  String pass = json.substring(ps_start, ps_end);

  Serial.printf("CFG:RECV ssid=%s len=%d csum=%u\n",
                ssid.c_str(), pass.length(), csum(ssid+":"+pass));

  saveWifiCredentials(ssid, pass);
  Serial.println("CFG:SAVED");
  Serial.println("[CONFIG] Reiniciando...");
  delay(400);
  ESP.restart();
}

void dumpEEPROM(){
  EEPROM.begin(EEPROM_SIZE);
  Serial.print("DUMP:SSID=\"");
  for(int i=0;i<100;i++){ char c=EEPROM.read(ADDR_SSID+i); if(!c)break; Serial.print(c); }
  Serial.print("\" PASS_LEN=");
  int len=0; for(int i=0;i<100;i++){ if(!EEPROM.read(ADDR_PASS+i)) break; len++; }
  Serial.println(len);
}

void checkSerial() {
  static String buffer;
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') {
      buffer.trim();
      if (buffer.startsWith("CONFIG_")) {
        int sep = buffer.indexOf(':');
        if (sep > 0) handleConfigJSON(buffer.substring(sep + 1));
      } else if (buffer == "STATUS?") {
        printNetStatus();
      } else if (buffer == "CLEAR") {
        saveWifiCredentials("", ""); Serial.println("CFG:CLEARED");
      } else if (buffer == "DUMP") {
        dumpEEPROM();
      } else if (buffer == "SCAN?") {
        // Escaneo manual
        int n = WiFi.scanNetworks(false, true);
        Serial.printf("SCAN:found=%d\n", n);
        for (int i=0;i<n;i++){
          Serial.printf("AP:%d ssid=%s ch=%d rssi=%d enc=%d\n",
            i, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), WiFi.encryptionType(i));
        }
      }
      buffer = "";
    } else {
      buffer += c;
      if (buffer.length() > 500) buffer = ""; // seguridad
    }
  }
}

/* ---------- Setup / Loop ---------- */
void setup() {
  pinMode(RELAY_PIN, OUTPUT);
  relayOff();

  Serial.begin(115200);
  delay(200);

  allowCh12_13();                     // 1..13
  WiFi.setPhyMode(WIFI_PHY_MODE_11N); // N-only mejora compatibilidad
  WiFi.setSleepMode(WIFI_NONE_SLEEP);
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
  WiFi.setOutputPower(20.5);

  ehConn = WiFi.onStationModeConnected([](const WiFiEventStationModeConnected& i){
    Serial.printf("EVT:CONN ssid=%s ch=%d\n", i.ssid.c_str(), i.channel);
  });
  ehIP = WiFi.onStationModeGotIP([](const WiFiEventStationModeGotIP& i){
    Serial.printf("EVT:GOTIP ip=%s gw=%s mask=%s\n",
      i.ip.toString().c_str(), i.gw.toString().c_str(), i.mask.toString().c_str());
  });
  ehDisc = WiFi.onStationModeDisconnected([](const WiFiEventStationModeDisconnected& i){
    Serial.printf("EVT:DISC reason=%d\n", i.reason);
  });

  Serial.println("\n[BOOT] Cargando credenciales...");
  loadWifiCredentials();

  connectWithParams(wifi_ssid, wifi_pass);

  Serial.println("[READY] Comandos:");
  Serial.println("  CONFIG_8266:{\"ssid\":\"...\",\"pass\":\"...\"}");
  Serial.println("  STATUS? | CLEAR | DUMP | SCAN?");
}

void loop() {
  checkSerial();

  static unsigned long lastTry = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastTry > 10000) {
      connectWithParams(wifi_ssid, wifi_pass);
      lastTry = millis();
    }
  }
  delay(50);
}
