#include "artwork.hpp"

#include <QDir>
#include <QFont>
#include <QFontMetrics>
#include <QNetworkReply>
#include <QPainter>
#include <QTimer>

using namespace Qt::Literals::StringLiterals;

namespace
{
constexpr int FRAME_WIDTH = 648;
constexpr int FRAME_HEIGHT = 480;
constexpr int MARGIN = 18;
constexpr int COVER_WIDTH = 260;
constexpr int COVER_HEIGHT = 430;
}

Artwork::Artwork(steam::Steam *steam, QObject *parent)
    : QObject(parent)
    , m_steam(steam)
    , m_netman(new QNetworkAccessManager(this))
{
}

QImage Artwork::loadLocalArtwork(const QString &appid) const
{
    const QString cache = m_steam->steamDir() + u"/appcache/librarycache"_s;
    const QStringList candidates = {
        u"%1/%2/library_600x900.jpg"_s.arg(cache, appid),
        u"%1/%2_library_600x900.jpg"_s.arg(cache, appid),
        u"%1/%2/header.jpg"_s.arg(cache, appid),
        u"%1/%2_header.jpg"_s.arg(cache, appid),
    };
    for (const auto &path : candidates) {
        QImage image(path);
        if (!image.isNull()) {
            return image;
        }
    }

    QDir appCache(u"%1/%2"_s.arg(cache, appid));
    const auto files = appCache.entryList({u"*.jpg"_s, u"*.png"_s}, QDir::Files);
    QImage best;
    for (const auto &file : files) {
        QImage image(appCache.absoluteFilePath(file));
        if (!image.isNull() && image.height() >= image.width() &&
            image.width() * image.height() > best.width() * best.height()) {
            best = image;
        }
    }
    return best;
}

void Artwork::composeForApp(const steam::App &app, double playtimeMinutes,
                            QPair<int, int> achievements)
{
    if (app.appid.isEmpty()) {
        return;
    }
    m_app = app;
    m_playtimeMinutes = playtimeMinutes;
    m_achievements = achievements;

    if (m_artworkAppId == app.appid && !m_artwork.isNull()) {
        compose();
        return;
    }
    if (m_pendingAppId == app.appid) {
        return;
    }

    m_artworkAppId = app.appid;
    m_artwork = loadLocalArtwork(app.appid);
    if (!m_artwork.isNull()) {
        compose();
        return;
    }

    m_pendingAppId = app.appid;
    const auto url =
        QUrl(u"https://cdn.cloudflare.steamstatic.com/steam/apps/%1/library_600x900.jpg"_s.arg(
            app.appid));
    auto *reply = m_netman->get(QNetworkRequest(url));
    QTimer::singleShot(10000, reply, &QNetworkReply::abort);
    connect(reply, &QNetworkReply::finished, this, [this, reply, appId = app.appid]() {
        reply->deleteLater();
        if (m_pendingAppId == appId) {
            m_pendingAppId.clear();
        }
        if (m_app.appid != appId) {
            return;
        }
        if (reply->isOpen() && reply->error() == QNetworkReply::NoError) {
            m_artwork.loadFromData(reply->readAll());
        } else {
            qWarning() << "Game artwork unavailable for" << appId << ":" << reply->errorString();
        }
        compose();
    });
}

void Artwork::compose()
{
    QImage frame(FRAME_WIDTH, FRAME_HEIGHT, QImage::Format_RGB32);
    frame.fill(Qt::white);
    QPainter painter(&frame);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.setPen(Qt::black);

    const QRect coverRect(MARGIN, MARGIN, COVER_WIDTH, COVER_HEIGHT);
    if (!m_artwork.isNull()) {
        const QImage cover =
            m_artwork.scaled(coverRect.size(), Qt::KeepAspectRatio, Qt::SmoothTransformation);
        const QRect placed(coverRect.x() + (coverRect.width() - cover.width()) / 2,
                           coverRect.y() + (coverRect.height() - cover.height()) / 2,
                           cover.width(), cover.height());
        painter.drawImage(placed, cover);
        painter.drawRect(placed.adjusted(-2, -2, 1, 1));
        painter.drawRect(placed.adjusted(-1, -1, 0, 0));
    } else {
        painter.drawRect(coverRect);
        QFont placeholder(u"DejaVu Sans Mono"_s);
        placeholder.setPixelSize(20);
        painter.setFont(placeholder);
        painter.drawText(coverRect, Qt::AlignCenter | Qt::TextWordWrap,
                         u"NO BOX ART"_s);
    }

    const int textX = MARGIN + COVER_WIDTH + MARGIN;
    const int textWidth = FRAME_WIDTH - textX - MARGIN;
    QFont titleFont(u"DejaVu Sans Mono"_s);
    titleFont.setBold(true);
    QRect titleBounds;
    for (int size = 34; size >= 18; size -= 2) {
        titleFont.setPixelSize(size);
        titleBounds = QFontMetrics(titleFont).boundingRect(
            QRect(textX, MARGIN, textWidth, 0), Qt::TextWordWrap, m_app.name);
        if (titleBounds.height() <= 190) {
            break;
        }
    }
    painter.setFont(titleFont);
    painter.drawText(QRect(textX, MARGIN, textWidth, titleBounds.height()), Qt::TextWordWrap,
                     m_app.name);

    QFont labelFont(u"DejaVu Sans Mono"_s);
    labelFont.setPixelSize(17);
    labelFont.setBold(true);
    QFont valueFont(u"DejaVu Sans Mono"_s);
    valueFont.setPixelSize(25);
    valueFont.setBold(true);
    int y = MARGIN + titleBounds.height() + 34;

    painter.setFont(labelFont);
    painter.drawText(textX, y, u"PLAYTIME"_s);
    painter.setFont(valueFont);
    y += 34;
    if (m_playtimeMinutes < 0) {
        painter.drawText(textX, y, u"--"_s);
    } else if (m_playtimeMinutes < 60) {
        painter.drawText(textX, y, u"%1 MIN"_s.arg(QString::number(m_playtimeMinutes, 'f', 0)));
    } else {
        painter.drawText(textX, y,
                         u"%1 HRS"_s.arg(QString::number(m_playtimeMinutes / 60.0, 'f', 1)));
    }
    y += 52;

    painter.setFont(labelFont);
    painter.drawText(textX, y, u"ACHIEVEMENTS"_s);
    painter.setFont(valueFont);
    y += 34;
    if (m_achievements.second > 0) {
        painter.drawText(textX, y, u"%1 / %2"_s.arg(QString::number(m_achievements.first),
                                                    QString::number(m_achievements.second)));
        y += 16;
        const int barWidth = textWidth;
        const int fillWidth = barWidth * m_achievements.first / m_achievements.second;
        painter.drawRect(textX, y, barWidth - 1, 20);
        painter.fillRect(textX + 3, y + 3, qMax(0, fillWidth - 6), 14, Qt::black);
    } else {
        painter.drawText(textX, y, u"--"_s);
    }

    QFont tagFont(u"DejaVu Sans Mono"_s);
    tagFont.setPixelSize(14);
    painter.setFont(tagFont);
    painter.drawText(QRect(textX, FRAME_HEIGHT - MARGIN - 20, textWidth, 20),
                     Qt::AlignRight | Qt::AlignBottom, u"NOW PLAYING"_s);
    painter.end();

    const QImage mono = frame.convertToFormat(QImage::Format_Mono, Qt::DiffuseDither);
    const auto colors = mono.colorTable();
    const bool indexOneIsInk = colors.size() > 1 && qGray(colors.at(1)) < qGray(colors.at(0));
    const int rowBytes = (FRAME_WIDTH + 7) / 8;
    QByteArray bits(rowBytes * FRAME_HEIGHT, char(0));
    for (int yPos = 0; yPos < FRAME_HEIGHT; ++yPos) {
        const uchar *source = mono.constScanLine(yPos);
        uchar *dest = reinterpret_cast<uchar *>(bits.data()) + yPos * rowBytes;
        for (int x = 0; x < FRAME_WIDTH; ++x) {
            const int colorIndex = (source[x / 8] >> (7 - (x % 8))) & 1;
            const bool ink = (colorIndex == 1) == indexOneIsInk;
            if (ink) {
                dest[x / 8] |= static_cast<uchar>(0x80 >> (x % 8));
            }
        }
    }
    emit frameReady(bits, FRAME_WIDTH, FRAME_HEIGHT);
}
