#ifndef ARTWORK_HPP
#define ARTWORK_HPP

#include <QImage>
#include <QNetworkAccessManager>
#include <QObject>

#include "steam.hpp"

class Artwork : public QObject
{
    Q_OBJECT

  public:
    explicit Artwork(steam::Steam *steam, QObject *parent = nullptr);
    void composeForApp(const steam::App &app, double playtimeMinutes,
                       QPair<int, int> achievements);

  signals:
    void frameReady(QByteArray bits, quint16 width, quint16 height);

  private:
    steam::Steam *m_steam = nullptr;
    QNetworkAccessManager *m_netman = nullptr;
    steam::App m_app;
    double m_playtimeMinutes = -1;
    QPair<int, int> m_achievements = {-1, -1};
    QString m_artworkAppId;
    QString m_pendingAppId;
    QImage m_artwork;

    QImage loadLocalArtwork(const QString &appid) const;
    void compose();
};

#endif
