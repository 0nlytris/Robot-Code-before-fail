#include <WiFi.h>
#include <ESP_I2S.h>

const char *WIFI_SSID = "Altibox222286";
const char *WIFI_PASS = "2GruDwUw";
const uint16_t HTTP_PORT = 85;

const int SD   = 44;  // D7
const int BCLK = 7;   // D8
const int LRC  = 8;   // D9
const int DIN  = 9;   // D10


const uint32_t DEFAULT_SAMPLE_RATE = 24000;
const uint32_t UPLOAD_STOPPED = 5000; 

const size_t BUFFER_BYTES = 4194304;  // ca. 87 s ved 24 kHz
const size_t CHUNK_FRAMES = 256; // hvor mye lydbiter som sender av gangen


WiFiServer server(HTTP_PORT);
I2SClass i2s;

int16_t *audioBuffer = nullptr;

volatile bool playing = false;
size_t clipBytes = 0;
uint32_t clipRate = DEFAULT_SAMPLE_RATE;

uint32_t i2sRate = 0;  // 0 = ikke startet
TaskHandle_t playTaskHandle = nullptr;


void setup() {
  pinMode(SD, OUTPUT);
  digitalWrite(SD, LOW);  

  Serial.begin(115200);
  delay(1000);

  audioBuffer = (int16_t *)ps_malloc(BUFFER_BYTES);
  if (!audioBuffer) {
    Serial.println("FEIL: fant ikke PSRAM. Slå på Tools -> PSRAM -> OPI PSRAM.");
    while (true) delay(1000);
  }
  if (!startI2S(DEFAULT_SAMPLE_RATE)) {
    Serial.println("FEIL: I2S startet ikke");
  }

  xTaskCreatePinnedToCore(playTask, "lyd", 4096, nullptr, 3, &playTaskHandle, 1);

  connectWiFi();
  server.begin();
}


void loop() {
  WiFiClient client = server.accept();
  if (client) {
    handleClient(client);
  }

  // Prøver å koble til på nytt hvis Wi-Fi faller ut
  static unsigned long lastRetry = 0;
  if (WiFi.status() != WL_CONNECTED && millis() - lastRetry > 10000) {
    lastRetry = millis();
    WiFi.reconnect();
  }

  delay(1);
}


// ============================================================
// LYD

bool startI2S(uint32_t rate) {
  if (i2sRate == rate) return true;
  if (i2sRate != 0) i2s.end();

  i2s.setPins(BCLK, LRC, DIN);


  if (!i2s.begin(I2S_MODE_STD, rate, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    i2sRate = 0;
    return false;
  }

  i2sRate = rate;
  return true;
}


// fungerer som en seperat "loop" så loop og denne skjer uavhengig
void playTask(void *param) {
  (void)param;
  static int16_t stereo[2 * CHUNK_FRAMES];
  static int16_t zeros[2 * CHUNK_FRAMES];  

  for (;;) { 
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // venter på neste lydklipp. sover fram t det

    if (startI2S(clipRate)) {
      digitalWrite(SD, HIGH); //høytaler skrus på
      delay(10);  

      size_t frames = clipBytes / 2;
      size_t pos = 0;

      // så lenge posisjonen ikke har oversteget antall frames i pakken kjører den
      //sender pakker til i2sen
      while (pos < frames) { 
        size_t n = frames - pos;
        if (n > CHUNK_FRAMES) n = CHUNK_FRAMES;

        for (size_t i = 0; i < n; i++) {
          stereo[2 * i] = audioBuffer[pos + i];
          stereo[2 * i + 1] = audioBuffer[pos + i];
        }

        i2s.write((uint8_t *)stereo, n * 4);
        pos += n;
      }

      // 100 ms stillhet, så alt er spilt ut før forsterkeren slås av
      size_t silence = i2sRate / 10;
      while (silence > 0) {
        size_t n = silence < CHUNK_FRAMES ? silence : CHUNK_FRAMES;
        i2s.write((uint8_t *)zeros, n * 4);
        silence -= n;
      }

      digitalWrite(SD, LOW);
    }

    playing = false;
  }
}


void startClip(size_t bytes, uint32_t rate) {
  clipBytes = bytes & ~(size_t)1; //for å gjøre samplene til et heltall
  clipRate = rate;
  playing = true;
  xTaskNotifyGive(playTaskHandle); //starter lydavspillingen ovenfor
}


// ============================================================
// HTTP

void handleClient(WiFiClient &client) {
  unsigned long start = millis();
  while (client.connected() && !client.available() && millis() - start < 2000) {
    delay(1);
  }

  if (!client.available()) {
    client.stop();
    return;
  }

  String requestLine = client.readStringUntil('\n');
  int firstSpace = requestLine.indexOf(' ');
  int secondSpace = requestLine.indexOf(' ', firstSpace + 1);

  if (firstSpace == -1 || secondSpace == -1) {
    sendReply(client, 400, "Invalid HTTP request");
    client.stop();
    return;
  }

  String method = requestLine.substring(0, firstSpace);
  String path = requestLine.substring(firstSpace + 1, secondSpace);

  // Headere
  long contentLength = -1;
  long sampleRate = DEFAULT_SAMPLE_RATE;

  for (int headerCount = 0; headerCount < 40; ++headerCount) {
    String line = client.readStringUntil('\n');
    line.trim();
    if (line.length() == 0) break;

    String lower = line;
    lower.toLowerCase();

    if (lower.startsWith("content-length:")) {
      contentLength = line.substring(15).toInt();
    } else if (lower.startsWith("x-sample-rate:")) {
      sampleRate = line.substring(14).toInt();
    }
  }

  if (method == "POST" && path.startsWith("/audio")) {
    handleAudio(client, contentLength, sampleRate);
  } else if (path.startsWith("/status")) {
    sendReply(client, 200, playing ? "PLAYING" : "IDLE");
  } else {
    if (contentLength > 0) drainBody(client, contentLength);
    sendReply(client, 404, "Unknown command");
  }

  client.stop();
}


void handleAudio(WiFiClient &client, long contentLength, long sampleRate) {

  // Hvis lydfilen mottat er for stor. pc mottar hvor stor lydfilen var
  if ((size_t)contentLength > BUFFER_BYTES) {
    drainBody(client, contentLength);
    sendReply(client, 413, "Too long, max " + String((unsigned)BUFFER_BYTES) + " bytes");
    return;
  }

  if (!receiveBody(client, (size_t)contentLength)) {
    sendReply(client, 408, "Audio upload incomplete");
    return;
  }

  uint32_t ms = (uint32_t)((uint64_t)(contentLength / 2) * 1000 / sampleRate);
  startClip((size_t)contentLength, (uint32_t)sampleRate);
  sendReply(client, 200, "PLAYING " + String((unsigned)ms));
}


// Leser hele bodyen inn i lydbufferen.
bool receiveBody(WiFiClient &client, size_t length) {
  uint8_t *dst = (uint8_t *)audioBuffer;
  size_t received = 0;
  unsigned long lastData = millis();

  while (received < length) {
    int avail = client.available();

    if (avail > 0) {
      size_t want = length - received;
      if ((size_t)avail < want) want = (size_t)avail;

      int n = client.read(dst + received, want);
      if (n > 0) {
        received += (size_t)n;
        lastData = millis();
      }
    } else {
      if (!client.connected()) break;
      if (millis() - lastData > UPLOAD_STOPPED) break;
      delay(1);
    }
  }

  return received == length;
}


// Leser og kaster en body vi ikke vil ha, så svaret kommer fram.
void drainBody(WiFiClient &client, long length) {
  uint8_t scratch[256];
  unsigned long lastData = millis();

  while (length > 0 && millis() - lastData < 2000) {
    int avail = client.available();

    if (avail > 0) {
      int want = avail < (int)sizeof(scratch) ? avail : (int)sizeof(scratch);
      int n = client.read(scratch, want);
      if (n > 0) {
        length -= n;
        lastData = millis();
      }
    } else if (!client.connected()) {
      break;
    } else {
      delay(1);
    }
  }
}


void sendReply(WiFiClient &client, int statusCode, const String &body) {
  client.print("HTTP/1.1 ");
  client.print(statusCode);
  client.println(statusCode == 200 ? " OK" : " Error");
  client.println("Content-Type: text/plain");
  client.print("Content-Length: ");
  client.println(body.length());
  client.println("Connection: close");
  client.println();
  client.print(body);
}


// ============================================================
// WI-FI

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // ingen modem sleep: jevnere overføring
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);

    if (millis() - start > 20000) {  // prøver på nytt hvert 20. sekund
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      start = millis();
    }
  }

  Serial.print("Lyd-URL: http://");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  Serial.println(HTTP_PORT);
}
