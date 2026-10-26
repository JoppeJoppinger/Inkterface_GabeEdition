// vim: foldmethod=marker:foldmarker={{{,}}}
#include <iomanip>
#include <limits>
#include <sstream>

#include <Adafruit_MAX1704X.h>
#include <Adafruit_ThinkInk.h>
#include <NimBLEDevice.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>

#include "gabe_portrait.h"
#include "gabe_cube.h"

#define SERVICE_UUID                                                                               \
    NimBLEUUID { "95c7b479-8e84-4ce7-a121-faf74bf48c84" }
#define TOPLINE_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871900" }
#define MIDLINE_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871901" }
#define BOTLINE_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871902" }
#define KEYVAL_UUID                                                                                \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871903" }
#define VECTOR_UUID                                                                                \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871904" }
#define DISPLAY_MODE_UUID                                                                         \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871905" }
#define ARTWORK_UUID                                                                               \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a871906" }
#define FLUSH_UUID                                                                                 \
    NimBLEUUID { "d6f4c07e-4a21-4c69-bd15-43a38a8719FF" }

#define SPARKBOX_HEIGHT 90
#define SPARKBOX_WIDTH 209
#define ART_MAX_WIDTH 648
#define ART_MAX_HEIGHT 480
#define ART_BUFFER_SIZE ((ART_MAX_WIDTH / 8) * ART_MAX_HEIGHT)

#define INTERFACE_VERSION "IFv01"

#define BATT_MINV 3.3f
#define BATTERY_MONITOR_PIN 35
#define SW38_PIN GPIO_NUM_38
#define NEOPIXEL_POWER_PIN GPIO_NUM_2

static constexpr unsigned long OFFLINE_GRACE_MS = 60000;
static constexpr unsigned long OFFLINE_WINDOW_MS = 30000;
RTC_DATA_ATTR static bool OFFLINE_SLEEP_ARMED = false;

#if EPD_EN == 32
static constexpr bool HAS_BATTERY_ADC = true;
#else
static constexpr bool HAS_BATTERY_ADC = false;
#endif

NimBLEServer *BLE_SERVER = nullptr;
std::string BLE_NAME = "INKTF";

Adafruit_MAX17048 maxlipo;
bool MAXLIPO_PRESENT = false;

bool INVERTED = false;
#define FG_COLOR (INVERTED ? EPD_WHITE : EPD_BLACK)
#define BG_COLOR (INVERTED ? EPD_BLACK : EPD_WHITE)

class DualPrint : public Print
{ // {{{
  public:
    DualPrint(Print &a, Print &b)
        : _a(a)
        , _b(b)
    {
    }
    size_t write(uint8_t c) override
    {
        _a.write(c);
        return _b.write(c);
    }
    size_t write(const uint8_t *buffer, size_t size) override
    {
        _a.write(buffer, size);
        return _b.write(buffer, size);
    }

  private:
    Print &_a;
    Print &_b;
}; // }}}
DualPrint Debug(Serial, Serial1);

class CustomDisp : public ThinkInk_583_Mono_AAAMFGN
{ // {{{
  public:
    CustomDisp(int16_t SID, int16_t SCLK, int16_t DC, int16_t RST, int16_t CS, int16_t SRCS,
               int16_t MISO, int16_t BUSY = -1)
        : ThinkInk_583_Mono_AAAMFGN(SID, SCLK, DC, RST, CS, SRCS, MISO, BUSY){};

    CustomDisp(int16_t DC, int16_t RST, int16_t CS, int16_t SRCS, int16_t BUSY = -1,
               SPIClass *spi = &SPI)
        : ThinkInk_583_Mono_AAAMFGN(DC, RST, CS, SRCS, BUSY, spi){};

    void updateFrame()
    {
        m_sleeping = false;
        Debug.println("full display refresh");
        display(false);
        writeRAMFramebufferToEPD(buffer2, buffer2_size, 0);
        writeRAMFramebufferToEPD(buffer2, buffer2_size, 1, true);
        powerOffKeepingRAM();
    }

    void sleepUntilUpdate()
    {
        if (!m_sleeping) {
            powerDown();
            m_sleeping = true;
        }
    }

  private:
    bool m_sleeping = true;

    void powerOffKeepingRAM()
    {
        EPD_command(UC8179_POWEROFF);
        busy_wait();
    }
}; // }}}

// ThinkInk_583_Mono_AAAMFGN MF_DISPLAY(EPD_DC, EPD_RESET, EPD_CS, SRAM_CS, EPD_BUSY);
CustomDisp MF_DISPLAY(EPD_DC, EPD_RESET, EPD_CS, -1 /* SRAM_CS */, EPD_BUSY);

static unsigned long DISP_DEBOUNCE = 0;
static bool MODE_REFRESH_PENDING = false;
static uint8_t *ART_BUFFER = nullptr;
static size_t ART_BYTES_RECEIVED = 0;
static unsigned long LAST_HOST_CONNECTED_MS = 0;
static bool OFFLINE_CUBE_SHOWN = false;
static unsigned long OFFLINE_AWAKE_MS = OFFLINE_GRACE_MS;

static const uint8_t DISPLAY_HOLD_PINS[] = {
    EPD_EN, EPD_RESET, EPD_CS, EPD_DC, SCK, MOSI, static_cast<uint8_t>(NEOPIXEL_POWER_PIN)};

void releaseDisplayHolds()
{
    for (auto pin : DISPLAY_HOLD_PINS) {
        digitalWrite(pin, pin == EPD_EN || pin == EPD_RESET || pin == EPD_CS ? HIGH : LOW);
        pinMode(pin, OUTPUT);
        const esp_err_t err = gpio_hold_dis(static_cast<gpio_num_t>(pin));
        if (err != ESP_OK) {
            Debug.printf("failed to release display GPIO %u: %s\n", pin, esp_err_to_name(err));
        }
    }
    gpio_deep_sleep_hold_dis();
}

struct Point { // {{{
    float x;
    float y;

    Point()
        : x(0)
        , y(0)
    {
    }
    Point(float _x, float _y)
        : x(_x)
        , y(_y)
    {
    }
}; // }}}

struct Points { // {{{
    float yMin;
    float yMax;
    std::vector<Point> points;

    Points()
        : yMin(0)
        , yMax(0)
        , points()
    {
    }

    void clear()
    {
        yMin = 0;
        yMax = 0;
        points.clear();
    }
}; // }}}

struct KeyVal { // {{{
    std::string key;
    std::string val;

    KeyVal()
        : key{""}
        , val{""}
    {
    }
}; // }}}
typedef std::vector<KeyVal> KeyVals;

struct State { // {{{
    bool connected = false;
    uint8_t displayMode = 0;
    bool artMode = false;
    uint16_t artWidth = 0;
    uint16_t artHeight = 0;
    std::string topLine{"Starting up..."};
    std::string midLine{"No User"};
    std::string botLine{"No Activity"};
    std::string hostMsg{""};
    std::string battLine{""};

    KeyVals keyvals{12};
    std::vector<Points> sparks{9};

    void reset()
    {
        keyvals.clear();
        keyvals.resize(12);
        sparks.clear();
        sparks.resize(9);

        uint32_t addr = (uint64_t)NimBLEDevice::getAddress() & 0xFFFFFF;
        std::stringstream name;
        name << "INKTF-";
        name << std::uppercase << std::hex << std::setfill('0') << std::setw(6) << addr;
        BLE_NAME = name.str();

        connected = false;
        displayMode = 0;
        artMode = false;
        artWidth = 0;
        artHeight = 0;
        topLine = "Waiting on connection...";
        midLine = BLE_NAME;
        botLine = "";
        hostMsg = "";
        keyvals[0].key = "OS";
        keyvals[0].val = "--";
        keyvals[1].key = "BIOS";
        keyvals[1].val = "--";
        keyvals[2].key = "STEAM";
        keyvals[2].val = "--";
        keyvals[3].key = "CPU";
        keyvals[3].val = "-- dC";
        keyvals[4].key = "GPU";
        keyvals[4].val = "-- dC";
        keyvals[5].key = "FAN";
        keyvals[5].val = "-- RPM";
        keyvals[6].key = "CPU";
        keyvals[6].val = "--%";
        keyvals[7].key = "GPU";
        keyvals[7].val = "--%";
        keyvals[8].key = "MEM";
        keyvals[8].val = "--%";
        keyvals[9].key = "DISK";
        keyvals[9].val = "--%";
        keyvals[10].key = "NET";
        keyvals[10].val = "-- MB/s";
        keyvals[11].key = "LOAD";
        keyvals[11].val = "--%";
    }
} STATE; // }}}

void drawStatic();

class ServerCallbacks : public NimBLEServerCallbacks
{ // {{{
    void onConnect(NimBLEServer *server, NimBLEConnInfo &conn) override
    {
        Debug.println("got connection");
        // we don't want any other devices to see us once we are connected
        // to a host
        NimBLEDevice::stopAdvertising();
        STATE.connected = true;
        LAST_HOST_CONNECTED_MS = millis();
        OFFLINE_CUBE_SHOWN = false;
        OFFLINE_AWAKE_MS = OFFLINE_GRACE_MS;
    }

    void onDisconnect(NimBLEServer *server, NimBLEConnInfo &conn, int reason) override
    {
        Debug.print("got disconnect event, connected count: ");
        Debug.println(server->getConnectedCount());
        if (server->getConnectedCount() == 0) {
            STATE.connected = false;
            LAST_HOST_CONNECTED_MS = millis();
            OFFLINE_AWAKE_MS = OFFLINE_GRACE_MS;
        }
        NimBLEDevice::startAdvertising();
    }
} SERVER_CALLBACKS; // }}}

class StatusLineCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        auto uuid = characteristic->getUUID();
        if (uuid == TOPLINE_UUID && STATE.topLine != value) {
            STATE.topLine = value;
        } else if (uuid == MIDLINE_UUID && STATE.midLine != value) {
            STATE.midLine = value;
        } else if (uuid == BOTLINE_UUID && STATE.botLine != value) {
            STATE.botLine = value;
        } else if (uuid != TOPLINE_UUID && uuid != MIDLINE_UUID && uuid != BOTLINE_UUID) {
            Debug.print("Got value (");
            Debug.print(value.c_str());
            Debug.print(") for unknown UUID (");
            Debug.print(uuid.toString().c_str());
            Debug.println("), ignoring.");
            return;
        }
    }
} STATUS_CALLBACKS; // }}}

class KeyValCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    typedef struct __attribute__((packed)) {
        uint8_t index;
        char key[32];
        char val[32];
    } Msg;

    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        Msg msg;
        if (value.length() == sizeof(Msg)) {
            memcpy(&msg, value.data(), sizeof(Msg));
            if (msg.index < STATE.keyvals.size()) {
                STATE.keyvals[msg.index].key = msg.key;
                STATE.keyvals[msg.index].val = msg.val;
            } else {
                Debug.println("got keyval index out of range");
            }
        } else {
            Debug.print("got bad keyval write, size: ");
            Debug.println(value.length());
        }
    }
} KEYVAL_CALLBACKS; // }}}

class VectorCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    typedef struct __attribute__((packed)) {
        uint8_t index;
        uint8_t count;
        float minVal;
        float maxVal;
        uint8_t values[32 * 2]; // 32 (x, y) pairs, 64 bytes
        // total 74 bytes
    } Msg;

    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        Msg msg{};
        if (value.length() >= 10) {
            memcpy(&msg, value.data(), std::min(value.length(), sizeof(Msg)));
            if (msg.index >= STATE.sparks.size() || msg.count > sizeof(msg.values) ||
                msg.count % 2 != 0 || value.length() < 10 + msg.count) {
                Debug.println("got invalid vector");
                return;
            }
            Debug.print("got vector for index (");
            Debug.print(msg.index);
            Debug.print(") with ");
            Debug.print(msg.count);
            Debug.print(" values, min ");
            Debug.print(msg.minVal);
            Debug.print(", max ");
            Debug.println(msg.maxVal);
            STATE.sparks[msg.index].clear();
            STATE.sparks[msg.index].yMin = msg.minVal;
            STATE.sparks[msg.index].yMax = msg.maxVal;
            for (int i = 0; i < msg.count; i += 2) {
                STATE.sparks[msg.index].points.emplace_back(msg.values[i] / 255.0,
                                                            msg.values[i + 1] / 255.0);
            }
        } else {
            Debug.print("got bad vectors write, size: ");
            Debug.println(value.length());
        }
    }
} VECTOR_CALLBACKS; // }}}

class DisplayModeCallbacks : public NimBLECharacteristicCallbacks
{
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        std::string value = characteristic->getValue();
        if (value.size() == 1 && static_cast<uint8_t>(value[0]) <= 2) {
            const uint8_t mode = static_cast<uint8_t>(value[0]);
            if (STATE.displayMode != mode) {
                STATE.displayMode = mode;
                MODE_REFRESH_PENDING = true;
                if (!STATE.artMode) {
                    DISP_DEBOUNCE = 100;
                }
            }
        } else {
            Debug.println("got invalid display mode");
        }
    }
} DISPLAY_MODE_CALLBACKS;

class ArtworkCallbacks : public NimBLECharacteristicCallbacks
{
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        const std::string value = characteristic->getValue();
        if (value.empty()) {
            return;
        }
        const auto *data = reinterpret_cast<const uint8_t *>(value.data());
        switch (data[0]) {
        case 0x00: {
            if (value.size() < 5) {
                Debug.println("got short artwork begin");
                return;
            }
            const uint16_t width = data[1] | (data[2] << 8);
            const uint16_t height = data[3] | (data[4] << 8);
            if (width == 0 || height == 0 || width > ART_MAX_WIDTH ||
                height > ART_MAX_HEIGHT || (width + 7) / 8 * height > ART_BUFFER_SIZE) {
                Debug.println("rejected invalid artwork dimensions");
                return;
            }
            if (!ART_BUFFER) {
                ART_BUFFER = static_cast<uint8_t *>(malloc(ART_BUFFER_SIZE));
            }
            if (!ART_BUFFER) {
                Debug.println("no memory for artwork frame");
                return;
            }
            STATE.artWidth = width;
            STATE.artHeight = height;
            ART_BYTES_RECEIVED = 0;
            memset(ART_BUFFER, 0, ART_BUFFER_SIZE);
            break;
        }
        case 0x01: {
            if (value.size() < 6 || !ART_BUFFER || !STATE.artWidth || !STATE.artHeight) {
                Debug.println("got invalid artwork data");
                return;
            }
            const uint32_t offset = data[1] | (data[2] << 8) | (data[3] << 16) |
                                    (static_cast<uint32_t>(data[4]) << 24);
            const size_t length = value.size() - 5;
            const size_t frameSize = (STATE.artWidth + 7) / 8 * STATE.artHeight;
            if (offset != ART_BYTES_RECEIVED || offset > frameSize ||
                length > frameSize - offset) {
                Debug.println("rejected nonsequential or out-of-frame artwork data");
                return;
            }
            memcpy(ART_BUFFER + offset, data + 5, length);
            ART_BYTES_RECEIVED += length;
            break;
        }
        case 0x02: {
            const size_t frameSize = (STATE.artWidth + 7) / 8 * STATE.artHeight;
            if (!STATE.artWidth || !STATE.artHeight || ART_BYTES_RECEIVED < frameSize) {
                Debug.println("ignored incomplete artwork frame");
                return;
            }
            STATE.artMode = true;
            DISP_DEBOUNCE = 100;
            break;
        }
        case 0x03:
            if (STATE.artMode) {
                STATE.artMode = false;
                DISP_DEBOUNCE = 100;
            }
            break;
        default:
            Debug.println("got unknown artwork opcode");
            break;
        }
    }
} ARTWORK_CALLBACKS;

class FlushCallbacks : public NimBLECharacteristicCallbacks
{ // {{{
    void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &conn) override
    {
        if (STATE.displayMode == 2 && !MODE_REFRESH_PENDING) {
            return;
        }
        STATE.hostMsg = characteristic->getValue();
        DISP_DEBOUNCE = 100;
    }
} FLUSH_CALLBACKS; // }}}

void enterOfflineSleep(bool force = false)
{
    if (!force && (STATE.connected || BLE_SERVER->getConnectedCount() > 0)) {
        return;
    }
    if (!rtc_gpio_is_valid_gpio(SW38_PIN)) {
        Debug.println("GPIO38 cannot wake from deep sleep; staying awake");
        LAST_HOST_CONNECTED_MS = millis();
        return;
    }
    const esp_err_t wakeError = esp_sleep_enable_ext0_wakeup(SW38_PIN, 0);
    if (wakeError != ESP_OK) {
        Debug.printf("failed to configure SW38 wakeup: %s; staying awake\n",
                     esp_err_to_name(wakeError));
        LAST_HOST_CONNECTED_MS = millis();
        return;
    }
    auto *advertising = NimBLEDevice::getAdvertising();
    if (advertising && advertising->isAdvertising() && !advertising->stop()) {
        Debug.println("failed to stop BLE advertising; delaying offline sleep");
        LAST_HOST_CONNECTED_MS = millis();
        return;
    }
    if (!force && (STATE.connected || BLE_SERVER->getConnectedCount() > 0)) {
        return;
    }

    MF_DISPLAY.sleepUntilUpdate();
    if (!force && (STATE.connected || BLE_SERVER->getConnectedCount() > 0)) {
        return;
    }
    pinMode(static_cast<uint8_t>(NEOPIXEL_POWER_PIN), OUTPUT);
    digitalWrite(static_cast<uint8_t>(NEOPIXEL_POWER_PIN), LOW);
    digitalWrite(EPD_DC, LOW);
    digitalWrite(SCK, LOW);
    digitalWrite(MOSI, LOW);
    for (auto pin : DISPLAY_HOLD_PINS) {
        const esp_err_t err = gpio_hold_en(static_cast<gpio_num_t>(pin));
        if (err != ESP_OK) {
            Debug.printf("failed to hold display GPIO %u: %s; restarting\n",
                         pin, esp_err_to_name(err));
            ESP.restart();
        }
    }
    if (!NimBLEDevice::deinit()) {
        Debug.println("failed to shut down BLE for offline sleep; restarting");
        ESP.restart();
    }
    gpio_deep_sleep_hold_en();
    OFFLINE_SLEEP_ARMED = true;
    Debug.println("offline deep sleep until SW38 press; BLE window: 30 seconds");
    Serial.flush();
    Serial1.flush();
    esp_deep_sleep_start();
}

void setup()
{ // {{{
    Serial.begin(115200);
    Serial1.begin(115200);
    // these usb serial and rx/tx interfaces are combined in the Debug instance
    const bool sw38Wake = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0;
    const bool offlineWake = sw38Wake && OFFLINE_SLEEP_ARMED;
    OFFLINE_SLEEP_ARMED = false;
    releaseDisplayHolds();
    if (sw38Wake) {
        const esp_err_t err = rtc_gpio_deinit(SW38_PIN);
        if (err != ESP_OK) {
            Debug.printf("failed to restore SW38 as GPIO: %s\n", esp_err_to_name(err));
        }
    }
    pinMode(static_cast<uint8_t>(NEOPIXEL_POWER_PIN), OUTPUT);
    digitalWrite(static_cast<uint8_t>(NEOPIXEL_POWER_PIN), HIGH);
    pinMode(static_cast<uint8_t>(SW38_PIN), INPUT);
    if (HAS_BATTERY_ADC) {
        analogReadResolution(12);
        analogSetPinAttenuation(BATTERY_MONITOR_PIN, ADC_11db);
    }

#if defined(STARTUP_DELAY_MS)
    delay(STARTUP_DELAY_MS);
#endif

    // TODO: add support for more battery tracking on different boards, etc.
    Debug.println("setting up i2c interface");
    MAXLIPO_PRESENT = maxlipo.begin();
    if (!MAXLIPO_PRESENT) {
        Debug.println("failed to setup MAX17048, not present on all boards!");
    }

    Debug.println("setting up ble device and service");
    NimBLEDevice::init("");
    NimBLEDevice::setPower(2); // we don't need much power
    NimBLEDevice::setMTU(256); // bump the mtu to fit a decent number of points
    BLE_SERVER = NimBLEDevice::createServer();
    BLE_SERVER->setCallbacks(&SERVER_CALLBACKS);
    BLEService *service = BLE_SERVER->createService(SERVICE_UUID);
    BLECharacteristic *characteristic = nullptr;

    // status line characteristics, they share callbacks
    characteristic =
        service->createCharacteristic(TOPLINE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    characteristic->setValue(STATE.topLine.c_str());
    characteristic->setCallbacks(&STATUS_CALLBACKS);
    characteristic =
        service->createCharacteristic(MIDLINE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    characteristic->setValue(STATE.midLine.c_str());
    characteristic->setCallbacks(&STATUS_CALLBACKS);
    characteristic =
        service->createCharacteristic(BOTLINE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE);
    characteristic->setValue(STATE.botLine.c_str());
    characteristic->setCallbacks(&STATUS_CALLBACKS);

    characteristic = service->createCharacteristic(KEYVAL_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&KEYVAL_CALLBACKS);
    characteristic = service->createCharacteristic(VECTOR_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&VECTOR_CALLBACKS);
    characteristic = service->createCharacteristic(DISPLAY_MODE_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&DISPLAY_MODE_CALLBACKS);

    characteristic = service->createCharacteristic(ARTWORK_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&ARTWORK_CALLBACKS);

    characteristic = service->createCharacteristic(FLUSH_UUID, NIMBLE_PROPERTY::WRITE);
    characteristic->setCallbacks(&FLUSH_CALLBACKS);

    BLE_SERVER->start();

    Debug.println("initializing display");
    pinMode(EPD_EN, OUTPUT);
    digitalWrite(EPD_EN, HIGH);
    STATE.reset();
    LAST_HOST_CONNECTED_MS = millis();
    MF_DISPLAY.begin(THINKINK_MONO);
    if (offlineWake) {
        STATE.displayMode = 2;
        OFFLINE_CUBE_SHOWN = true;
        OFFLINE_AWAKE_MS = OFFLINE_WINDOW_MS;
        Debug.println("SW38 wake: retaining GabeCube without refresh");
    } else {
        MF_DISPLAY.clearBuffer();
        MF_DISPLAY.fillScreen(BG_COLOR);
        drawStatic();
        MF_DISPLAY.updateFrame();
    }

    Debug.println("starting ble advert");
    uint32_t addr = (uint64_t)NimBLEDevice::getAddress() & 0xFFFFFF;
    std::stringstream name;
    name << "INKTF-";
    name << std::uppercase << std::hex << std::setfill('0') << std::setw(6) << addr;
    BLE_NAME = name.str();
    BLEAdvertising *advert = NimBLEDevice::getAdvertising();
    BLEAdvertisementData ad_data{};
    ad_data.setName(BLE_NAME);
    ad_data.setManufacturerData("\x5d\x05" INTERFACE_VERSION);
    advert->setAdvertisementData(ad_data);
    advert->addServiceUUID(SERVICE_UUID);
    advert->enableScanResponse(false);
    NimBLEDevice::startAdvertising();
    LAST_HOST_CONNECTED_MS = millis();
    setCpuFrequencyMhz(80);
    Debug.printf("CPU frequency set to %u MHz\n", getCpuFrequencyMhz());

} // }}}

void loop()
{ // {{{
    static unsigned long LAST_MS = 0;
    static unsigned long CONN_DEBOUNCE = 5000;
    static unsigned long BATT_DEBOUNCE = 60000;
    static float last_battv = 0;
    static bool batterySampled = false;

    auto now = millis();
    auto delta = now - LAST_MS;
    if (now < LAST_MS) {
        // handling rollover
        Debug.println("handling time rollover");
        delta = (std::numeric_limits<unsigned long>::max() - LAST_MS) + now;
    }

    if (CONN_DEBOUNCE > 0 && CONN_DEBOUNCE > delta) {
        CONN_DEBOUNCE -= delta;
    } else if (CONN_DEBOUNCE > 0) {
        // NOTE: this should be handled in our SERVER_CALLBACKS but we don't
        //       always seem to get the onDisconnect() callback and get stuck
        //       with advertising stopped, so might as well just check here...
        bool advertising = NimBLEDevice::getAdvertising()->isAdvertising();
        uint8_t connections = BLE_SERVER->getConnectedCount();
        if (STATE.connected != (connections > 0)) {
            STATE.connected = connections > 0;
            LAST_HOST_CONNECTED_MS = now;
            OFFLINE_AWAKE_MS = OFFLINE_GRACE_MS;
            if (STATE.connected) {
                OFFLINE_CUBE_SHOWN = false;
            }
            Debug.println("reconciled BLE connection state");
        }
        if (!advertising && connections == 0) {
            Debug.print("starting advertisement, we have no connections");
            NimBLEDevice::startAdvertising();
        } else if (advertising && connections > 0) {
            Debug.print("stopping advertisement, we have connections");
            NimBLEDevice::stopAdvertising();
        }
        CONN_DEBOUNCE = 5000;
    }

    if ((MAXLIPO_PRESENT || HAS_BATTERY_ADC) && batterySampled && BATT_DEBOUNCE > delta) {
        BATT_DEBOUNCE -= delta;
    } else if (MAXLIPO_PRESENT || HAS_BATTERY_ADC) {
        float battv = 0;
        if (MAXLIPO_PRESENT) {
            battv = maxlipo.cellVoltage();
        } else {
            uint32_t millivolts = 0;
            for (uint8_t sample = 0; sample < 8; ++sample) {
                millivolts += analogReadMilliVolts(BATTERY_MONITOR_PIN);
                delay(2);
            }
            battv = (millivolts / 8.0f) * 0.002f;
        }
        batterySampled = true;
        if (abs(battv - last_battv) > 0.02f) {
            last_battv = battv;
            Debug.printf("battery voltage: %.2f V%s\n", battv,
                         MAXLIPO_PRESENT ? " (MAX17048)" : " (GPIO35)");
            std::stringstream line;
            line << std::fixed << std::setprecision(2) << battv << " V";
            STATE.battLine = line.str();
            if (STATE.displayMode != 2 && !STATE.artMode) {
                DISP_DEBOUNCE = 10;
            }
        }
        BATT_DEBOUNCE = 60000;
    }

    if ((MAXLIPO_PRESENT || HAS_BATTERY_ADC) && batterySampled && last_battv < BATT_MINV) {
        Debug.println("drawing low battery message");
        DISP_DEBOUNCE = 0;
        MF_DISPLAY.begin(THINKINK_MONO);
        MF_DISPLAY.clearBuffer();
        MF_DISPLAY.fillScreen(BG_COLOR);
        drawLowBatt();
        MF_DISPLAY.updateFrame();
        enterOfflineSleep(true);
    }

    if (!STATE.connected && !OFFLINE_CUBE_SHOWN &&
        static_cast<unsigned long>(now - LAST_HOST_CONNECTED_MS) >= OFFLINE_AWAKE_MS) {
        if (STATE.displayMode != 2 || STATE.artMode) {
            STATE.displayMode = 2;
            STATE.artMode = false;
            DISP_DEBOUNCE = 10;
        }
        OFFLINE_CUBE_SHOWN = true;
    }

    if (DISP_DEBOUNCE > 0 && DISP_DEBOUNCE > delta) {
        DISP_DEBOUNCE -= delta;
    } else if (DISP_DEBOUNCE > 0) {
        Debug.println("drawing to display");
        DISP_DEBOUNCE = 0;
        MF_DISPLAY.clearBuffer();
        MF_DISPLAY.fillScreen(BG_COLOR);
        if (STATE.artMode && ART_BUFFER) {
            MF_DISPLAY.drawBitmap(0, 0, ART_BUFFER, STATE.artWidth, STATE.artHeight, FG_COLOR);
        } else {
            drawStatic();
        }
        MF_DISPLAY.updateFrame();
        MODE_REFRESH_PENDING = false;
        Debug.println("drew to display");
    }

    if (!STATE.connected && OFFLINE_CUBE_SHOWN && DISP_DEBOUNCE == 0 &&
        STATE.displayMode == 2 && !STATE.artMode &&
        static_cast<unsigned long>(now - LAST_HOST_CONNECTED_MS) >= OFFLINE_AWAKE_MS) {
        enterOfflineSleep();
    }

    LAST_MS = now;
    delay(10);
} // }}}

void drawText(const char *text, const int16_t &x = -1, const int16_t &y = -1,
              const uint8_t &size = 1, const bool &wrap = false)
{ // {{{
    if (x >= 0 && y >= 0) {
        MF_DISPLAY.setCursor(x, y);
    }
    MF_DISPLAY.setTextSize(size);
    MF_DISPLAY.setTextColor(FG_COLOR);
    MF_DISPLAY.setTextWrap(wrap);
    MF_DISPLAY.print(text);
} // }}}

void drawLogo(int16_t &x, const int16_t &y = 0)
{ // {{{
    MF_DISPLAY.fillRoundRect(x, y, 101, 101, 3, FG_COLOR);
    MF_DISPLAY.fillCircle(x + 50, y + 50, 31, BG_COLOR);
    MF_DISPLAY.fillCircle(x + 50, y + 50, 23, FG_COLOR);
    x += 101;
} // }}}

void drawSparkbox(int16_t &x, const int16_t &y, std::string &title, const std::string &value,
                  const Points &points)
{ // {{{
    const int16_t w = SPARKBOX_WIDTH;
    const int16_t h = SPARKBOX_HEIGHT;
    const int16_t hpad = 8;
    const int16_t vpad = 6;
    const int16_t title_h = 26;
    const int16_t graph_h = (h - title_h) - 32;
    const int16_t graph_w = w - 20;
    const int16_t graph_x = x + 10;
    const int16_t graph_y = (y + h) - 16;

    if (!title.empty()) {
        MF_DISPLAY.drawRoundRect(x, y, w, h, 4, FG_COLOR);
        MF_DISPLAY.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, FG_COLOR);
        MF_DISPLAY.fillRect(x, y + title_h, w, 1, FG_COLOR);
        drawText(title.c_str(), x + hpad, y + vpad, 2);
        drawText(value.c_str(), (x + (w - hpad)) - (12 * strlen(value.c_str())), y + vpad, 2);

        std::stringstream maxstrm;
        maxstrm << std::fixed << std::setprecision(0) << points.yMax;
        auto maxstr = maxstrm.str();
        drawText(maxstr.c_str(), x + hpad, y + title_h + vpad);

        std::stringstream minstrm;
        minstrm << std::fixed << std::setprecision(0) << points.yMin;
        auto minstr = minstrm.str();
        drawText(minstr.c_str(), x + hpad, y + h - (vpad + 7));

        if (points.points.size() >= 2) {
            int16_t s_x = 0.0, s_y = 0.0, e_x = 0.0, e_y = 0.0;
            for (auto p = points.points.cbegin(); p != points.points.cend() - 1; ++p) {
                s_x = graph_x + (p->x * graph_w);
                e_x = graph_x + ((p + 1)->x * graph_w);
                s_y = graph_y + (p->y * graph_h * -1.0);
                e_y = graph_y + ((p + 1)->y * graph_h * -1.0);
                MF_DISPLAY.drawLine(s_x, s_y, e_x, e_y, FG_COLOR);
                MF_DISPLAY.drawLine(s_x, s_y - 1, e_x, e_y - 1, FG_COLOR);
                MF_DISPLAY.drawLine(s_x, s_y + 1, e_x, e_y + 1, FG_COLOR);
                MF_DISPLAY.drawLine(s_x - 1, s_y, e_x - 1, e_y, FG_COLOR);
                MF_DISPLAY.drawLine(s_x + 1, s_y, e_x + 1, e_y, FG_COLOR);
            }
        }
    }

    x += w;
} // }}}

void drawDiscreteBox(int16_t &x, const int16_t &y, const std::string &title,
                     const std::string &value)
{ // {{{
    const int16_t w = 209;
    const int16_t h = 26;
    const int16_t hpad = 8;
    const int16_t vpad = 6;

    if (!title.empty()) {
        MF_DISPLAY.drawRoundRect(x, y, w, h, 4, FG_COLOR);
        MF_DISPLAY.drawRoundRect(x + 1, y + 1, w - 2, h - 2, 4, FG_COLOR);
        drawText(title.c_str(), x + hpad, y + vpad, 2);
        drawText(value.c_str(), (x + (w - hpad)) - (12 * strlen(value.c_str())), y + vpad, 2);
    }

    x += w;
} // }}}

void drawPortrait()
{
    static uint8_t lastPortrait = GABE_PORTRAIT_COUNT;
    uint8_t portrait;
    do {
        portrait = esp_random() % GABE_PORTRAIT_COUNT;
    } while (portrait == lastPortrait);
    lastPortrait = portrait;

    MF_DISPLAY.drawXBitmap(0, 0, GABE_PORTRAITS[portrait], GABE_PORTRAIT_WIDTH,
                           GABE_PORTRAIT_HEIGHT, FG_COLOR);
}

void drawGabeCube()
{
    MF_DISPLAY.drawXBitmap(0, 0, GABE_CUBE_BITS, GABE_CUBE_WIDTH,
                           GABE_CUBE_HEIGHT, FG_COLOR);
}

void drawStatistics()
{ // {{{
    int16_t x = 0;
    int16_t y = 0;

    // fremont logo in top left corner
    x = 5;
    y = 5;
    drawLogo(x, y);

    // show connected fremont hostname/serial or connecting status
    x = 120;
    y = 15;
    drawText(STATE.topLine.c_str(), x, y, 3);
    y += 35;
    drawText(STATE.midLine.c_str(), x, y, 2);
    y += 30;
    drawText(STATE.botLine.c_str(), x, y, 2);

    // first row of boxes with no sparklines
    x = 5;
    y = 115;
    drawDiscreteBox(x, y, STATE.keyvals[0].key, STATE.keyvals[0].val);
    x += 5;
    drawDiscreteBox(x, y, STATE.keyvals[1].key, STATE.keyvals[1].val);
    x += 5;
    drawDiscreteBox(x, y, STATE.keyvals[2].key, STATE.keyvals[2].val);

    // second row
    x = 5;
    y += 26 + 5;
    drawSparkbox(x, y, STATE.keyvals[3].key, STATE.keyvals[3].val, STATE.sparks[0]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[4].key, STATE.keyvals[4].val, STATE.sparks[1]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[5].key, STATE.keyvals[5].val, STATE.sparks[2]);

    // third row
    x = 5;
    y += SPARKBOX_HEIGHT + 5;
    drawSparkbox(x, y, STATE.keyvals[6].key, STATE.keyvals[6].val, STATE.sparks[3]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[7].key, STATE.keyvals[7].val, STATE.sparks[4]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[8].key, STATE.keyvals[8].val, STATE.sparks[5]);

    x = 5;
    y += SPARKBOX_HEIGHT + 5;
    drawSparkbox(x, y, STATE.keyvals[9].key, STATE.keyvals[9].val, STATE.sparks[6]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[10].key, STATE.keyvals[10].val, STATE.sparks[7]);
    x += 5;
    drawSparkbox(x, y, STATE.keyvals[11].key, STATE.keyvals[11].val, STATE.sparks[8]);

    // battery state
    x = 5;
    y = MF_DISPLAY.height() - 24;
    drawText(STATE.battLine.c_str(), x, y);

    // version tag
    std::stringstream tag;
    tag << BLE_NAME << " " << GIT_REVISION << " " << INTERFACE_VERSION;
    x = 5;
    y = MF_DISPLAY.height() - 12;
    drawText(tag.str().c_str(), x, y);

    // host message if provided (usually a timestamp)
    x = MF_DISPLAY.width() - (6 * strlen(STATE.hostMsg.c_str())) - 5;
    drawText(STATE.hostMsg.c_str(), x, y);
} // }}}

void drawStatic()
{
    if (STATE.displayMode == 1) {
        drawPortrait();
    } else if (STATE.displayMode == 2) {
        drawGabeCube();
    } else {
        drawStatistics();
    }
}

void drawLowBatt()
{ // {{{
    int16_t x = 0;
    int16_t y = 0;

    x = 5;
    y = 15;
    drawText("Please charge the battery!", x, y, 3);
    y += 35;
    drawText("Once charged, press reset on the back!", x, y, 2);

    // battery state
    x = 5;
    y = MF_DISPLAY.height() - 24;
    drawText(STATE.battLine.c_str(), x, y);

    // version tag
    std::stringstream tag;
    tag << BLE_NAME << " " << GIT_REVISION << " " << INTERFACE_VERSION;
    x = 5;
    y = MF_DISPLAY.height() - 12;
    drawText(tag.str().c_str(), x, y);

    // host message if provided (usually a timestamp)
    x = MF_DISPLAY.width() - (6 * strlen(STATE.hostMsg.c_str())) - 5;
    drawText(STATE.hostMsg.c_str(), x, y);
} // }}}
