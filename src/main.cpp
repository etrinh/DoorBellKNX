/*
    DoorBell KNX

    WARNING:
    WiFiServer.cpp MUST be PATCHED according to bug report (to add SO_REUSEADDR):
    https://github.com/espressif/arduino-esp32/issues/3960
    Else no webserver will be able to handle requests after wifi is reset or restarted

*/
#define FW_TAG       "DoorBell KNX"
#define FW_VERSION   "1.00"

//#define ENABLE_DEBUG
#define ENABLE_UPDATE
#define ENABLE_MP3
//#define ENABLE_FLAC
//#define ENABLE_WAV    // SPIFFS too slow
//#define ENABLE_MIDI   // https://github.com/earlephilhower/ESP8266Audio/issues/240 + printf in midi + tinysoundfont
//#define ENABLE_MOD    // Poor quality on SPIFFS
//#define ENABLE_AAC

#include <Arduino.h>
#include <esp_pm.h>
#include <soc/soc.h>           //disable brownout problems
#include <soc/rtc_cntl_reg.h>  //disable brownout problems
#include <knx.h>
#include <WiFi.h>
#include <SPIFFS.h>
#include <AudioFileSourceSPIFFS.h>
#ifdef ENABLE_MP3
  #if 0 // Lower Quality
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
#include <Arduino.h>
#include <WebServer.h>

#define WATCHDOG_TIMEOUT  (3 * 60 * 1000 * 1000)
hw_timer_t *watchdog = NULL;

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
#define PIN_DAC           0  //PIN 25 -> https://github.com/earlephilhower/ESP8266Audio/issues/95

#define NBBANKS           32
#define BANK_MAXNAMESIZE  32
#define META_PATH         "/meta"
#ifdef ENABLE_MIDI
# define SOUNDFONT_SUFFIX  ".sf2"
# define SOUNDFONT_PATH    "/soundfont" SOUNDFONT_SUFFIX
#endif

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
        m_GO.onOff = baseGO++;
        knx.getGroupObject(m_GO.onOff).dataPointType(DPT_Switch);
        m_GO.status = baseGO++;
        knx.getGroupObject(m_GO.status).dataPointType(DPT_Switch);
        m_GO.block = baseGO++;
        knx.getGroupObject(m_GO.block).dataPointType(DPT_Switch);

        // Callbacks
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
    enum FORMAT : uint8_t { UNKNOWN = (uint8_t)-1, NO_FILE = 0, MP3, AAC, FLAC, WAV, MOD, MIDI };
    void init(uint16_t pinNb, uint16_t mutePinNb)
    {
        m_mutePin = mutePinNb;
        pinMode(m_mutePin, OUTPUT);
        digitalWrite(m_mutePin, true);
        m_out.SetOutputModeMono(true);

        memset(&m_content, 0, sizeof(m_content));
        m_content.volume = 100;
        File f = SPIFFS.open(META_PATH, FILE_READ);
        if (f.available()) {
            f.read((uint8_t*)&m_content, sizeof(m_content));
        }
        f.close();
    }

    void initKNX(int baseAddr, uint16_t baseGO)
    {
        m_GO.playStop = baseGO++;
        knx.getGroupObject(m_GO.playStop).dataPointType(DPT_Value_1_Ucount);
        m_GO.pauseResume = baseGO++;
        knx.getGroupObject(m_GO.pauseResume).dataPointType(DPT_Switch);
        m_GO.volume = baseGO++;
        knx.getGroupObject(m_GO.volume).dataPointType(DPT_Scaling);
        m_GO.block = baseGO++;
        knx.getGroupObject(m_GO.block).dataPointType(DPT_Switch);
        m_GO.playing = baseGO++;
        knx.getGroupObject(m_GO.playing).dataPointType(DPT_Switch);
        m_GO.playingChannel = baseGO++;
        knx.getGroupObject(m_GO.playingChannel).dataPointType(DPT_Value_1_Ucount);
        for (int i = 0; i < NBBANKS; ++i) {
            m_GO.play[i] = baseGO++;
            knx.getGroupObject(m_GO.play[i]).dataPointType(DPT_Switch);
        }

        // Callbacks
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
        knx.getGroupObject(m_GO.volume).callback([this](GroupObject& go) {
            setVolume(go.value());
          });
        for (int i = 0; i < NBBANKS; ++i) {
            knx.getGroupObject(m_GO.play[i]).callback([this,i](GroupObject& go) {
                println((bool)go.value());
                println(i);
                if (go.value()) {
                    play(i + 1);
                }
                else {
                    m_action = STOP;
                }
              });
        }
    }

    void play(int bank) {
        if (pathFromChannel(bank)) {
            m_action = PLAY;
            m_playingChannel = bank;
        }
    }

    void pauseResume() {
        if (m_player) m_action = m_action==PAUSE?RESUME:PAUSE;
    }

    void stop() {
        if (m_player) m_action = STOP;
    }
    int playingBank() const {
        return m_playingChannel;
    }

    FORMAT format(int bank) const {
        if (pathFromChannel(bank)) {
            return m_content.bank[bank - 1].format;
        }
        return NO_FILE;
    }

    uint8_t volume() const { return m_content.volume; }
    void setVolume(uint8_t value)
    {
        if (knx.configured())
            knx.getGroupObject(m_GO.volume).value(value);
        _setVolume(value);
    }
    void setVolume(const KNXValue& value)
    {
        _setVolume((uint8_t)value);
    }

    FORMAT autodetect(uint32_t channel = UINT32_MAX) {
        const char* path = pathFromChannel(channel);
        if (path == NULL) return NO_FILE;
        FORMAT format = UNKNOWN;
        File f = SPIFFS.open(path, FILE_READ);
        if (f.available()) {
            char buffer[12];
            f.readBytes(buffer, 12);
            if (buffer[0] == 0xFF) {
                if (buffer[1] == 0xF1 || buffer[1] == 0xF9) {
                    format = AAC;
                }
                else if (buffer[1] == 0xFB) {
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
        else {
            format = NO_FILE;
        }
        f.close();
        return format;
    }
    String channelName(uint32_t channel)
    {
        if (pathFromChannel(channel) == NULL) return String();
        String n;
        n.reserve(BANK_MAXNAMESIZE);
        char * p = m_content.bank[channel - 1].name;
        for (int i = 0; i < BANK_MAXNAMESIZE && *p; ++i, ++p) 
            n += *p;
        return n;
    }
    void setChannelName(uint32_t channel, String name, FORMAT format)
    {
        if (pathFromChannel(channel) == NULL) return;
        m_content.bank[channel - 1].name[0] = 0;
        strncpy(m_content.bank[channel - 1].name, name.c_str(), BANK_MAXNAMESIZE);
        m_content.bank[channel - 1].name[MIN(BANK_MAXNAMESIZE - 1, name.length())] = 0;
        m_content.bank[channel - 1].format = format;
    }
    void flushChannels()
    {
        File f = SPIFFS.open(META_PATH, "w");
        if (f.available()) {
            f.write((uint8_t*)&m_content, sizeof(m_content));
        }
        f.close();
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
        if (channel == 0 || channel > sizeof(Banks)/sizeof(Banks[0])) {
            return NULL;
        }
        return (const char*)Banks[channel - 1];
    }
    void removeChannel(uint32_t channel)
    {
        const char* path = pathFromChannel(channel);
        if (path) {
            setChannelName(channel, String(), NO_FILE);
            SPIFFS.remove(path);
        }
    }

    void clean()
    {
        clear();
        memset(&m_content, 0, sizeof(m_content));
        m_content.volume = 100;
    }
#ifdef ENABLE_MIDI
    bool hasSoundFont() const
    {
        return SPIFFS.exists(SOUNDFONT_PATH);
    }
#endif
    AudioGenerator* audioGeneratorbuilder(uint32_t channel)
    {
        if (pathFromChannel(channel) == NULL) {
            return NULL;
        }
        switch (m_content.bank[channel - 1].format) {
#ifdef ENABLE_AAC
            case AAC: return new AudioGeneratorAAC(); break;
#endif
#ifdef ENABLE_MP3
            case MP3: return new AudioGeneratorMP3(); break;
#endif
#ifdef ENABLE_MIDI
            case MIDI: {
                if (hasSoundFont()) {
                    AudioGeneratorMIDI* midi = new AudioGeneratorMIDI();
                    delete m_sf2;
                    m_sf2 = new AudioFileSourceSPIFFS(SOUNDFONT_PATH);
                    midi->SetSoundfont(m_sf2);
                    midi->SetSampleRate(22050);
                    return midi;
                }
             } break;
#endif
#ifdef ENABLE_FLAC
            case FLAC: return new AudioGeneratorFLAC(); break;
#endif
#ifdef ENABLE_WAV
            case WAV: return new AudioGeneratorWAV(); break;
#endif
#ifdef ENABLE_MOD
            case MOD: {
                AudioGeneratorMOD* mod = new AudioGeneratorMOD();
                mod->SetBufferSize(3*1024);
                mod->SetSampleRate(22050);
                mod->SetStereoSeparation(32);
                return mod;
            }; break;
#endif
            default: break;
        }
        return NULL;
    }

    void loop()
    {
        switch (m_action) {
            case PLAY: {
                uint32_t channel = m_playingChannel;
                clear();
                const char* path = pathFromChannel(channel);
                if (path) {
                    m_file = new AudioFileSourceSPIFFS(path);
                    if (m_file) {
                        m_player = audioGeneratorbuilder(channel);
                        if (m_player) {
                            if (m_player->begin(m_file, &m_out)) {
                                if (knx.configured()) {
                                    knx.getGroupObject(m_GO.playingChannel).value(channel);
                                    knx.getGroupObject(m_GO.play[channel - 1]).value(true);
                                    knx.getGroupObject(m_GO.playing).value(true);
                                }
                                digitalWrite(m_mutePin, false);
                                m_playingChannel = channel;
                            }
                            else {
                                clear();
                            }
                        }
                        else {
                            delete m_file; m_file = NULL;
                        }
                    }
                    m_action = NONE;
                }
            }; break;
            case STOP: {
                if (m_player && m_player->isRunning()) {
                    if (knx.configured()) {
                        knx.getGroupObject(m_GO.playing).value(false);
                        knx.getGroupObject(m_GO.play[m_playingChannel - 1]).value(false);
                        knx.getGroupObject(m_GO.playingChannel).value(0);
                    }
                    m_player->stop();
                    clear();
                }
                m_action = NONE;
            }; break;
            case PAUSE: {
                if (m_player && m_player->isRunning()) {
                    if (knx.configured()) {
                        knx.getGroupObject(m_GO.playing).value(false);
                        knx.getGroupObject(m_GO.play[m_playingChannel - 1]).value(false);
                    }
                    digitalWrite(m_mutePin, true);
                }
            }; break;
            case RESUME: {
                if (m_player && m_player->isRunning()) {
                    if (knx.configured()) {
                        knx.getGroupObject(m_GO.playing).value(true);
                        knx.getGroupObject(m_GO.play[m_playingChannel - 1]).value(true);
                    }
                    digitalWrite(m_mutePin, false);
                }
                m_action = NONE;
            }; break;
            case NONE: default: break;
        }
        if (m_player && m_player->isRunning() && m_action != PAUSE) {
            if (!m_player->loop()) {
                if (knx.configured()) {
                    knx.getGroupObject(m_GO.playing).value(false);
                    knx.getGroupObject(m_GO.play[m_playingChannel - 1]).value(false);
                    knx.getGroupObject(m_GO.playingChannel).value(0);
                }
                m_player->stop();
                clear();
            }
        }
    }

  private:
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
        if (m_sf2) {
            delete m_sf2;
            m_sf2 = NULL;
        }
        if (m_player) {
            delete m_player;
            m_player = NULL;
        }
    }

    void _setVolume(uint8_t value)
    {
        m_content.volume = value;
        m_out.SetGain(MIN(value, 100) * 4.f / 100);
        File f = SPIFFS.open(META_PATH, "w");
        if (f.available()) {
            f.seek((ptrdiff_t)&m_content.volume - (ptrdiff_t)&m_content);
            f.write((uint8_t*)&m_content.volume, sizeof(m_content.volume));
        }
        f.close();
    }

    enum ACTION { NONE, STOP, PLAY, PAUSE, RESUME } m_action;
    int m_playingChannel = 0;
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
    AudioFileSourceSPIFFS *m_sf2 = NULL;
    AudioOutputI2S m_out = AudioOutputI2S(PIN_DAC, AudioOutputI2S::INTERNAL_DAC, 128);
    struct {
        struct {
            char name[BANK_MAXNAMESIZE];
            FORMAT format;
        } bank[NBBANKS];
        uint8_t volume;
    } m_content;
  public:
    enum { NBGO = sizeof(m_GO)/sizeof(uint16_t), SIZEPARAMS = 0 };
} player;

// Web server port - port du serveur web
#define WEB_SERVER_PORT 80
#define URI_WIFI "/reset"
#define URI_REBOOT "/reboot"
#define URI_PROGMODE "/progmode"
#define URI_UPDATE "/update"
#define URI_UPLOAD "/upload"
#define URI_STATUS "/status"
#define URI_PLAY "/play"
#define URI_STOP "/stop"
#define URI_PAUSE "/pause"
#define URI_VOLUME "/volume"
#define URI_FORMAT "/format"
#define URI_REMOVE "/remove"
#define URI_ROOT "/"

WebServer server ( WEB_SERVER_PORT );
enum SERVER_STATE: uint8_t { DISCONNECTED = 0, CONNECTING, CONNECTED, RUNNING } serverState = DISCONNECTED;

#define REBOOT_TIMER (1)
#define OTA_REBOOT_TIMER (1)

int64_t rebootRequested = 0;
bool wifiResetRequested = false;

static void requestReboot(int timer = REBOOT_TIMER)
{
    if (timer == 0) {
        ESP.restart();
    }
    else {
        rebootRequested = millis() + timer * 1000;    
    }
}

static void initWebServer() {
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
                        "document.getElementById(\"playing\").innerHTML = obj.playing>0?(obj.playing):\"\";"
                        "document.getElementById(\"vol\").value = obj.volume;"
                        "document.getElementById(\"reboot\").innerHTML = obj.rebootTimer>0?\" - \"+obj.rebootTimer:\"\";"
#ifdef ENABLE_MIDI
                        "document.getElementById(\"soundfont\").innerHTML = obj.hasSoundFont?\"Yes\":\"No\";"
#endif
                        "document.getElementById(\"usedSpace\").innerHTML = obj.usedSpace;"
                        "document.getElementById(\"totalSpace\").innerHTML = obj.totalSpace;"
                        "document.getElementById(\"freeSpace\").innerHTML = obj.totalSpace-obj.usedSpace;"
                        "document.getElementById(\"bankName\").innerHTML = obj.banks[document.getElementById(\"bank\").value-1].name;"
                        "document.getElementById(\"progMode\").innerHTML = obj.progMode?\"on\":\"off\";"
                        "}"
                    "}"
                    "};"
                    "xhr.send(null);"
                "};"
                "update();"
                "setInterval(update, 5000);"
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
                        "<br/>"
#ifdef ENABLE_MIDI
                        "<span class=\"info\">Has SoundFont (sf2 file for MIDI playback): </span><span id=\"soundfont\"></span>"
                        "<br/>"
#endif
                        "<span class=\"info\">Space: </span><span id=\"usedSpace\"></span> (Used) / <span id=\"totalSpace\"></span> (Total) / </span><span id=\"freeSpace\"></span> (Free)"
                        "<br/>"
                        "<span class=\"info\">Playing: </span><span id=\"playing\"></span>"
                    "</td>"
                    "</tr>"
                    "<tr>"
                    "<td style=\"width: 50%;\">"
                        "Bell: <input id=\"bank\" type=\"number\" min=\"1\" max=\"" STRINGIFY(NBBANKS) "\" value=\"1\" onchange=\"document.getElementById('uploadForm').action = '" URI_UPLOAD "?id='+this.value; update();\" required>"
                        "<span id=\"bankName\"></span>"
                        "<input type=\"button\" onclick=\"invoke('" URI_PLAY "?id='+document.getElementById('bank').value)\" value=\"Play\"/>"
                        "<input type=\"button\" onclick=\"invoke('" URI_STOP "')\" value=\"Stop\"/>"
                        "<input type=\"button\" onclick=\"invoke('" URI_PAUSE "')\" value=\"||>\"/>"
                        "<input type=\"button\" onclick=\"invoke('" URI_REMOVE "?id='+document.getElementById('bank').value)\" value=\"Clear\"/>"
                        "<form id=\"uploadForm\" method=\"post\" enctype=\"multipart/form-data\" action = \"" URI_UPLOAD "?id=1\"><span class=\"action\">Upload: </span><input type=\"file\" name=\"fileToUpload\" id=\"uploadFile\" accept=\""
#ifdef ENABLE_AAC
                        ".aac,"
#endif
#ifdef ENABLE_MP3
                        ".mp3,"
#endif
#ifdef ENABLE_MIDI
                        ".mid,"
#endif
#ifdef ENABLE_FLAC
                        ".flac,"
#endif
#ifdef ENABLE_WAV
                        ".wav,"
#endif
#ifdef ENABLE_MOD
                        ".mod,"
#endif
                        "|audio/*\" /><input type=\"submit\" value=\"Upload\" id=\"uploadSubmit\"/></form>"
                        "<br/>"
                        "Volume: <input type=\"range\" id=\"vol\" min=\"0\" max=\"100\" onchange=\"invoke('" URI_VOLUME "?value='+this.value)\"/>"
                        "<br/>"
                        "<a class=\"link\" href=\"\" onclick=\"invoke(\'" URI_FORMAT "\');return false;\">Remove All Bells</a>"
                        "<br/>"
                        "<a class=\"link\" href=\"\" onclick=\"invoke(\'" URI_REBOOT "\');return false;\">Reboot Device</a><span id=\"reboot\"></span>"
                        "<br/>"
                        "<a class=\"link\" href=\"\" onclick=\"invoke(\'" URI_WIFI "\');return false;\">Reset WiFi</a>"
                        "<br/>"
                        "<a class=\"link\" href=\"\" onclick=\"invoke(\'" URI_PROGMODE "\');return false;\">Toggle Program Mode :</a><span id=\"progMode\"></span>"
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
    server.on ( URI_PLAY, [](){ player.play(server.arg("id").toInt()); server.send(200); });
    server.on ( URI_PAUSE, [](){ player.pauseResume(); server.send(200); });
    server.on ( URI_STOP, [](){ player.stop(); server.send(200); });
    server.on ( URI_VOLUME, [](){ if (!server.arg("value").isEmpty()) player.setVolume(server.arg("value").toInt()); server.send(200); });
    server.on ( URI_STATUS, [](){
        unsigned long currentTimer = millis();
        String banks;
        for (size_t i = 1; i <= NBBANKS; ++i) {
            banks += "{\"bank\":" + String(i) + ",\"format\":" + String(player.format(i)) + ",\"name\":\"" + player.channelName(i) + "\"}";
            if (i < NBBANKS) banks += ",";
        }
        String info = "{"
                        "\"firmware\":\"" FW_TAG "\","
                        "\"version\":\"" FW_VERSION "\","
                        "\"ssid\":\"" + WiFi.SSID() + "\","
                        "\"rssi\":\"" + String(WiFi.RSSI()) + "\","
                        "\"ip\":\"" + WiFi.localIP().toString() + "\","
                        "\"mac\":\"" + WiFi.macAddress() + "\","
                        "\"playing\":\"" + String(player.playingBank()) + "\","
                        "\"banks\":[" + banks + "],"
#ifdef ENABLE_MIDI
                        "\"hasSoundFont\":" + String(player.hasSoundFont()) + ","
#endif
                        "\"volume\":\"" + String(player.volume()) + "\","
                        "\"chipId\":\"" + String((uint32_t)ESP.getEfuseMac()) + "\","
                        "\"reboot\":\"" + String(rebootRequested > 0 ? "true" : "false") + "\","
                        "\"usedSpace\":" + String(SPIFFS.usedBytes()) + ","
                        "\"totalSpace\":" + String(SPIFFS.totalBytes()) + ","
                        "\"rebootTimer\":" + String(MAX(0, int((rebootRequested - currentTimer) / 1000))) + ","
                        "\"progMode\":" + String(knx.progMode() ? "true" : "false") + ""
                        "}";
        server.send(200, F("application/json"), info);
      });
    server.on ( URI_WIFI, [](){
        wifiResetRequested = true;
        server.send(200);
      });
    server.on ( URI_PROGMODE, [](){
        knx.progMode(!knx.progMode());
        server.send(200);
      });
    server.on ( URI_REBOOT, [](){
        server.send(200);
        requestReboot(0);
      });
    server.on ( URI_FORMAT, [](){
        player.clean();
        server.send(SPIFFS.format()?200:500);
        server.send(200);
      });
    server.on ( URI_REMOVE, [](){
        int channel = server.arg("id").toInt();
        player.removeChannel(channel);
        server.send(200);
      });
    server.on ( URI_UPLOAD, HTTP_POST, []() {
        const __FlashStringHelper* html = F("<html>"
                        "<head>"
                          "<meta http-equiv=\"refresh\" content=\"0;url=/\">"
                        "</head>"
                      "</html>");
        server.sendHeader(F("Connection"), F("close"));
        server.send(200, F("text/html"), html);
      }, [](){
        timerWrite(watchdog, 0); //reset timer (feed watchdog)
        HTTPUpload& upload = server.upload();
        int channel = server.arg("id").toInt();
        static File file;
        static String fileName;
        if (upload.status == UPLOAD_FILE_START) {
#ifdef ENABLE_MIDI
            String lowerFileName = upload.filename;
            lowerFileName.toLowerCase();
            if (lowerFileName.endsWith(SOUNDFONT_SUFFIX)) {
                fileName = SOUNDFONT_PATH;
            }
            else {
                fileName = player.pathFromChannel(channel);
            }
#else
            fileName = player.pathFromChannel(channel);
#endif
            if (fileName) {
                SPIFFS.remove(fileName);
                file = SPIFFS.open(fileName, FILE_WRITE);
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (file) {
                if (file.write(upload.buf, upload.currentSize) != upload.currentSize) {
                    // error
                    file.close();
                }
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (file) {
#ifdef ENABLE_MIDI
                if (!fileName.endsWith(SOUNDFONT_SUFFIX)) {
#else
                {
#endif
                    player.setChannelName(channel, upload.filename, player.autodetect(channel));
                    if ((player.format(channel) == Player::UNKNOWN || player.format(channel) == Player::NO_FILE)) {
                        player.removeChannel(channel);
                    }
                    player.flushChannels();
                }
            }
            else {
                SPIFFS.remove(fileName);
            }
        } else if (upload.status == UPLOAD_FILE_ABORTED) {
            file.close();
            SPIFFS.remove(fileName);
        }
        yield();
      } );
#ifdef ENABLE_UPDATE
     server.on ( URI_UPDATE, HTTP_POST, []() {
        String html = "<html>"
                        "<head>"
                        "<title>" FW_TAG " - Update</title>" +
                        (!Update.hasError() ? "<meta http-equiv=\"refresh\" content=\"" + String(OTA_REBOOT_TIMER + 10) + "; url=/\">" : "") +
                        "</head>"
                        "<body>Update " + (Update.hasError() ? "failed" : "succeeded") + "</body>"
                    "</html>";
        server.sendHeader(F("Connection"), F("close"));
        server.send(200, F("text/html"), html);
        requestReboot();
      }, [](){
        timerWrite(watchdog, 0); //reset timer (feed watchdog)
        HTTPUpload& upload = server.upload();
        if (upload.status == UPLOAD_FILE_START) {
            if (!Update.begin(UPDATE_SIZE_UNKNOWN)) { //start with max available size
            }
        } else if (upload.status == UPLOAD_FILE_WRITE) {
            if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            }
        } else if (upload.status == UPLOAD_FILE_END) {
            if (!Update.end(true)) { //true to set the size to the current progress
            }
        }
        yield();
      } );
#endif
}

static void blink(int nb) {
    for (int i = 0; i < nb; ++i) {
        digitalWrite(PIN_PROG_LED, 1);
        delay(250);
        digitalWrite(PIN_PROG_LED, 0);
        delay(250);
    }
    delay(1000);
}

bool wifiOn = true;
static bool wifiForProgramming = false;
void setup()
{
    pinMode(PIN_PROG_LED, OUTPUT);
    blink(1);

    // Stop Bluetooth
    btStop();

    // set frequency to 80Mhz
    esp_pm_config_esp32_t pm_config;
    pm_config.max_freq_mhz = 80;
    pm_config.min_freq_mhz = 80;
    pm_config.light_sleep_enable = false;
    esp_pm_configure(&pm_config);

    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); //disable brownout detector

#ifdef ENABLE_DEBUG
    Serial.begin(115200);
    esp_log_level_set("*", ESP_LOG_INFO);
#else
    // No Debug 
    esp_log_level_set("*", ESP_LOG_NONE);
    uartSetDebug(NULL);
    ArduinoPlatform::SerialDebug = &nullDevice;
#endif

    pinMode(PIN_PROG_SWITCH, INPUT_PULLUP);
    knx.platform().knxUart(&Serial2);
    knx.ledPin(PIN_PROG_LED);
    knx.ledPinActiveOn(HIGH);
    knx.buttonPin(PIN_PROG_SWITCH);
    knx.buttonPinInterruptOn(RISING);

    // Init device
    knx.version(1);                                         // PID_VERSION
    static const uint8_t orderNumber = 0;
    knx.orderNumber(&orderNumber);                          // PID_ORDER_INFO
    // knx.manufacturerId(0xfa);                               // PID_SERIAL_NUMBER (2 first bytes) - 0xfa for KNX Association
    knx.bauNumber(0x42454c4c /* = 'BELL'*/);     // PID_SERIAL_NUMBER (4 last bytes)
    static const uint8_t hardwareType [] = { 0, 0, 0, 0, 0, 0 };
    knx.hardwareType(hardwareType);                         // PID_HARDWARE_TYPE
    knx.bau().deviceObject().induvidualAddress(1);

    // read adress table, association table, groupobject table and parameters from eeprom
    knx.readMemory();

    SPIFFS.begin(true);

    player.init(PIN_DAC, PIN_MUTE);

    if (knx.configured()) {
        uint16_t offsetGO = 1; int offsetParam = 0;
        // Wifi On/Off
        wifiOn = false;
        wifiForProgramming = false;
        knx.getGroupObject(offsetGO).dataPointType(DPT_Switch);
  //      knx.getGroupObject(offsetGO).valueNoSend(false);
        knx.getGroupObject(offsetGO + 1 /* status */).dataPointType(DPT_Switch);
        knx.getGroupObject(offsetGO).callback([offsetGO](GroupObject& go) { wifiOn = go.value(); wifiForProgramming = false; knx.getGroupObject(offsetGO + 1 /* status */).value(wifiOn); });
        offsetGO += 2;
        for (uint16_t i = 0; i < outputCount; ++i, offsetGO += Output::NBGO, offsetParam += Output::SIZEPARAMS) {
            output[i].init(offsetParam, offsetGO, outputPins[i]);
        }
        player.initKNX(offsetParam, offsetGO);
    }

    // start the framework.
    knx.start();

    watchdog = timerBegin(0, 80, true); //timer 0, div 80
    timerAttachInterrupt(watchdog, &esp_restart, true);
    timerAlarmWrite(watchdog, WATCHDOG_TIMEOUT, false); //set time in us
    timerAlarmEnable(watchdog); //enable interrupt

    WiFi.disconnect(true);  // WiFi off managed at runtime
    initWebServer();

    blink(3);
}

void loop() 
{
    timerWrite(watchdog, 0); //reset timer (feed watchdog)

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
    }
    player.loop();

    if (wifiOn) {
        if (serverState == DISCONNECTED) {
            serverState = CONNECTING;
            WiFiManager wm;
            wm.setConfigPortalTimeout(PROG_TIMEOUT / 1000);
            wm.setLoopCallback(&loop);  // main loop is called
            wm.autoConnect((FW_TAG "-" + String((uint32_t)ESP.getEfuseMac())).c_str(), "");
            serverState = WiFi.isConnected()?CONNECTED:DISCONNECTED;
        }
    }
    if (serverState == CONNECTED) {
        server.begin();
        serverState = RUNNING;
    }
    if (serverState == RUNNING) {
        if (wifiOn) {
            server.handleClient();
        }
        else {
            server.stop();
            WiFi.disconnect(true);
            serverState = DISCONNECTED;
        }
    }

    if (wifiResetRequested) {
        server.stop();
        WiFi.disconnect(true, true);
        serverState = DISCONNECTED;
        wifiResetRequested = false;
    }
    static uint32_t timerProgMode = 0;
    if (knx.progMode()) {
        if (timerProgMode == 0) {
            timerProgMode = millis();
            if (!wifiOn) {
                wifiOn = true;
                wifiForProgramming = true;
            }
        }
        else {
            if (millis() > timerProgMode + PROG_TIMEOUT) {
                knx.progMode(false);
                timerProgMode = 0;
                if (wifiForProgramming) {
                    wifiOn = false;
                }
            }
        }
    }
    else {
        timerProgMode = 0;
    }

    unsigned long currentTime = millis();
    if (rebootRequested != 0 && currentTime > rebootRequested) {
        rebootRequested = 0;
        requestReboot(0);
    }
}
