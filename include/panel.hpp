#ifndef PANEL_HPP
#define PANEL_HPP

#include <QByteArray>
#include <QList>
#include <QLowEnergyController>
#include <QObject>
#include <QTimer>

#include <chrono>

#include "panel-finder.hpp"
#include "panel-state.hpp"

class Panel : public QObject
{
    Q_OBJECT

  public:
    explicit Panel(QObject *parent = nullptr);
    ~Panel()
    {
        if (m_controller) {
            m_controller->disconnectFromDevice();
        }
    }

    bool isConnected() const
    {
        auto state = m_controller ? m_controller->state() : QLowEnergyController::UnconnectedState;
        return state == QLowEnergyController::ConnectingState ||
               state == QLowEnergyController::ConnectedState ||
               state == QLowEnergyController::DiscoveringState ||
               state == QLowEnergyController::DiscoveredState;
    }

  public slots:
    void stop();

  private slots:
    void connCheck();
    void sendState();
    void checkDisplayMode();

    void clearConnection();
    void onControllerStateChanged(QLowEnergyController::ControllerState state);
    void onControllerServicesDiscovered();
    void onControllerError(QLowEnergyController::Error error);
    void onServiceStateChanged(QLowEnergyService::ServiceState state);
    void onServiceError(QLowEnergyService::ServiceError error);
    void onServiceCharacteristicWritten(const QLowEnergyCharacteristic &characteristic,
                                        const QByteArray &value);
    void onArtworkFrameReady(const QByteArray &bits, quint16 width, quint16 height);

  private:
    QLowEnergyController *m_controller = nullptr;
    QLowEnergyService *m_service = nullptr;
    QBluetoothDeviceInfo m_device;

    PanelFinder *m_finder = nullptr;
    PanelState *m_state = nullptr;

    QTimer *m_connTimer = nullptr;
    QTimer *m_sendTimer = nullptr;
    QTimer *m_modeTimer = nullptr;
    bool m_connecting = false;
    bool m_stopping = false;
    bool m_artworkSupported = false;
    int m_lastDisplayMode = -1;
    int m_lastEffectiveMode = -1;
    bool m_autoGameActive = false;
    bool m_autoShowArtwork = false;
    QByteArray m_nowPlayingBits;
    quint16 m_nowPlayingWidth = 0;
    quint16 m_nowPlayingHeight = 0;
    QList<QByteArray> m_artQueue;
    bool m_artSending = false;
    bool m_artResendPending = false;
    bool m_artClearPending = false;
    std::chrono::steady_clock::time_point m_nextAutomaticSwitch;
    std::chrono::time_point<std::chrono::steady_clock> m_lastComms;

    void writeLine(const QUuid &uuid, const QString &value);
    void writeDisplayMode(uint8_t mode);
    bool syncDisplayMode();
    void queueArtworkFrame();
    void sendArtwork();
    void sendArtworkClear();
    void writeKeyVal(const uint8_t &index, const QString &key, const QString &value);
    void writePoints(const uint8_t &index, const PanelField *field);
    void flushDisplay();
};

#endif /* PANEL_HPP */
