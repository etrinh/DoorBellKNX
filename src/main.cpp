/*
    DoorBell KNX
*/

#define FW_TAG       "DoorBell KNX"
#define FW_VERSION   "1.00"

#define ENABLE_UPDATE
#define ENABLE_MP3
#define ENABLE_FLAC
#define ENABLE_WAV
#define ENABLE_MIDI
#define ENABLE_MOD
#define ENABLE_AAC

#include <Arduino.h>
#include <knx.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <AudioFileSourceSPIFFS.h>
#ifdef ENABLE_MP3
  #ifdef ENABLE_AAC
    #include <AudioGeneratorMP3a.h>
    typedef AudioGeneratorMP3a AudioGeneratorMP3;
  #else
    #include <AudioGeneratorMP3.h>
  #endif
#endif
#ifdef ENABLE_FLAC
  #include <AudioGeneratorFLAC.h>
#endif
#ifdef ENABLE_AAC
  #include <AudioGeneratorAAC.h>
#endif
#ifdef ENABLE_WAV
  #include <AudioGeneratorWAV.h>
#endif
#ifdef ENABLE_MOD
  #include <AudioGeneratorMOD.h>
#endif
#ifdef ENABLE_MIDI
  #include <AudioGeneratorMIDI.h>
#endif
#include <AudioOutputI2S.h>
#ifdef ENABLE_UPDATE
  #include <Update.h>
#endif
#include <WiFiManager.h>
#include "Arduino.h"
#include <WiFiClient.h>
#include <EEPROM.h>
#include <WebServer.h>

#define SERIAL_DEBUG false               // Enable / Disable log - activer / désactiver le journal

#define MIN(X,Y)    ((X)<(Y)?(X):(Y))
#define MAX(X,Y)    ((X)>(Y)?(X):(Y))
#define STRINGIFY(s) STRINGIFY2(s)
#define STRINGIFY2(s) #s

#define PIN_PROG_SWITCH   0
#define PIN_PROG_LED      33
#define PROG_TIMEOUT      ( 15 * 60 * 1000 )    // 15 mins
#define TPUART            2
#define PIN_TPUART_RX     17   // UART2
#define PIN_TPUART_TX     16
#define PIN_TPUART_SAVE   NC   // Unused
#define PIN_TPUART_RESET  NC   // Unused

#define PIN_MUTE          23
#define PIN_DAC           25

#define NBBANKS           32

class NullStream : public Stream
{
public:
    virtual int available() { return 0; }
    virtual int read() { return -1; }
    virtual int peek() { return -1; }
    virtual void flush() {}
    virtual size_t write(uint8_t) { return 0; }
} nullDevice;


static const uint16_t outputPins[] = { 18, 19, 21, 22 };
enum { outputCount = sizeof(outputPins)/sizeof(outputPins[0]) };


struct Output
{
    void init(int baseAddr, uint16_t baseGO, uint16_t pinNb)
    {
        m_params.autoOffTimer = knx.paramInt(baseAddr) * 100;
        m_GO.onOff = baseGO;
        knx.getGroupObject(m_GO.onOff).dataPointType(DPT_Switch);
        knx.getGroupObject(m_GO.onOff).callback([this](GroupObject& go) {
            if (knx.getGroupObject(m_GO.block).value())
                return;
            bool value = go.value();
            if (value && m_params.autoOffTimer) {
                m_timer = millis() + m_params.autoOffTimer;
            }
            else {
                m_timer = 0;
            }
            digitalWrite(m_pin, value);
            knx.getGroupObject(m_GO.status).value(value);
          });
        m_GO.status = baseGO + 1;
        knx.getGroupObject(m_GO.status).dataPointType(DPT_Switch);
        m_GO.block = baseGO + 2;
        knx.getGroupObject(m_GO.block).dataPointType(DPT_Switch);
        m_pin = pinNb;
        pinMode(m_pin, OUTPUT);
    }

    void loop(uint32_t delta)
    {
        // Manage MONO Stable timer
        if (m_timer) {
            m_timer += delta;
            if (m_timer > millis()) {
                digitalWrite(m_pin, false);
                knx.getGroupObject(m_GO.status).value(false);
                m_timer = 0;
            }
        }
    }
  private:
    uint16_t m_pin;
    uint32_t m_timer = 0;
    struct {
      uint32_t autoOffTimer = 0;
    } m_params;
    struct {
      uint16_t onOff;
      uint16_t status;
      uint16_t block;
    } m_GO;
  public:
    enum { NBGO = sizeof(m_GO)/sizeof(uint16_t), SIZEPARAMS = sizeof(m_params) };    
} output[outputCount];

struct Player
{
    void init(int baseAddr, uint16_t baseGO, uint16_t pinNb, uint16_t mutePinNb)
    {
        (void)pinNb;
        m_mutePin = mutePinNb;
        pinMode(m_mutePin, OUTPUT);
        pinMode(pinNb, ANALOG);
        m_out.SetOutputModeMono(true);

        m_GO.playStop = baseGO;
        knx.getGroupObject(m_GO.playStop).dataPointType(DPT_Value_1_Ucount);
        knx.getGroupObject(m_GO.playStop).callback([this](GroupObject& go) {
            uint32_t value = (uint32_t)go.value();
            if (value) {
                if(knx.getGroupObject(m_GO.block).value())
                  return;
                play(go.value());
            }
            else {
                m_action = STOP;
            }
          });
        m_GO.pauseResume = baseGO + 1;
        knx.getGroupObject(m_GO.pauseResume).dataPointType(DPT_Switch);
        knx.getGroupObject(m_GO.pauseResume).callback([this](GroupObject& go) {
            bool value = go.value();
            if (!value) {
                if(knx.getGroupObject(m_GO.block).value())
                    return;
                m_action = RESUME;
            }
            else {
                m_action = PAUSE;
            }
          });
        m_GO.volume = baseGO + 2;
        knx.getGroupObject(m_GO.volume).dataPointType(DPT_Scaling);
        knx.getGroupObject(m_GO.volume).callback([this](GroupObject& go) {
            setVolume(go.value());
          });
        if ((uint8_t)knx.getGroupObject(m_GO.volume).value() == 0)   // Not initialised if 0 (volume is offset by 1)
            setVolume(100);
        m_GO.block = baseGO + 3;
        knx.getGroupObject(m_GO.block).dataPointType(DPT_Switch);
        m_GO.playing = baseGO + 4;
        knx.getGroupObject(m_GO.playing).dataPointType(DPT_Switch);
        m_GO.playingChannel = baseGO + 5;
        knx.getGroupObject(m_GO.playingChannel).dataPointType(DPT_Value_1_Ucount);
        for (int i = 0; i < NBBANKS; ++i) {
            m_GO.play[i] = baseGO + 6 + i;
            knx.getGroupObject(m_GO.play[i]).dataPointType(DPT_Switch);
            knx.getGroupObject(m_GO.play[i]).callback([this,i](GroupObject& go) { play(i); });
        }
        SPIFFS.begin(true);
        autodetect(UINT32_MAX);
    }

    void play(int bank) {
        if (pathFromChannel(bank)) {
            m_action = PLAY;
            m_playingChannel = bank;
        }
    }

    uint8_t volume() const { return (uint8_t)knx.getGroupObject(m_GO.volume).value() - 1; }
    void setVolume(int value)
    {
        knx.getGroupObject(m_GO.volume).value(value + 1);   // offset 1 because 0 is reserved when not init
        _setVolume(value);
    }
    void setVolume(const KNXValue& value)
    {
        _setVolume((uint8_t)value - 1);
    }

    void autodetect(uint32_t channel) {
        if (channel == UINT32_MAX) {
            for (int i = 0; i < NBBANKS; ++i)   autodetect(channel + 1);
        }
        const char* path = pathFromChannel(channel);
        if (path == NULL) return;
        FORMAT format = UNKNOWN;
        File f = SPIFFS.open(path, FILE_READ);
        if (f.available()) {
            char buffer[12];
            f.readBytes(buffer, 12);
            if (buffer[0] == 0xFF) {
                if (buffer[1] == 0xF1 || buffer[1] == 0xF9) {
                    format = AAC;
                }
                else
                if (buffer[1] == 0xFB) {
                    format = MP3;
                }
            }
            else if (memcmp(buffer, F("MThd"), 4) == 0) {
                format = MIDI;
            }
            else if (memcmp(buffer, F("fLaC"), 4) == 0) {
                format = FLAC;
            }
            else if (memcmp(buffer, F("RIFF"), 4) == 0 && memcmp(buffer + 8, F("WAVE"), 4) == 0) {
                format = WAV;
            }
            else if (memcmp(buffer, F("ID3"), 3) == 0) {
                format = MP3;
            }
            else  if (f.seek(0x438, SeekSet) && f.readBytes(buffer, 4) == 4 && memcmp(buffer, F("M.K."), 4) == 0) {
                format = MOD;
            }
        }
        f.close();
        m_format[channel] = format;
    }
    static const char* pathFromChannel(uint32_t channel)
    {
        static const __FlashStringHelper* Banks[] = { F("/bank_1"), F("/bank_2"), F("/bank_3"), F("/bank_4"),
            F("/bank_5"), F("/bank_6"), F("/bank_7"), F("/bank_8"),
            F("/bank_9"), F("/bank_10"), F("/bank_11"), F("/bank_12"),
            F("/bank_13"), F("/bank_14"), F("/bank_15"), F("/bank_16"),
            F("/bank_17"), F("/bank_18"), F("/bank_19"), F("/bank_20"),
            F("/bank_21"), F("/bank_22"), F("/bank_23"), F("/bank_24"),
            F("/bank_25"), F("/bank_26"), F("/bank_27"), F("/bank_28"),
            F("/bank_29"), F("/bank_30"), F("/bank_31"), F("/bank_32") };
        if (channel >= sizeof(Banks)/sizeof(Banks[0])) {
            return NULL;
        }
        return (const char*)Banks[channel];
    }

    AudioGenerator* audioGeneratorbuilder(uint32_t channel)
    {
        if (pathFromChannel(channel) == NULL) {
            return NULL;
        }
        switch (m_format[channel]) {
#ifdef ENABLE_AAC
            case AAC: return new AudioGeneratorAAC(); break;
#endif
#ifdef ENABLE_MP3
            case MP3: return new AudioGeneratorMP3(); break;
#endif
#ifdef ENABLE_MIDI
            case MIDI: return new AudioGeneratorMIDI(); break;
#endif
#ifdef ENABLE_FLAC
            case FLAC: return new AudioGeneratorFLAC(); break;
#endif
#ifdef ENABLE_WAV
            case WAV: return new AudioGeneratorWAV(); break;
#endif
#ifdef ENABLE_MOD
            case MOD: return new AudioGeneratorMOD(); break;
#endif
            default: return NULL; break;
        }
    }

    void loop()
    {
        switch (m_action) {
            case PLAY: {
                clear();
                const char* path = pathFromChannel(m_playingChannel);
                if (path) {
                    m_file = new AudioFileSourceSPIFFS(path);
                    if (m_file) {
                        m_player = audioGeneratorbuilder(m_playingChannel);
                        if (m_player) {
                            knx.getGroupObject(m_GO.playingChannel).value(m_playingChannel + 1);
                            knx.getGroupObject(m_GO.play[m_playingChannel]).value(true);
                            m_player->begin(m_file, &m_out);
                            knx.getGroupObject(m_GO.playing).value(true);
                            m_action = NONE;
                            digitalWrite(m_mutePin, false);
                        }
                        else {
                            delete m_file; m_file = NULL;
                        }
                    }
                }
            }; break;
            case STOP: {
                if (m_player && m_player->isRunning()) {
                    knx.getGroupObject(m_GO.playing).value(false);
                    knx.getGroupObject(m_GO.play[m_playingChannel]).value(false);
                    knx.getGroupObject(m_GO.playingChannel).value(0);
                    m_player->stop();
                    clear();
                }
                m_action = NONE;
            }; break;
            case PAUSE: {
                if (m_player && m_player->isRunning()) {
                    knx.getGroupObject(m_GO.playing).value(false);
                    knx.getGroupObject(m_GO.play[m_playingChannel]).value(false);
                }
            }; break;
            case RESUME: {
                if (m_player && m_player->isRunning()) {
                    knx.getGroupObject(m_GO.playing).value(true);
                    knx.getGroupObject(m_GO.play[m_playingChannel]).value(true);
                }
                m_action = NONE;
            }; break;
            case NONE: default: break;
        }
        if (m_player && m_player->isRunning() && m_action != PAUSE) {
            if (!m_player->loop()) {
                knx.getGroupObject(m_GO.playing).value(false);
                knx.getGroupObject(m_GO.play[m_playingChannel]).value(false);
                knx.getGroupObject(m_GO.playingChannel).value(0);
                m_player->stop();
                clear();
            }
        }
    }

    void clear()
    {
        m_playingChannel = 0;
        m_action = NONE;
        digitalWrite(m_mutePin, true);
        if (m_player)
            m_player->stop();
        if (m_file) {
            delete m_file;
            m_file = NULL;
        }
        if (m_player) {
            delete m_player;
            m_player = NULL;
        }
    }
  private:
    void _setVolume(uint8_t value)
    {
        m_out.SetGain(MIN(value, 100) * 4.f / 100);
    }

    enum ACTION { NONE, STOP, PLAY, PAUSE, RESUME } m_action;
    int m_playingChannel;
    struct {
    } m_params;
    struct {
      uint16_t playStop;
      uint16_t pauseResume;
      uint16_t volume;
      uint16_t block;
      uint16_t playing;
      uint16_t playingChannel;
      uint16_t play[NBBANKS];
    } m_GO;
    int m_mutePin;
    AudioGenerator *m_player = NULL;
    AudioFileSourceSPIFFS *m_file = NULL;
    AudioOutputI2S m_out = AudioOutputI2S(PIN_DAC, AudioOutputI2S::INTERNAL_DAC);
    enum FORMAT : uint8_t { UNKNOWN, MP3, AAC, FLAC, WAV, MOD, MIDI } m_format[NBBANKS];
  public:
    enum { NBGO = sizeof(m_GO)/sizeof(uint16_t), SIZEPARAMS = sizeof(m_params) };    
} player;

// Web server port - port du serveur web
#define WEB_SERVER_PORT 80
#define URI_WIFI "/reset"
#define URI_REBOOT "/reboot"
#define URI_UPDATE "/update"
#define URI_UPLOAD "/upload"
#define URI_STATUS "/status"
#define URI_PLAY "/play"
#define URI_VOLUME "/volume"
#define URI_FORMAT "/format"
#define URI_REMOVE "/remove"
#define URI_ROOT "/"

WebServer server ( WEB_SERVER_PORT );

#define REBOOT_TIMER (1)
#define OTA_REBOOT_TIMER (1)

int64_t rebootRequested = 0;
bool uploadStatus = false;

String ip2Str(IPAddress ip) {
    String s;
    for (int i = 0; i < 4; i++) {
        s += i  ? "." + String(ip[i]) : String(ip[i]);
    }
    return s;
}

static void requestReboot(int timer = REBOOT_TIMER)
{
    if (timer == 0) {
        ESP.restart();
    }
    else {
        rebootRequested = millis() + timer * 1000;    
    }
}

static void startServer() {
    server.on ( URI_ROOT, [](){
        const __FlashStringHelper* info =
          F("<html>"
              "<head>"
                "<title>" FW_TAG "</title>"
                "<script type=\"text/javascript\">"
                "function invoke(url)"
                "{"
                    "var xhr = new XMLHttpRequest();"
                    "xhr.open(\"GET\", url, true);"
                    "xhr.send(null);"
                "};"
                "function update()"
                "{"
                    "var xhr = new XMLHttpRequest();"
                    "xhr.open(\"GET\", \"" URI_STATUS "\", true);"
                    "xhr.onload = function (e) {"
                    "if (xhr.readyState === 4) {"
                        "if (xhr.status === 200) {"
                        "var obj = JSON.parse(xhr.responseText);"
                        "document.getElementById(\"ssid\").innerHTML = obj.ssid;"
                        "document.getElementById(\"rssi\").innerHTML = obj.rssi;"
                        "document.getElementById(\"ip\").innerHTML = obj.ip;"
                        "document.getElementById(\"mac\").innerHTML = obj.mac;"
                        "document.getElementById(\"vol\").value = obj.volume;"
                        "document.getElementById(\"reboot\").innerHTML = obj.rebootTimer>0?\" - \"+obj.rebootTimer:\"\";"
                        "}"
                    "}"
                    "};"
                    "xhr.send(null);"
                "};"
                "update();"
                "setInterval(update, 1000);"
                "</script>"
            "</head>"
            "<body>"
                "<h1>" FW_TAG "<span id=\"version\"> (v" FW_VERSION ")</span></h1>"
                "<table style=\"height: 60px;\" width=\"100%\">"
                "<tbody>"
                    "<tr>"
                    "<td style=\"width: 50%;\">"
                        "<span class=\"info\">SSID: </span><span id=\"ssid\"></span>"
                        "<br/>"
                        "<span class=\"info\">RSSI: </span><span id=\"rssi\"></span>"
                        "<br/>"
                        "<span class=\"info\">IP: </span><span id=\"ip\"></span>"
                        "<br/>"
                        "<span class=\"info\">MAC: </span><span id=\"mac\"></span>"
                    "</td>"
                    "</tr>"
                    "<tr>"
                    "<td style=\"width: 50%;\">"
                        "Bell: <input id=\"bank\" type=\"number\" min=\"1\" max=\"" STRINGIFY(NBBANK) "\" value=\"1\" onchange=\"document.getElementById('uploadForm').action = '" URI_UPLOAD "?id=this.value'\" required>"
                        "<input type=\"button\" onclick=\"invoke('" URI_PLAY "?id=document.getElementById('bank').value')\" value=\"Play\"/>"
                        "<input type=\"button\" onclick=\"invoke('" URI_REMOVE "?id=document.getElementById('bank').value')\" value=\"Clear\"/>"
                        "<form id=\"uploadForm\" method=\"post\" enctype=\"multipart/form-data\" action=\"\"><span class=\"action\">Upload: </span><input type=\"file\" name=\"fileToUpload\" id=\"uploadFile\" /><input type=\"submit\" value=\"Upload\" id=\"uploadSubmit\"/></form>"
                        "<br/>"
                        "Volume: <input type=\"range\" id=\"vol\" min=\"0\" max=\"100\" onchange=\"invoke('" URI_VOLUME "?value='+this.value)\"/>"
                        "<br/>"
                        "<a class=\"link\" href=\"\" onclick=\"invoke(\'" URI_FORMAT "\');return false;\">Remove All Bells</a>"
                        "<br/>"
                        "<a class=\"link\" href=\"\" onclick=\"invoke(\'" URI_REBOOT "\');return false;\">Reboot Device</a><span id=\"reboot\"></span>"
                        "<br/>"
                        "<a class=\"link\" href=\"\" onclick=\"invoke(\'" URI_WIFI "\');return false;\">Reset WiFi</a>"
                        "<br/>"
#ifdef ENABLE_UPDATE
                        "<form id=\"upgradeForm\" method=\"post\" enctype=\"multipart/form-data\" action=\"" URI_UPDATE "\"><span class=\"action\">Upgrade Firmware: </span><input type=\"file\" name=\"fileToUpgrade\" id=\"upgradeFile\" /><input type=\"submit\" value=\"Upgrade\" id=\"upgradeSubmit\"/></form>"
                        "<br/>"
#endif
                    "</td>"
                    "</tr>"
                "</tbody>"
                "</table>"
              "</body>"
            "</html>");
        server.send(200, F("text/html"), info);
      });
    server.on ( URI_PLAY, [](){ player.play(server.arg("id").toInt()); });
    server.on ( URI_VOLUME, [](){ if (!server.arg("value").isEmpty()) player.setVolume(server.arg("value").toInt()); });
    server.on ( URI_STATUS, [](){
        unsigned long currentTimer = millis();
        String info = "{"
                        "\"firmware\":\"" FW_TAG "\","
                        "\"version\":\"" FW_VERSION "\","
                        "\"ssid\":\"" + WiFi.SSID() + "\","
                        "\"rssi\":\"" + String(WiFi.RSSI()) + "\","
                        "\"ip\":\"" + ip2Str(WiFi.localIP()) + "\","
                        "\"mac\":\"" + WiFi.macAddress() + "\","
                        "\"volume\":\"" + String(player.volume()) + "\","
                        "\"chipId\":\"" + String((uint32_t)ESP.getEfuseMac()) + "\","
                        "\"reboot\":\"" + String(rebootRequested > 0 ? "true" : "false") + "\","
                        "\"rebootTimer\":" + String(MAX(0, int((rebootRequested - currentTimer) / 1000))) + ""
                        "}";
        server.send(200, F("application/json"), info);
      });
    server.on ( URI_WIFI, [](){
        server.send(200);
        esp_wifi_restore();
        requestReboot(0);
      });
    server.on ( URI_REBOOT, [](){
        server.send(200);
        esp_wifi_restore();
        requestReboot(0);
      });
    server.on ( URI_FORMAT, [](){
        SPIFFS.end();
        server.send(SPIFFS.format()?200:500);
        SPIFFS.begin();
        player.autodetect(UINT32_MAX);
      });
    server.on ( URI_REMOVE, [](){
        int channel = server.arg("id").toInt() - 1;
        const char* path = player.pathFromChannel(channel);
        if (path) {
            SPIFFS.remove(path);
            player.autodetect(channel);
        }
        server.send(200);
      });
    server.on ( URI_UPLOAD, HTTP_POST, []() {
        String html = "<html>"
                        "<head>"
                          "<title>" FW_TAG " - OTA</title>" +
                          String(uploadStatus ? "<meta http-equiv=\"refresh\" content=\"url=/\">" : "") +
                        "</head>"
                        "<body>" + (!uploadStatus ? "FAIL" : "OK") + "</body>"
                      "</html>";
        server.sendHeader(F("Connection"), F("close"));
        server.send(200, F("text/html"), html);
        requestReboot();
      }, [](){
        HTTPUpload& upload = server.upload();
        int channel = server.arg("id").toInt() - 1;
        if (upload.status == UPLOAD_FILE_START) {
            if (uploadStatus) {
                const char* path = player.pathFromChannel(channel);
                if (path) {
                    File file = SPIFFS.open(path, FILE_WRITE);
                    if (!file.available()) {
                        uploadStatus = false;
                    }
                    file.close();
                    player.autodetect(channel);
                }
                else {
                    uploadStatus = false;
                }
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (uploadStatus) {
                const char* path = player.pathFromChannel(channel);
                if (path) {
                    File file = SPIFFS.open(path, FILE_APPEND);
                    if (file.write(upload.buf, upload.currentSize) != upload.currentSize) {
                        // error
                        uploadStatus = false;
                    }
                    file.close();
                }
                else {
                    uploadStatus = false;
                }
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            player.autodetect(channel);
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            const char* path = player.pathFromChannel(channel);
            if (path) {
                SPIFFS.remove(path);
            }
        }
        yield();
      } );
#ifdef ENABLE_UPDATE
     server.on ( URI_UPDATE, HTTP_POST, []() {
        String html = "<html>"
                        "<head>"
                          "<title>" FW_TAG " - OTA</title>" +
                          (Update.hasError() ? "<meta http-equiv=\"refresh\" content=\"" + String(OTA_REBOOT_TIMER) + "; url=/\">" : "") +
                        "</head>"
                        "<body>" + (Update.hasError() ? "FAIL" : "OK") + "</body>"
                      "</html>";
        server.sendHeader(F("Connection"), F("close"));
        server.send(200, F("text/html"), html);
        requestReboot();
      }, [](){
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
            if (!Update.begin(maxSketchSpace)) { //start with max available size
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
                Update.printError(Serial);
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (!Update.end(true)) { //true to set the size to the current progress
                Update.printError(Serial);
            }
        }
        yield();
      } );
#endif
    server.begin();
}

bool wifiOn = true;
void setup()
{
    ArduinoPlatform::SerialDebug = &nullDevice;
    static HardwareSerial serialTpuart(TPUART);
    knx.platform().knxUart(&serialTpuart);
    knx.ledPin(PIN_PROG_LED);
    knx.ledPinActiveOn(HIGH);
    knx.buttonPin(PIN_PROG_SWITCH);
    knx.buttonPinInterruptOn(RISING);

    // read adress table, association table, groupobject table and parameters from eeprom
    knx.readMemory();

    if (knx.configured()) {
        uint16_t offsetGO = 0; int offsetParam = 0;
        // Wifi On/Off
        wifiOn = false;
        knx.getGroupObject(offsetGO).dataPointType(DPT_Switch);
        knx.getGroupObject(offsetGO).callback([](GroupObject& go) { wifiOn = go.value(); });
        knx.getGroupObject(offsetGO).value(false);
        ++offsetGO;
        for (uint16_t i = 0; i < outputCount; ++i, offsetGO += Output::NBGO, offsetParam += Output::SIZEPARAMS) {
            output[i].init(offsetParam, offsetGO, outputPins[i]);
        }
        player.init(offsetGO, offsetParam, PIN_DAC, PIN_MUTE);
    }

    // start the framework.
    knx.start();

    startServer();
}

void loop() 
{
    // don't delay here to much. Otherwise you might lose packages or mess up the timing with ETS
    knx.loop();

    // only run the application code if the device was configured with ETS
    if (knx.configured()) {
        static uint32_t lastTime = 0;
        uint32_t time = millis();
        const uint32_t delta = time - lastTime;
        if (delta > 10) {
            for (int i = 0; i < outputCount; ++i) {
                output[i].loop(delta);
            }
            lastTime = time;
        }
        player.loop();

        if (wifiOn || knx.progMode()) {
            if (!WiFi.isConnected()) {
                // Wi-Fi connection - Connecte le module au réseau Wi-Fi
                // attempt to connect; should it fail, fall back to AP
                WiFiManager().autoConnect((FW_TAG + String((uint32_t)ESP.getEfuseMac())).c_str(), "");
            }
        }
        else {
            WiFi.disconnect(true);
        }
    }

    static uint32_t timerProgMode = 0;
    if (knx.progMode()) {
        if (timerProgMode == 0) {
            timerProgMode = millis();
        }
        else {
            if (millis() - timerProgMode > PROG_TIMEOUT) {
                knx.progMode(false);
                timerProgMode = 0;
            }
        }
    }
    else {
        timerProgMode = 0;
    }

    server.handleClient();
    unsigned long currentTime = millis();
    if (rebootRequested != 0 && currentTime > rebootRequested) {
        rebootRequested = 0;
        requestReboot(0);
    }
}