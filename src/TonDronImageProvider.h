#pragma once

#include <QQuickImageProvider>

// Loads cached JPEG/PNG files for QML Image via image://TonDron/<percent-encoded-path>
class TonDronImageProvider : public QQuickImageProvider
{
public:
    TonDronImageProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};
