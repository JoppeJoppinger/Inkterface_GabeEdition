#include "panel.hpp"

#include <QByteArray>
#include <QCoreApplication>
#include <QDateTime>
#include <QDataStream>
#include <QDebug>
#include <QObject>
#include <QSettings>
#include <QUuid>

#define SERVICE_UUID                                                                               \
    QUuid { "95c7b479-8e84-4ce7-a121-faf74bf48c84" }
#define TOPLINE_UUID                                                                               \
    QUuid { "d6f4c07e-4a21-4c69-bd15-43a38a871900" }
#define MIDLINE_UUID                                                                               \
    QUuid { "d6f4c07e-4a21-4c69-bd15-43a38a871901" }
#define BOTLINE_UUID                                                                               \
    QUuid { "d6f4c07e-4a21-4c69-bd15-43a38a871902" }
#define KEYVAL_UUID                                                                                \
    QUuid { "d6f4c07e-4a21-4c69-bd15-43a38a871903" }
#define VECTOR_UUID                                                                                \
    QUuid { "d6f4c07e-4a21-4c69-bd15-43a38a871904" }
#define DISPLAY_MODE_UUID                                                                         \
    QUuid { "d6f4c07e-4a21-4c69-bd15-43a38a871905" }
#define ARTWORK_UUID                                                                               \
    QUuid { "d6f4c07e-4a21-4c69-bd15-43a38a871906" }
#define FLUSH_UUID                                                                                 \
    QUuid { "d6f4c07e-4a21-4c69-bd15-43a38a8719FF" }

#define CONN_INTERVAL 2000
#define SEND_INTERVAL 30000

// durations that can trigger connection re-evaluation (in seconds)
#define CONN_WARNING std::chrono::duration<double>(45)
#define CONN_LOST std::chrono::duration<double>(60)

using namespace Qt::Literals::StringLiterals;

Panel::Panel(QObject *parent)
    : QObject(parent)
    , m_finder(new PanelFinder(this))
    , m_state(new PanelState(this))
    , m_connTimer(new QTimer(this))
    , m_sendTimer(new QTimer(this))
    , m_modeTimer(new QTimer(this))
{
    connect(m_finder, &PanelFinder::panelsChanged, this, &Panel::connCheck);

    m_connTimer->setSingleShot(false);
    m_connTimer->setInterval(CONN_INTERVAL);
    connect(m_connTimer, &QTimer::timeout, this, &Panel::connCheck);
    m_connTimer->start();

    m_sendTimer->setSingleShot(false);
    m_sendTimer->setInterval(SEND_INTERVAL);
    connect(m_sendTimer, &QTimer::timeout, this, &Panel::sendState);
    m_sendTimer->start();

    m_modeTimer->setSingleShot(false);
    m_modeTimer->setInterval(500);
    connect(m_modeTimer, &QTimer::timeout, this, &Panel::checkDisplayMode);
    m_modeTimer->start();

    connect(m_state, &PanelState::activityChanged, this, &Panel::checkDisplayMode);
    connect(m_state, &PanelState::artworkFrameReady, this, &Panel::onArtworkFrameReady);
}

void Panel::stop()
{
    m_stopping = true;
    if (m_controller) {
        m_controller->disconnectFromDevice();
    }
    QTimer::singleShot(1000, this, [&]() { QCoreApplication::quit(); });
}

void Panel::onControllerStateChanged(QLowEnergyController::ControllerState state)
{
    qDebug() << "New controller state: " << state;
    if (!isConnected()) {
        qDebug() << "lost connection, clearing...";
        clearConnection();
        m_finder->startDiscovery();
        return;
    }
    if (state == QLowEnergyController::ConnectedState) {
        qDebug() << "connected, discovering services...";
        m_connecting = false;
        m_controller->discoverServices();
    }
}

void Panel::onControllerServicesDiscovered()
{
    if (!m_controller || m_controller->state() == QLowEnergyController::UnconnectedState) {
        return;
    }
    auto services = m_controller->services();
    qDebug() << "Discovered " << services.count() << " services!";
    if (m_service) {
        return;
    }
    m_service = m_controller->createServiceObject(SERVICE_UUID, this);
    if (!m_service) {
        qDebug() << "Failed to create service!";
        clearConnection();
        m_finder->startDiscovery();
        return;
    }
    connect(m_service, &QLowEnergyService::stateChanged, this, &Panel::onServiceStateChanged);
    connect(m_service, &QLowEnergyService::errorOccurred, this, &Panel::onServiceError);
    connect(m_service, &QLowEnergyService::characteristicWritten, this,
            &Panel::onServiceCharacteristicWritten);
    m_service->discoverDetails();
    m_lastComms = std::chrono::steady_clock::now();
}

void Panel::onControllerError(QLowEnergyController::Error error)
{
    qDebug() << "controller error:" << error;
    clearConnection();
    m_finder->startDiscovery();
}

void Panel::onServiceStateChanged(QLowEnergyService::ServiceState state)
{
    qDebug() << "New service state: " << state;
    if (state != QLowEnergyService::RemoteServiceDiscovered) {
        return;
    }
    qDebug() << "Found " << m_service->characteristics().count() << "characteristics!";
    m_artworkSupported = m_service->characteristic(ARTWORK_UUID).isValid();
    if (!m_artworkSupported) {
        qWarning() << "Connected panel firmware lacks Now Playing support; update its firmware";
    }
    m_lastDisplayMode = -1;
    m_lastEffectiveMode = -1;
    sendArtworkClear();
    checkDisplayMode();
    m_sendTimer->start(250);
    m_lastComms = std::chrono::steady_clock::now();
}

void Panel::onServiceError(QLowEnergyService::ServiceError error)
{
    qDebug() << "service error:" << error;
    clearConnection();
    m_finder->startDiscovery();
}

void Panel::onServiceCharacteristicWritten(
    [[maybe_unused]] const QLowEnergyCharacteristic &characteristic,
    [[maybe_unused]] const QByteArray &value)
{
    m_lastComms = std::chrono::steady_clock::now();
    if (characteristic.uuid().toString() == ARTWORK_UUID.toString()) {
        const bool frameCompleted =
            m_artQueue.size() == 1 && !m_artQueue.first().isEmpty() &&
            static_cast<uint8_t>(m_artQueue.first().at(0)) == 0x02;
        if (!m_artQueue.isEmpty()) {
            m_artQueue.removeFirst();
        }
        m_artSending = false;
        if (!m_artQueue.isEmpty()) {
            sendArtwork();
        } else if (m_artClearPending) {
            sendArtworkClear();
        } else if (m_artResendPending) {
            m_artResendPending = false;
            queueArtworkFrame();
        } else if (frameCompleted && m_lastDisplayMode == 3 && m_lastEffectiveMode == 3 &&
                   m_autoShowArtwork) {
            qDebug() << "Now Playing artwork transfer completed";
            m_nextAutomaticSwitch = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        }
    }
}

void Panel::onArtworkFrameReady(const QByteArray &bits, quint16 width, quint16 height)
{
    m_nowPlayingBits = bits;
    m_nowPlayingWidth = width;
    m_nowPlayingHeight = height;
    const bool transitioned = syncDisplayMode();
    if (!transitioned && m_lastEffectiveMode == 3) {
        queueArtworkFrame();
    }
}

void Panel::writeLine(const QUuid &uuid, const QString &value)
{
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        return;
    }
    auto c = m_service->characteristic(uuid);
    if (c.isValid()) {
        m_service->writeCharacteristic(c, value.left(42).toStdString().c_str());
    }
}

void Panel::writeDisplayMode(uint8_t mode)
{
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        return;
    }
    auto c = m_service->characteristic(DISPLAY_MODE_UUID);
    if (c.isValid()) {
        m_service->writeCharacteristic(c, QByteArray(1, static_cast<char>(mode)));
    }
}

bool Panel::syncDisplayMode()
{
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        return false;
    }
    QSettings settings;
    settings.sync();
    const int displayMode = qBound(0, settings.value(u"displayMode"_s, 0).toInt(), 4);
    if (displayMode != m_lastDisplayMode) {
        m_lastDisplayMode = displayMode;
        m_autoGameActive = false;
        m_autoShowArtwork = false;
        m_nextAutomaticSwitch = {};
    }

    int effectiveMode = displayMode;
    if (displayMode == 3) {
        const auto now = std::chrono::steady_clock::now();
        if (!m_state->steamRunning()) {
            m_autoGameActive = false;
            m_autoShowArtwork = false;
            m_nextAutomaticSwitch = {};
            effectiveMode = 2;
        } else if (!m_state->gameRunning()) {
            m_autoGameActive = false;
            m_autoShowArtwork = false;
            m_nextAutomaticSwitch = {};
            effectiveMode = 1;
        } else if (!m_artworkSupported) {
            m_autoGameActive = false;
            m_autoShowArtwork = false;
            m_nextAutomaticSwitch = {};
            effectiveMode = 0;
        } else {
            if (!m_autoGameActive) {
                m_autoGameActive = true;
                m_autoShowArtwork = true;
                m_nextAutomaticSwitch = now + std::chrono::seconds(30);
            } else if (now >= m_nextAutomaticSwitch &&
                       !(m_autoShowArtwork && m_artSending)) {
                m_autoShowArtwork = !m_autoShowArtwork;
                m_nextAutomaticSwitch = now + std::chrono::seconds(30);
            }
            effectiveMode = m_autoShowArtwork && m_state->hasNowPlayingFrame() ? 3 : 0;
        }
    } else if (displayMode == 4) {
        effectiveMode = m_artworkSupported ? 3 : 0;
    }

    if (effectiveMode == m_lastEffectiveMode) {
        return false;
    }
    const int previousMode = m_lastEffectiveMode;
    m_lastEffectiveMode = effectiveMode;
    qDebug() << "Panel display mode changed:" << previousMode << "->" << effectiveMode;
    if (previousMode == 3 && effectiveMode != 3) {
        sendArtworkClear();
    }
    if (effectiveMode == 3) {
        queueArtworkFrame();
    } else {
        writeDisplayMode(static_cast<uint8_t>(effectiveMode));
    }
    return true;
}

void Panel::checkDisplayMode()
{
    if (syncDisplayMode() && m_lastEffectiveMode == 0) {
        sendState();
    }
}

void Panel::queueArtworkFrame()
{
    if (m_nowPlayingBits.isEmpty() || m_nowPlayingWidth == 0 || m_nowPlayingHeight == 0) {
        return;
    }
    if (m_artSending) {
        m_artResendPending = true;
        return;
    }
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        return;
    }
    auto characteristic = m_service->characteristic(ARTWORK_UUID);
    if (!characteristic.isValid()) {
        qWarning() << "Connected panel does not support Now Playing artwork";
        return;
    }

    m_artQueue.clear();
    QByteArray begin;
    begin.append(char(0x00));
    begin.append(char(m_nowPlayingWidth & 0xFF));
    begin.append(char((m_nowPlayingWidth >> 8) & 0xFF));
    begin.append(char(m_nowPlayingHeight & 0xFF));
    begin.append(char((m_nowPlayingHeight >> 8) & 0xFF));
    m_artQueue.append(begin);

    const qsizetype chunkSize = 244;
    for (qsizetype offset = 0; offset < m_nowPlayingBits.size(); offset += chunkSize) {
        QByteArray chunk;
        chunk.append(char(0x01));
        chunk.append(char(offset & 0xFF));
        chunk.append(char((offset >> 8) & 0xFF));
        chunk.append(char((offset >> 16) & 0xFF));
        chunk.append(char((offset >> 24) & 0xFF));
        chunk.append(m_nowPlayingBits.mid(offset, chunkSize));
        m_artQueue.append(chunk);
    }
    m_artQueue.append(QByteArray(1, char(0x02)));
    qDebug() << "Queued Now Playing artwork in" << m_artQueue.size() << "BLE writes";
    sendArtwork();
}

void Panel::sendArtwork()
{
    if (m_artQueue.isEmpty()) {
        m_artSending = false;
        return;
    }
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        m_artQueue.clear();
        m_artSending = false;
        return;
    }
    const auto characteristic = m_service->characteristic(ARTWORK_UUID);
    if (!characteristic.isValid()) {
        m_artQueue.clear();
        m_artSending = false;
        return;
    }
    m_artSending = true;
    m_service->writeCharacteristic(characteristic, m_artQueue.first());
}

void Panel::sendArtworkClear()
{
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        return;
    }
    if (m_artSending) {
        m_artQueue.clear();
        m_artClearPending = true;
        m_artResendPending = false;
        return;
    }
    const auto characteristic = m_service->characteristic(ARTWORK_UUID);
    if (!characteristic.isValid()) {
        return;
    }
    m_artClearPending = false;
    m_artQueue = {QByteArray(1, char(0x03))};
    sendArtwork();
}

void Panel::writeKeyVal(const uint8_t &index, const QString &key, const QString &value)
{
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        return;
    }
    auto c = m_service->characteristic(KEYVAL_UUID);
    if (c.isValid()) {
        QByteArray ba;
        QDataStream s(&ba, QDataStream::WriteOnly);
        s.setByteOrder(QDataStream::ByteOrder(QSysInfo::ByteOrder));

        s << index;
        s.writeRawData(key.left(31).toStdString().c_str(), 32);
        s.writeRawData(value.left(31).toStdString().c_str(), 32);
        m_service->writeCharacteristic(c, ba);
    }
}

void Panel::writePoints(const uint8_t &index, const PanelField *field)
{
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        return;
    }
    auto c = m_service->characteristic(VECTOR_UUID);
    if (c.isValid()) {
        QByteArray ba;
        QDataStream s(&ba, QDataStream::WriteOnly);
        s.setByteOrder(QDataStream::ByteOrder(QSysInfo::ByteOrder));
        s.setFloatingPointPrecision(QDataStream::SinglePrecision);

        s << index;
        if (qFuzzyCompare(field->xMax() + 1.0, field->xMin() + 1.0) ||
            qFuzzyCompare(field->yMax() + 1.0, field->yMin() + 1.0)) {
            // handle potential divide by zero errors, if line is flat, draw a flat line
            s << uint8_t(4);
            s << float(field->yMin());
            s << float(field->yMax());
            s << uint8_t(0);
            s << uint8_t(255 / 2);
            s << uint8_t(255);
            s << uint8_t(255 / 2);
        } else {
            // usually we just scale the x/y axis to the full range of a uint16
            s << uint8_t(field->points().size() * 2);
            s << float(field->yMin());
            s << float(field->yMax());
            double x, y;
            for (const auto &point : field->points()) {
                // we pack these into uint16_t to ease the unpack on the esp32
                x = 255.0 * ((point.x() - field->xMin()) / (field->xMax() - field->xMin()));
                y = 255.0 * ((point.y() - field->yMin()) / (field->yMax() - field->yMin()));
                s << uint8_t(x);
                s << uint8_t(y);
            }
        }

        m_service->writeCharacteristic(c, ba);
    }
}

void Panel::flushDisplay()
{
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        return;
    }
    auto c = m_service->characteristic(FLUSH_UUID);
    if (c.isValid()) {
        m_service->writeCharacteristic(
            c, QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss A").toLatin1());
    }
}

void Panel::connCheck()
{
    if (m_connecting) {
        return;
    }
    auto panel = m_finder->panel();
    bool matches = panel->name() == m_device.name();
    bool found = m_finder->panelFound();
    if (isConnected()) {
        auto delta = std::chrono::steady_clock::now() - m_lastComms;
        if (!matches) {
            qDebug() << "Removing connection, doesn't match.";
            clearConnection();
            m_finder->startDiscovery();
        } else if (delta > CONN_LOST) {
            qWarning() << "Connection inactive for" << delta << ", removing!";
            clearConnection();
            m_finder->startDiscovery();
        } else if (delta > CONN_WARNING) {
            qWarning() << "Connection inactive for" << delta;
        }
        return;
    }
    if (!found || panel->name().isEmpty()) {
        return;
    }
    m_finder->stopDiscovery();
    m_connecting = true;
    m_device = panel->bleInfo();
    qDebug() << "Connecting to " << m_device.name() << ", valid" << m_device.isValid() << ", cached"
             << m_device.isCached();
    m_controller = QLowEnergyController::createCentral(m_device, this);
    connect(m_controller, &QLowEnergyController::stateChanged, this,
            &Panel::onControllerStateChanged);
    connect(m_controller, &QLowEnergyController::discoveryFinished, this,
            &Panel::onControllerServicesDiscovered);
    connect(m_controller, &QLowEnergyController::errorOccurred, this, &Panel::onControllerError);
    m_controller->connectToDevice();
}

void Panel::clearConnection()
{
    if (m_service) {
        qDebug() << "clearing service";
        m_service->disconnect(this);
        m_service->deleteLater();
        m_service = nullptr;
    }
    if (m_controller) {
        qDebug() << "clearing controller";
        m_controller->disconnect(this);
        m_controller->disconnectFromDevice();
        m_controller->deleteLater();
        m_controller = nullptr;
    }
    m_device = QBluetoothDeviceInfo();
    m_connecting = false;
    m_artworkSupported = false;
    m_lastDisplayMode = -1;
    m_lastEffectiveMode = -1;
    m_artQueue.clear();
    m_artSending = false;
    m_artResendPending = false;
    m_artClearPending = false;
}

void Panel::sendState()
{
    if (!m_controller || m_controller->state() == QLowEnergyController::UnconnectedState) {
        return;
    }
    if (!m_service || m_service->state() != QLowEnergyService::RemoteServiceDiscovered) {
        return;
    }
    m_sendTimer->start(SEND_INTERVAL);
    syncDisplayMode();
    if (m_lastEffectiveMode == 2 || m_lastEffectiveMode == 3) {
        return;
    }

    if (m_state->dirty()) {
        writeLine(TOPLINE_UUID, m_state->topLine());
        writeLine(MIDLINE_UUID, m_state->midLine());
        writeLine(BOTLINE_UUID, m_state->botLine());
        int fields = 0;
        int sparks = 0;
        for (auto field : m_state->fields()) {
            writeKeyVal(fields, field->key(), field->val());
            ++fields;
            if (field->depth() > 0) {
                writePoints(sparks, field);
                ++sparks;
            }
        }
    }
    if (m_state->dirty() || m_lastEffectiveMode == 1) {
        flushDisplay();
    }

}
