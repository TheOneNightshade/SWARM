/*
 * Swarm generic node for ESP32-C5 (one file)                                   GENERIC NODE BUILD 1
 *
 * Turns any ESP32-C5 board into a Swarm node: it links to the Swarm Android app over Bluetooth LE
 * (the direct GATT link), locks itself to the first Swarm app that takes it, and hands that app every
 * sighting your scanning code reports. Everything Swarm needs is done for you:
 *
 *   - advertising as WDU / WDN<node number> with the Swarm service, Command / Response / Status
 *   - the clock (TIME:SYNC), hello, heartbeat every 3 s, slot / plan / run / ack / ping
 *   - the owner lock: the first app to link becomes the owner; later links must prove its key
 *   - a queue with sequence numbers: sightings are kept until the phone acknowledges them, so nothing
 *     is lost while the phone is out of range, and the backlog uploads when it comes back
 *   - node number, plan, run state and owner kept in flash across reboots
 *
 * You only write the scanning. Look for the box marked "YOUR SCANNING CODE" below. It already holds a
 * working example (Wi-Fi scan over the phone's channel plan, plus BLE), so the sketch does something
 * useful as it is; replace it with your own.
 *
 * Build (Arduino IDE):
 *   - Board package: esp32 by Espressif, 3.3.x (tested to compile with 3.3.12)
 *   - Library: NimBLE-Arduino by h2zero, 2.x (tested to compile with 2.5.1)
 *   - Board: "XIAO_ESP32C5" (default partition scheme is fine), or "ESP32C5 Dev Module" with
 *     Tools > Partition Scheme set to "Minimal SPIFFS (1.9MB APP ...)" or "Huge APP" (Wi-Fi and
 *     Bluetooth together don't fit in the DevKit's default 1.2 MB app space).
 *   - Save this file in a folder with the same name (swarm_generic_c5_node/swarm_generic_c5_node.ino);
 *     the IDE offers to do that when you open it.
 *   - Serial monitor at 115200 shows every line sent and received.
 *
 * Use it:
 *   Power the board, open Swarm, tap FIND MY NODES. The board shows up on the SWARM tab, gets a node
 *   number and a plan, and locks to your phone. Tap START to scan.
 *
 * Free a board from its owner (it becomes new again, any Swarm app can then take it):
 *   - hold the BOOT button for 5 seconds while it runs, or
 *   - type "forget" in the serial monitor ("owner" prints the owner id, "status" the state).
 */

#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <esp_wifi.h>
#include <esp_random.h>
#include <sys/time.h>
#include <time.h>

// ======================================================================================================
// Settings
// ======================================================================================================
#define GENERIC_NODE_BUILD 1        // shown in the app as the node's firmware build; bump when you change it
#define BOOT_BUTTON           28       // BOOT button on the XIAO ESP32-C5 and the ESP32-C5 DevKit
#define CAN_DO_5GHZ        1        // the C5 scans 5 GHz too; the phone only hands out 5 GHz channels if set
#define QUEUE_SIZE         400      // sightings kept in RAM until the phone acknowledges them
#define SEEN_SLOTS         512      // recently queued devices (to avoid queueing the same one over and over)
#define AP_REPEAT_MS       300000   // queue the same access point again only after 5 minutes
#define BLE_REPEAT_MS      30000    // and the same BLE device after 30 seconds

// ======================================================================================================
// Swarm protocol constants (do not change)
// ======================================================================================================
#define SVC_UUID      "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CMD_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define RSP_UUID      "beb5483e-36e1-4688-b7f5-ea07361b26a9"
#define STATUS_UUID   "beb5483e-36e1-4688-b7f5-ea07361b26aa"
#define PROTO_VER     6
#define SW_NAME_MAX      24            // bytes of SSID / device name kept
#define PLAN_MAX      48            // channel entries in a plan
#define CMD_MAX       512           // longest command write accepted
#define BEAT_MS       3000
#define TIME_REQ_MS   5000
#define UNPROVEN_MS   15000         // an owned board drops a link that hasn't proved the key by then
#define STALL_MS      10000         // everything sent, ack not moving: start again from the oldest
#define SEQ_BLOCK     1000          // seqs reserved in flash at a time (never reused after a reboot)

#define BEAT_RUNNING  0x01
#define BEAT_5G       0x08
#define BEAT_SYNCED   0x20
#define BEAT_OWNED    0x40

#define REC_WIFI      1
#define REC_BLE       2

// ======================================================================================================
// Types. Arduino adds function prototypes above the first function in this file, so every struct or
// class that a function takes or returns must be defined up here, before any function. Put your own
// types in the space marked below.
// ======================================================================================================
struct Sighting {
  uint32_t seq;
  uint32_t ms;            // millis() when seen; turned into unix time when sent
  uint16_t session;
  uint8_t  type;          // REC_WIFI / REC_BLE
  uint8_t  mac[6];
  int8_t   rssi;
  uint8_t  channel;
  uint8_t  security;      // index into SECURITY_NAMES (Wi-Fi only)
  char     name[SW_NAME_MAX + 1];
};

struct Plan {
  uint8_t flags;          // 1 Wi-Fi, 2 BLE
  char    region;         // 'E' or 'U'
  uint8_t n;
  uint8_t ch[PLAN_MAX];
  uint8_t dwell[PLAN_MAX];  // 10 ms steps
};

struct CmdFrame {          // one write from the phone, passed from the Bluetooth task to loop()
  uint16_t len;
  char     data[CMD_MAX + 1];
};

struct Sha256 {
  uint32_t h[8];
  uint8_t  buf[64];
  uint32_t used;
  uint64_t total;
};

// ---- your own types go here ----


// ======================================================================================================
// What your scanning code can use (defined further down)
// ======================================================================================================
// Report a sighting. Safe to call from any task. Duplicates within the repeat times above are dropped.
//   security: one of "OPEN", "WEP", "WPA_PSK", "WPA2_PSK", "WPA_WPA2_PSK", "WPA2_ENTERPRISE",
//             "WPA3_PSK", "WPA2_WPA3_PSK", "UNKNOWN" (swarmAuthName() converts an ESP-IDF auth mode).
//   mac / bssid: 6 bytes in printed order (AA:BB:CC:DD:EE:FF is AA BB CC DD EE FF).
void swarmAddWifi(const char* ssid, const uint8_t* bssid, uint8_t channel, int rssi, const char* security);
void swarmAddBle(const char* name, const uint8_t* mac, int rssi);
const char* swarmAuthName(wifi_auth_mode_t a);

// What to scan right now (the phone's run state and plan, already combined):
bool swarmWifiWanted();             // scan Wi-Fi now
bool swarmBleWanted();              // scan BLE now
uint8_t swarmPlanCount();           // Wi-Fi channels in this node's plan (0 = no plan yet)
uint8_t swarmPlanChannel(uint8_t i);
uint16_t swarmPlanDwellMs(uint8_t i);
char swarmRegion();                 // 'E' (Europe) or 'U' (USA); the radio's country is set for you

// Radio sharing (the C5 has one radio for Wi-Fi and BLE):
bool swarmRadioQuiet();             // a phone is setting up its link: start no new scans of any kind
bool swarmBleTurn();                // the advert is off for the BLE scan's turn: scan BLE only while true
bool swarmLinked();                 // a phone is connected right now

void userScanSetup();               // called once from setup()
void userScanLoop();                // called over and over from loop()


// ######################################################################################################
// ##                                                                                                  ##
// ##   YOUR SCANNING CODE                                                                             ##
// ##                                                                                                  ##
// ##   Everything between this box and the "END OF YOUR SCANNING CODE" box is yours to replace.       ##
// ##   Rules:                                                                                         ##
// ##     - Scan only while swarmWifiWanted() / swarmBleWanted() say so, and start nothing new         ##
// ##       while swarmRadioQuiet() is true.                                                           ##
// ##     - BLE: scan only while swarmBleTurn() is true. The board can't advertise and scan BLE at     ##
// ##       the same time, so it takes turns, and the phone lets the board go now and then so it can   ##
// ##       scan BLE without a link.                                                                   ##
// ##     - Report each thing you find with swarmAddWifi() or swarmAddBle(). That's all: queueing,     ##
// ##       timestamps, upload and acks are handled for you.                                           ##
// ##     - Don't block for long in userScanLoop(): the link is served from loop() too.                ##
// ##                                                                                                  ##
// ######################################################################################################

// ---- Example: Wi-Fi. Hops through the plan's channels with the Wi-Fi driver's own scan. ----
static uint8_t exPlanIdx = 0;
static bool exWifiScanning = false;
static bool exWifiUp = false;

static void exWifiStep() {
  int16_t r = WiFi.scanComplete();
  if (exWifiScanning) {
    if (r == WIFI_SCAN_RUNNING) return;
    exWifiScanning = false;
    for (int i = 0; i < r; i++) {
      swarmAddWifi(WiFi.SSID(i).c_str(), WiFi.BSSID(i), (uint8_t)WiFi.channel(i), WiFi.RSSI(i),
                   swarmAuthName(WiFi.encryptionType(i)));
    }
    WiFi.scanDelete();
  }
  bool want = swarmWifiWanted() && !swarmBleTurn();
  if (!want) {
    if (exWifiUp && !swarmWifiWanted()) { WiFi.mode(WIFI_OFF); exWifiUp = false; }
    return;
  }
  if (swarmRadioQuiet()) return;
  if (!exWifiUp) {
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    exWifiUp = true;
  }
  uint8_t ch = 0;
  uint16_t dwell = 120;
  uint8_t n = swarmPlanCount();
  if (n) {                                   // the phone's plan: this node's share of the channels
    if (exPlanIdx >= n) exPlanIdx = 0;
    ch = swarmPlanChannel(exPlanIdx);
    dwell = swarmPlanDwellMs(exPlanIdx);
    exPlanIdx++;
  }                                          // no plan yet: ch 0 = every channel
  if (dwell < 50) dwell = 50;
  if (WiFi.scanNetworks(true, true, false, dwell, ch) == WIFI_SCAN_FAILED) return;
  exWifiScanning = true;
}

// ---- Example: BLE. NimBLE scan during the BLE turns. ----
class ExBleCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice* d) override {
    if (d->isAdvertisingService(NimBLEUUID(SVC_UUID))) return;   // another Swarm node
    const uint8_t* v = d->getAddress().getVal();                  // NimBLE stores it reversed
    uint8_t mac[6];
    for (int i = 0; i < 6; i++) mac[i] = v[5 - i];
    swarmAddBle(d->haveName() ? d->getName().c_str() : "", mac, d->getRSSI());
  }
};
static ExBleCB exBleCB;

static void exBleStep() {
  NimBLEScan* sc = NimBLEDevice::getScan();
  bool want = swarmBleWanted() && swarmBleTurn() && !swarmRadioQuiet();
  if (!want) {
    if (sc->isScanning()) sc->stop();
    return;
  }
  if (!sc->isScanning()) sc->start(0, false, true);   // 0 = until stopped
}

void userScanSetup() {
  NimBLEScan* sc = NimBLEDevice::getScan();
  sc->setScanCallbacks(&exBleCB, true);   // true = report every advert, our own dedupe decides
  sc->setActiveScan(true);                 // ask for scan responses (that's where many names are)
  sc->setInterval(100);
  sc->setWindow(90);
  sc->setMaxResults(0);                    // don't keep results in RAM, the callback has them
}

void userScanLoop() {
  exWifiStep();
  exBleStep();
}

// ######################################################################################################
// ##   END OF YOUR SCANNING CODE. Below is the Swarm link; you shouldn't need to change it.            ##
// ######################################################################################################


// ======================================================================================================
// State
// ======================================================================================================
static const char* SECURITY_NAMES[] = {"OPEN", "WEP", "WPA_PSK", "WPA2_PSK", "WPA_WPA2_PSK",
                                       "WPA2_ENTERPRISE", "WPA3_PSK", "WPA2_WPA3_PSK", "UNKNOWN"};
#define SEC_UNKNOWN 8

static Preferences prefs;
static NimBLEServer* server = nullptr;
static NimBLECharacteristic* chRsp = nullptr;
static NimBLECharacteristic* chStatus = nullptr;

// kept in flash
static uint8_t  mySlot = 0;
static uint8_t  runMask = 0;
static uint16_t runSession = 0;
static Plan     plan = {0, 'E', 0, {0}, {0}};
static uint16_t planHash = 0;
static bool     owned = false;
static uint8_t  ownerKey[16];
static uint32_t seqReserved = 0;     // first seq not yet reserved in flash

// queue (ring), guarded by qLock
static portMUX_TYPE qLock = portMUX_INITIALIZER_UNLOCKED;
static Sighting queue[QUEUE_SIZE];
static uint16_t qHead = 0, qCount = 0;
static uint32_t nextSeq = 0;         // seq the next sighting gets
static uint32_t lostCount = 0;
static uint32_t seenMac[SEEN_SLOTS]; // hash of type+mac
static uint32_t seenAt[SEEN_SLOTS];

// link (loop task unless noted)
static volatile bool linked = false;       // set by NimBLE callbacks
static volatile bool subscribed = false;
static volatile uint16_t connHandle = 0;
static volatile uint16_t mtu = 23;
static volatile uint32_t connectedAt = 0;
static volatile bool linkStarted = false;  // a new link the loop hasn't set up yet
static volatile bool linkEnded = false;
static bool     helloOk = false;           // hello answered OK on this link
static bool     proven = false;            // owner key proved on this link (or board has no owner)
static uint8_t  challenge[8];
static bool     challengeOut = false;
static uint32_t quietUntil = 0;            // radio quiet while the phone sets up the link
static uint32_t sendSeq = 0;               // next seq to upload
static uint32_t lastAckMove = 0;
static uint32_t lastBeat = 0, lastTimeReq = 0;
static String   outbox;                    // lines waiting to be notified

// clock
static bool     clockSet = false;
static uint32_t utcAtSync = 0, msAtSync = 0;

// advert / BLE turn
static bool     advOn = false;
static bool     advDirty = true;
static bool     bleTurn = false;
static uint32_t turnEnd = 0;
#define ADV_TURN_MS  1500
#define SCAN_TURN_MS 2500

// commands from the phone (NimBLE task -> loop)
static QueueHandle_t cmdQueue;
static CmdFrame rxFrame;                                     // NimBLE task only

static bool     bootBtnOk = false;
static uint32_t bootDownAt = 0;

// ======================================================================================================
// SHA-256 and HMAC (for the owner lock)
// ======================================================================================================
static const uint32_t SHA_K[64] = {
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

static inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

static void shaBlock(Sha256& s, const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = s.h[0], b = s.h[1], c = s.h[2], d = s.h[3], e = s.h[4], f = s.h[5], g = s.h[6], h = s.h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i];
    uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s.h[0] += a; s.h[1] += b; s.h[2] += c; s.h[3] += d; s.h[4] += e; s.h[5] += f; s.h[6] += g; s.h[7] += h;
}

static void shaInit(Sha256& s) {
  static const uint32_t H0[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  memcpy(s.h, H0, sizeof(H0));
  s.used = 0;
  s.total = 0;
}

static void shaUpdate(Sha256& s, const uint8_t* d, size_t n) {
  s.total += n;
  while (n--) {
    s.buf[s.used++] = *d++;
    if (s.used == 64) { shaBlock(s, s.buf); s.used = 0; }
  }
}

static void shaFinal(Sha256& s, uint8_t out[32]) {
  uint64_t bits = s.total * 8;
  uint8_t pad = 0x80;
  shaUpdate(s, &pad, 1);
  pad = 0;
  while (s.used != 56) shaUpdate(s, &pad, 1);
  for (int i = 7; i >= 0; i--) { uint8_t b = (uint8_t)(bits >> (8 * i)); shaUpdate(s, &b, 1); }
  for (int i = 0; i < 8; i++) {
    out[4 * i] = s.h[i] >> 24; out[4 * i + 1] = s.h[i] >> 16; out[4 * i + 2] = s.h[i] >> 8; out[4 * i + 3] = s.h[i];
  }
}

// HMAC-SHA256 with a 16-byte key over up to three pieces of message.
static void hmac16(const uint8_t* key, const uint8_t* a, size_t an, const uint8_t* b, size_t bn,
                   const uint8_t* c, size_t cn, uint8_t out[32]) {
  uint8_t pad[64];
  Sha256 s;
  memset(pad, 0x36, 64);
  for (int i = 0; i < 16; i++) pad[i] ^= key[i];
  shaInit(s);
  shaUpdate(s, pad, 64);
  if (an) shaUpdate(s, a, an);
  if (bn) shaUpdate(s, b, bn);
  if (cn) shaUpdate(s, c, cn);
  uint8_t inner[32];
  shaFinal(s, inner);
  memset(pad, 0x5c, 64);
  for (int i = 0; i < 16; i++) pad[i] ^= key[i];
  shaInit(s);
  shaUpdate(s, pad, 64);
  shaUpdate(s, inner, 32);
  shaFinal(s, out);
}

// owner id = HMAC(key, "swarm-owner")[0..2) as little-endian u16
static uint16_t ownerId(const uint8_t* key) {
  uint8_t h[32];
  hmac16(key, (const uint8_t*)"swarm-owner", 11, nullptr, 0, nullptr, 0, h);
  return (uint16_t)(h[0] | (h[1] << 8));
}

// hello tag = HMAC(key, "hello" | nonce[8] | board address[6], printed order)[0..8)
static void helloTag(const uint8_t* key, const uint8_t* nonce, const uint8_t* addr, uint8_t out[8]) {
  uint8_t h[32];
  hmac16(key, (const uint8_t*)"hello", 5, nonce, 8, addr, 6, h);
  memcpy(out, h, 8);
}

static bool sameBytes(const uint8_t* a, const uint8_t* b, size_t n) {
  uint8_t d = 0;
  for (size_t i = 0; i < n; i++) d |= a[i] ^ b[i];
  return d == 0;
}

static void toHex(const uint8_t* d, size_t n, char* out) {
  static const char H[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) { out[2 * i] = H[d[i] >> 4]; out[2 * i + 1] = H[d[i] & 15]; }
  out[2 * n] = 0;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static bool fromHex(const char* s, size_t slen, uint8_t* out, size_t n) {
  if (slen != 2 * n) return false;
  for (size_t i = 0; i < n; i++) {
    int a = hexVal(s[2 * i]), b = hexVal(s[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = (uint8_t)(a << 4 | b);
  }
  return true;
}

// The board's Bluetooth address in printed order.
static void myAddress(uint8_t out[6]) {
  const uint8_t* v = NimBLEDevice::getAddress().getVal();
  for (int i = 0; i < 6; i++) out[i] = v[5 - i];
}

// ======================================================================================================
// Plan
// ======================================================================================================
static uint16_t calcPlanHash(const Plan& p) {
  uint32_t h = 2166136261u;
#define MIX(b) h = (h ^ (uint8_t)(b)) * 16777619u
  MIX(p.flags & 3);
  MIX(p.n);
  for (uint8_t i = 0; i < p.n; i++) { MIX(p.ch[i]); MIX(p.dwell[i]); }
#undef MIX
  return (uint16_t)((h >> 16) ^ (h & 0xFFFF));
}

static bool regionHas(char rg, uint8_t ch) {
  if (ch >= 1 && ch <= 14) return ch <= (rg == 'U' ? 11 : 13);
  if (ch >= 36 && ch <= 64) return true;
  if (ch >= 100 && ch <= 140) return true;
  if (rg == 'U' && ch >= 141 && ch <= 144) return true;
  if (rg == 'U' && ch >= 149 && ch <= 177) return true;
  return false;
}

// Finds key=value in "a=1,b=2" (value ends at ',' or end). Returns length, -1 if missing.
static int argFind(const char* args, const char* key, const char** val) {
  size_t kl = strlen(key);
  const char* p = args;
  while (p && *p) {
    if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
      *val = p + kl + 1;
      const char* e = strchr(*val, ',');
      return e ? (int)(e - *val) : (int)strlen(*val);
    }
    p = strchr(p, ',');
    if (p) p++;
  }
  return -1;
}

static long argLong(const char* args, const char* key, long dflt) {
  const char* v;
  if (argFind(args, key, &v) < 0) return dflt;
  return strtol(v, nullptr, 10);
}

// "f=3,rg=U,ch=1.25/6.25/11.25" -> Plan. Channels the region doesn't have stay in the plan (they are part
// of the hash) but are skipped by swarmPlanChannel's callers via swarmPlanCount.
static bool parsePlan(const char* args, Plan& p) {
  Plan q = {0, 'E', 0, {0}, {0}};
  q.flags = (uint8_t)argLong(args, "f", 0);
  const char* v;
  if (argFind(args, "rg", &v) >= 0 && (*v == 'U' || *v == 'E')) q.region = *v;
  int len = argFind(args, "ch", &v);
  if (len > 0) {
    const char* end = v + len;
    while (v < end && q.n < PLAN_MAX) {
      char* e;
      long ch = strtol(v, &e, 10);
      if (e == v || *e != '.') return false;
      long dw = strtol(e + 1, &e, 10);
      if (ch < 1 || ch > 196 || dw < 1 || dw > 255) return false;
      q.ch[q.n] = (uint8_t)ch;
      q.dwell[q.n] = (uint8_t)dw;
      q.n++;
      v = e;
      if (v < end && *v == '/') v++;
    }
  }
  p = q;
  return true;
}

static void applyRegion() {
  esp_err_t e = esp_wifi_set_country_code(plan.region == 'U' ? "US" : "DE", false);
  if (e != ESP_OK && e != ESP_ERR_WIFI_NOT_INIT) Serial.printf("wifi country: %s\n", esp_err_to_name(e));
}

// ======================================================================================================
// What the scanning code sees
// ======================================================================================================
bool swarmWifiWanted() {
  uint8_t f = plan.n || plan.flags ? plan.flags : 3;    // no plan yet: whatever the run mask says
  return (runMask & 1) && (f & 1);
}
bool swarmBleWanted() {
  uint8_t f = plan.n || plan.flags ? plan.flags : 3;
  return (runMask & 2) && (f & 2);
}
uint8_t swarmPlanCount() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < plan.n; i++) if (regionHas(plan.region, plan.ch[i])) n++;
  return n;
}
uint8_t swarmPlanChannel(uint8_t i) {        // i-th channel the region allows
  for (uint8_t k = 0; k < plan.n; k++) {
    if (!regionHas(plan.region, plan.ch[k])) continue;
    if (i-- == 0) return plan.ch[k];
  }
  return 0;
}
uint16_t swarmPlanDwellMs(uint8_t i) {
  for (uint8_t k = 0; k < plan.n; k++) {
    if (!regionHas(plan.region, plan.ch[k])) continue;
    if (i-- == 0) return plan.dwell[k] * 10;
  }
  return 0;
}
char swarmRegion() { return plan.region; }
bool swarmLinked() { return linked; }
bool swarmBleTurn() { return bleTurn; }
bool swarmRadioQuiet() { return quietUntil && (int32_t)(millis() - quietUntil) < 0; }

const char* swarmAuthName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "OPEN";
    case WIFI_AUTH_WEP: return "WEP";
    case WIFI_AUTH_WPA_PSK: return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK: return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK: return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK: return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2_WPA3_PSK";
    default: return "UNKNOWN";
  }
}

// ======================================================================================================
// Queue
// ======================================================================================================
static uint32_t macHash(uint8_t type, const uint8_t* mac) {
  uint32_t h = 2166136261u ^ type;
  for (int i = 0; i < 6; i++) h = (h ^ mac[i]) * 16777619u;
  return h ? h : 1;
}

static void cleanName(const char* in, char* out) {
  size_t n = 0;
  if (in) for (; in[n] && n < SW_NAME_MAX; n++) {
    char c = in[n];
    out[n] = (c == ',' || c == '\r' || c == '\n') ? '_' : c;
  }
  out[n] = 0;
}

static void saveSeqReservation() {
  prefs.putUInt("seq", seqReserved);
}

// Queues one sighting (any task). Called by swarmAddWifi / swarmAddBle.
static void queueSighting(uint8_t type, const uint8_t* mac, int rssi, uint8_t channel, uint8_t sec, const char* name) {
  uint32_t now = millis();
  uint32_t h = macHash(type, mac);
  uint32_t repeat = type == REC_WIFI ? AP_REPEAT_MS : BLE_REPEAT_MS;
  bool needReserve = false;
  portENTER_CRITICAL(&qLock);
  uint16_t slot = h % SEEN_SLOTS;
  if (seenMac[slot] == h && now - seenAt[slot] < repeat) { portEXIT_CRITICAL(&qLock); return; }
  seenMac[slot] = h;
  seenAt[slot] = now;
  if (qCount == QUEUE_SIZE) {                 // full: drop the oldest
    qHead = (qHead + 1) % QUEUE_SIZE;
    qCount--;
    lostCount++;
  }
  Sighting& s = queue[(qHead + qCount) % QUEUE_SIZE];
  s.seq = nextSeq++;
  s.ms = now;
  s.session = runSession;
  s.type = type;
  memcpy(s.mac, mac, 6);
  s.rssi = (int8_t)constrain(rssi, -127, 0);
  s.channel = channel;
  s.security = sec;
  cleanName(name, s.name);
  qCount++;
  needReserve = nextSeq >= seqReserved;
  if (needReserve) seqReserved = nextSeq + SEQ_BLOCK;
  portEXIT_CRITICAL(&qLock);
  if (needReserve) saveSeqReservation();      // flash write outside the critical section
}

void swarmAddWifi(const char* ssid, const uint8_t* bssid, uint8_t channel, int rssi, const char* security) {
  if (!bssid || !runMask) return;
  uint8_t sec = SEC_UNKNOWN;
  if (security) for (uint8_t i = 0; i < 9; i++) if (strcmp(security, SECURITY_NAMES[i]) == 0) sec = i;
  queueSighting(REC_WIFI, bssid, rssi, channel, sec, ssid);
}

void swarmAddBle(const char* name, const uint8_t* mac, int rssi) {
  if (!mac || !runMask) return;
  queueSighting(REC_BLE, mac, rssi, 0, SEC_UNKNOWN, name);
}

// Deletes everything before seq.
static void applyAck(uint32_t seq) {
  portENTER_CRITICAL(&qLock);
  bool moved = false;
  while (qCount && (int32_t)(queue[qHead].seq - seq) < 0) {
    qHead = (qHead + 1) % QUEUE_SIZE;
    qCount--;
    moved = true;
  }
  portEXIT_CRITICAL(&qLock);
  if (moved) lastAckMove = millis();
  if ((int32_t)(sendSeq - seq) < 0) sendSeq = seq;
}

static uint32_t oldestSeq() {
  portENTER_CRITICAL(&qLock);
  uint32_t s = qCount ? queue[qHead].seq : nextSeq;
  portEXIT_CRITICAL(&qLock);
  return s;
}

// Copies the first queued sighting with seq >= from. False if none.
static bool peekFrom(uint32_t from, Sighting& out) {
  bool ok = false;
  portENTER_CRITICAL(&qLock);
  if (qCount) {
    uint32_t first = queue[qHead].seq;
    uint32_t off = (int32_t)(from - first) <= 0 ? 0 : from - first;   // seqs in the queue are consecutive
    if (off < qCount) {
      out = queue[(qHead + off) % QUEUE_SIZE];
      ok = true;
    }
  }
  portEXIT_CRITICAL(&qLock);
  return ok;
}

// ======================================================================================================
// Clock
// ======================================================================================================
static uint32_t utcOf(uint32_t ms) {
  if (!clockSet) return 0;
  return utcAtSync - (uint32_t)(((int32_t)(msAtSync - ms)) / 1000);
}

// TIME:SYNC:2026-10-15T02:20:05Z
static void setClock(const char* iso) {
  struct tm t = {};
  if (sscanf(iso, "%d-%d-%dT%d:%d:%d", &t.tm_year, &t.tm_mon, &t.tm_mday, &t.tm_hour, &t.tm_min, &t.tm_sec) != 6) return;
  t.tm_year -= 1900;
  t.tm_mon -= 1;
  // days from civil (UTC, no time zone involved)
  int y = t.tm_year + 1900, m = t.tm_mon + 1, d = t.tm_mday;
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  unsigned yoe = (unsigned)(y - era * 400);
  unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  long days = (long)era * 146097 + (long)doe - 719468;
  uint32_t utc = (uint32_t)(days * 86400L + t.tm_hour * 3600L + t.tm_min * 60L + t.tm_sec);
  utcAtSync = utc;
  msAtSync = millis();
  if (!clockSet) Serial.printf("clock set: %s\n", iso);
  clockSet = true;
}

// ======================================================================================================
// Sending
// ======================================================================================================
static void sendLine(const char* line) {
  Serial.printf("> %s\n", line);
  outbox += line;
  outbox += '\n';
}

// Notifies as much of the outbox as the link takes. Lines may span notifies; the phone joins them.
static void flushOut() {
  if (!linked || !subscribed) { outbox = ""; return; }
  size_t chunk = mtu > 3 ? mtu - 3 : 20;
  if (chunk > 244) chunk = 244;
  for (int i = 0; i < 8 && outbox.length(); i++) {
    size_t n = outbox.length() < chunk ? outbox.length() : chunk;
    if (!chRsp->notify((const uint8_t*)outbox.c_str(), n, connHandle)) return;   // try again next loop
    outbox.remove(0, n);
  }
}

static void sendBeat() {
  uint8_t f = 0;
  if (runMask) f |= BEAT_RUNNING;
  if (CAN_DO_5GHZ) f |= BEAT_5G;
  if (clockSet) f |= BEAT_SYNCED;
  if (owned) f |= BEAT_OWNED;
  uint32_t backlog;
  portENTER_CRITICAL(&qLock);
  backlog = qCount;
  portEXIT_CRITICAL(&qLock);
  char line[200];
  snprintf(line, sizeof(line),
           "DATA:BEAT:f=%u,h=%u,m=%u,s=%u,n=%u,b=%lu,l=%lu,q=%lu,t=%lu,v=%u,rg=%c,bv=0,tc=%d,up=%lu,hp=%u",
           f, planHash, runMask, runSession, mySlot, (unsigned long)backlog,
           (unsigned long)(lostCount > 65535 ? 65535 : lostCount), (unsigned long)nextSeq,
           (unsigned long)oldestSeq(), GENERIC_NODE_BUILD, plan.region, (int)temperatureRead(),
           (unsigned long)(millis() / 60000), (unsigned)(ESP.getFreeHeap() / 1024));
  sendLine(line);
  lastBeat = millis();
}

static void formatSighting(const Sighting& s, char* line, size_t cap) {
  char mac[18];
  snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", s.mac[0], s.mac[1], s.mac[2], s.mac[3], s.mac[4], s.mac[5]);
  if (s.type == REC_WIFI) {
    snprintf(line, cap, "DATA:AP:%s,%s,%u,%d,[%s],q=%lu,t=%lu,s=%u", s.name, mac, s.channel, s.rssi,
             SECURITY_NAMES[s.security < 9 ? s.security : SEC_UNKNOWN], (unsigned long)s.seq,
             (unsigned long)utcOf(s.ms), s.session);
  } else {
    snprintf(line, cap, "DATA:BT:%s,%s,%d,BLE,q=%lu,t=%lu,s=%u", s.name, mac, s.rssi, (unsigned long)s.seq,
             (unsigned long)utcOf(s.ms), s.session);
  }
}

// Uploads queued sightings, oldest unacked first. Only while the clock is set (the phone places each
// sighting by its time).
static void uploadTick() {
  if (!helloOk || !proven || !clockSet || !subscribed) return;
  uint32_t now = millis();
  for (int i = 0; i < 6 && outbox.length() < 600; i++) {
    Sighting s;
    if (!peekFrom(sendSeq, s)) break;
    char line[160];
    formatSighting(s, line, sizeof(line));
    outbox += line;            // no serial echo: too many
    outbox += '\n';
    sendSeq = s.seq + 1;
  }
  // everything sent and the ack hasn't moved for a while: start again from the oldest
  uint32_t oldest = oldestSeq();
  if (oldest != nextSeq && sendSeq == nextSeq && now - lastAckMove > STALL_MS) {
    sendSeq = oldest;
    lastAckMove = now;
  }
}

// ======================================================================================================
// Flash
// ======================================================================================================
static void loadState() {
  prefs.begin("swarmgen", false);
  mySlot = prefs.getUChar("slot", 0);
  runMask = prefs.getUChar("run", 0);
  runSession = prefs.getUShort("sess", 0);
  String p = prefs.getString("plan", "");
  if (p.length()) parsePlan(p.c_str(), plan);
  planHash = plan.n || plan.flags ? calcPlanHash(plan) : 0;
  owned = prefs.isKey("okey") && prefs.getBytes("okey", ownerKey, 16) == 16;
  seqReserved = prefs.getUInt("seq", 0);
  nextSeq = seqReserved;                     // skip whatever was reserved before the reboot
  seqReserved = nextSeq + SEQ_BLOCK;
  saveSeqReservation();
}

static void forgetOwner() {
  owned = false;
  memset(ownerKey, 0, 16);
  prefs.remove("okey");
  Serial.println("owner forgotten: any Swarm app can take this board now");
  if (linked && server) server->disconnect(connHandle);
}

// ======================================================================================================
// Commands
// ======================================================================================================
static void rsp(const char* name, const char* result) {
  char line[96];
  snprintf(line, sizeof(line), "RSP:%s:%s", name, result);
  sendLine(line);
}

static void handleHello(const char* args) {
  if (!owned) {
    helloOk = true;
    proven = true;
  } else {
    const char* a;
    int al = argFind(args, "a", &a);
    uint8_t tag[8], want[8], addr[6];
    if (al >= 0 && challengeOut && fromHex(a, al, tag, 8)) {
      myAddress(addr);
      helloTag(ownerKey, challenge, addr, want);
      if (sameBytes(tag, want, 8)) {
        helloOk = true;
        proven = true;
        challengeOut = false;
      }
    }
    if (!proven) {                           // new challenge (also after a wrong tag)
      esp_fill_random(challenge, 8);
      challengeOut = true;
      char c[17], line[80];
      toHex(challenge, 8, c);
      snprintf(line, sizeof(line), "RSP:hello:ERROR:owned,o=%04x,c=%s", ownerId(ownerKey), c);
      sendLine(line);
      return;
    }
  }
  rsp("hello", "OK");
  sendSeq = oldestSeq();                     // upload restarts from the oldest unacked
  lastAckMove = millis();
  quietUntil = millis() + 2000;              // let the link settle a little longer
  sendBeat();
}

static void handleLine(char* line) {
  Serial.printf("< %s\n", line);
  if (strncmp(line, "TIME:SYNC:", 10) == 0) {
    if (proven || !owned) setClock(line + 10);
    return;
  }
  if (strncmp(line, "CMD:", 4) != 0) return;
  char* name = line + 4;
  char* args = strchr(name, ':');
  if (args) *args++ = 0; else args = name + strlen(name);

  if (strcmp(name, "hello") == 0) { handleHello(args); return; }
  if (strcmp(name, "ping") == 0) { rsp("ping", "OK"); if (helloOk && proven) sendBeat(); return; }
  if (strcmp(name, "ack") == 0) { if (proven) applyAck((uint32_t)strtoul(args + (strncmp(args, "n=", 2) ? 0 : 2), nullptr, 10)); return; }
  if (owned && !proven) { rsp(name, "ERROR:owned"); return; }

  if (strcmp(name, "own") == 0) {
    const char* k;
    int kl = argFind(args, "k", &k);
    uint8_t key[16];
    if (owned || kl < 0 || !fromHex(k, kl, key, 16)) { rsp("own", "ERROR"); return; }
    memcpy(ownerKey, key, 16);
    prefs.putBytes("okey", ownerKey, 16);
    owned = true;
    proven = true;
    Serial.printf("locked to owner %04x\n", ownerId(ownerKey));
    rsp("own", "OK");
  } else if (strcmp(name, "disown") == 0) {
    rsp("disown", "OK");
    flushOut();
    owned = false;
    memset(ownerKey, 0, 16);
    prefs.remove("okey");
    Serial.println("owner released by the app");
  } else if (strcmp(name, "slot") == 0) {
    mySlot = (uint8_t)argLong(args, "n", 0);
    prefs.putUChar("slot", mySlot);
    advDirty = true;
    rsp("slot", "OK");
  } else if (strcmp(name, "plan") == 0) {
    Plan p;
    if (!parsePlan(args, p)) { rsp("plan", "ERROR:bad plan"); return; }
    plan = p;
    planHash = calcPlanHash(plan);
    prefs.putString("plan", args);
    applyRegion();
    rsp("plan", "OK");
  } else if (strcmp(name, "run") == 0) {
    runMask = (uint8_t)argLong(args, "m", 0) & 3;
    runSession = (uint16_t)argLong(args, "s", runSession);
    prefs.putUChar("run", runMask);
    prefs.putUShort("sess", runSession);
    rsp("run", "OK");
    sendLine(runMask ? "STATUS:2:Busy" : "STATUS:1:Ready");
  } else {
    rsp(name, "ERROR:unknown command");
    return;
  }
  if (helloOk) sendBeat();
}

// Splits a write into lines and handles each.
static void handleWrite(char* data, size_t len) {
  data[len] = 0;
  char* p = data;
  while (*p) {
    char* e = strchr(p, '\n');
    if (e) *e = 0;
    size_t n = strlen(p);
    if (n && p[n - 1] == '\r') p[--n] = 0;
    if (n) handleLine(p);
    if (!e) break;
    p = e + 1;
  }
}

// ======================================================================================================
// Bluetooth
// ======================================================================================================
static void updateStatusChar() {
  char js[64];
  snprintf(js, sizeof(js), "{\"status\":%d,\"battery\":-1,\"build\":%d}", runMask ? 2 : 1, GENERIC_NODE_BUILD);
  chStatus->setValue((const uint8_t*)js, strlen(js));
}

// Rebuilds the whole advert each time (adding a name to the old one can make it too long).
static void startAdvert() {
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  adv->stop();
  char nm[12];
  if (mySlot) snprintf(nm, sizeof(nm), "WDN%u", (unsigned)mySlot);
  else strcpy(nm, "WDU");
  NimBLEAdvertisementData d;
  d.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  d.addServiceUUID(NimBLEUUID(SVC_UUID));
  d.setName(nm);
  adv->setAdvertisementData(d);
  advOn = adv->start();
  if (!advOn) Serial.println("advert failed to start, retrying");
  advDirty = false;
}

// Not linked: advertise. Wanting BLE scans: take turns, advert then the scan's turn with the advert off.
static void advTick() {
  uint32_t now = millis();
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  if (linked) {
    bleTurn = false;
    advOn = false;
    return;
  }
  if (swarmBleWanted()) {
    if ((int32_t)(now - turnEnd) >= 0) {
      bleTurn = !bleTurn;
      turnEnd = now + (bleTurn ? SCAN_TURN_MS : ADV_TURN_MS);
      if (bleTurn) { adv->stop(); advOn = false; return; }
      NimBLEScan* sc = NimBLEDevice::getScan();
      if (sc->isScanning()) sc->stop();
      startAdvert();
      return;
    }
    if (bleTurn) return;
  } else if (bleTurn) {
    bleTurn = false;
    NimBLEScan* sc = NimBLEDevice::getScan();
    if (sc->isScanning()) sc->stop();
  }
  static uint32_t check = 0;
  if (advDirty || !advOn || ((int32_t)(now - check) >= 0 && !adv->isAdvertising())) {
    startAdvert();
    check = now + 1000;
  }
}

class CmdCB : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
    NimBLEAttValue v = c->getValue();
    size_t n = v.length() > CMD_MAX ? CMD_MAX : v.length();
    memcpy(rxFrame.data, v.data(), n);
    rxFrame.len = (uint16_t)n;
    xQueueSend(cmdQueue, &rxFrame, 0);
  }
};

class RspCB : public NimBLECharacteristicCallbacks {
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& info, uint16_t subValue) override {
    subscribed = (subValue & 1) != 0;
  }
};

class SrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& info) override {
    connHandle = info.getConnHandle();
    mtu = 23;
    connectedAt = millis();
    linked = true;
    linkStarted = true;
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& info, int reason) override {
    linked = false;
    subscribed = false;
    linkEnded = true;
  }
  void onMTUChange(uint16_t m, NimBLEConnInfo& info) override {
    mtu = m;
  }
};

static CmdCB cmdCB;
static RspCB rspCB;
static SrvCB srvCB;

static void bleSetup() {
  NimBLEDevice::init("WDU");
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setPower(9);
  server = NimBLEDevice::createServer();
  server->setCallbacks(&srvCB);
  server->advertiseOnDisconnect(false);      // advTick decides
  NimBLEService* svc = server->createService(SVC_UUID);
  NimBLECharacteristic* chCmd = svc->createCharacteristic(CMD_UUID, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, CMD_MAX);
  chCmd->setCallbacks(&cmdCB);
  chRsp = svc->createCharacteristic(RSP_UUID, NIMBLE_PROPERTY::NOTIFY);
  chRsp->setCallbacks(&rspCB);
  chStatus = svc->createCharacteristic(STATUS_UUID, NIMBLE_PROPERTY::READ);
  updateStatusChar();
  server->start();
}

// Link housekeeping, once per loop.
static void linkTick() {
  uint32_t now = millis();
  if (linkEnded) {
    linkEnded = false;
    helloOk = false;
    proven = false;
    challengeOut = false;
    quietUntil = 0;
    outbox = "";
    advDirty = true;
    Serial.println("phone disconnected");
  }
  if (linkStarted) {
    linkStarted = false;
    helloOk = false;
    proven = !owned;
    challengeOut = false;
    outbox = "";
    quietUntil = now + UNPROVEN_MS;          // until hello (+2 s), the phone's link setup gets the radio
    NimBLEScan* sc = NimBLEDevice::getScan();
    if (sc->isScanning()) sc->stop();
    if (WiFi.scanComplete() == WIFI_SCAN_RUNNING) WiFi.scanDelete();
    Serial.println("phone connected");
  }
  if (!linked) return;

  CmdFrame f;
  while (xQueueReceive(cmdQueue, &f, 0) == pdTRUE) handleWrite(f.data, f.len);

  if (owned && !proven && now - connectedAt > UNPROVEN_MS) {
    Serial.println("link never proved the owner key: dropping it");
    server->disconnect(connHandle);
    return;
  }
  if (helloOk && proven) {
    if (!clockSet && now - lastTimeReq > TIME_REQ_MS) { sendLine("TIME:REQUEST:"); lastTimeReq = now; }
    if (now - lastBeat > BEAT_MS) sendBeat();
    uploadTick();
  }
  flushOut();
}

// ======================================================================================================
// Serial console and BOOT button
// ======================================================================================================
static void consoleTick() {
  static char buf[32];
  static uint8_t n = 0;
  while (Serial.available()) {
    char c = Serial.read();
    if (c != '\n' && c != '\r') { if (n < sizeof(buf) - 1) buf[n++] = c; continue; }
    buf[n] = 0;
    n = 0;
    if (strcmp(buf, "forget") == 0) forgetOwner();
    else if (strcmp(buf, "owner") == 0) {
      if (owned) Serial.printf("owner id %04x\n", ownerId(ownerKey)); else Serial.println("no owner");
    } else if (strcmp(buf, "status") == 0) {
      Serial.printf("build %d, node %u, run %u (session %u), plan %u channels (hash %u, region %c), queued %u, "
                    "next seq %lu, lost %lu, clock %s, linked %d, owner %s\n",
                    GENERIC_NODE_BUILD, mySlot, runMask, runSession, plan.n, planHash, plan.region, qCount,
                    (unsigned long)nextSeq, (unsigned long)lostCount, clockSet ? "set" : "not set", (int)linked,
                    owned ? "yes" : "none");
    } else if (buf[0]) Serial.println("commands: status, owner, forget");
  }
}

static void bootButtonTick() {
  if (!bootBtnOk) return;
  if (digitalRead(BOOT_BUTTON) == LOW) {
    if (!bootDownAt) bootDownAt = millis() | 1;
    else if (millis() - bootDownAt > 5000) {
      if (owned) forgetOwner();
      bootDownAt = 0;
      bootBtnOk = false;                      // once per boot is enough; release and reboot to use again
    }
  } else {
    bootDownAt = 0;
  }
}

// ======================================================================================================
// setup / loop
// ======================================================================================================
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nSwarm generic node build %d\n", GENERIC_NODE_BUILD);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);
  bootBtnOk = digitalRead(BOOT_BUTTON) == HIGH;  // armed only if not held at power-up
  cmdQueue = xQueueCreate(6, sizeof(CmdFrame));
  loadState();
  WiFi.mode(WIFI_STA);                        // start the driver once so the country and band can be set
  WiFi.setBandMode(WIFI_BAND_MODE_AUTO);      // 2.4 + 5 GHz
  applyRegion();
  WiFi.mode(WIFI_OFF);
  bleSetup();
  uint8_t addr[6];
  myAddress(addr);
  Serial.printf("address %02X:%02X:%02X:%02X:%02X:%02X, node %u, %s, run %u\n", addr[0], addr[1], addr[2], addr[3],
                addr[4], addr[5], mySlot, owned ? "locked to an owner" : "no owner yet", runMask);
  userScanSetup();
}

void loop() {
  linkTick();
  advTick();
  userScanLoop();
  consoleTick();
  bootButtonTick();
  static uint8_t lastRun = 0xFF;
  if (lastRun != runMask) { lastRun = runMask; updateStatusChar(); }
  delay(5);
}
