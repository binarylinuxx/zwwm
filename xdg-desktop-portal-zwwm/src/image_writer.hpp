#pragma once

#include <QImage>
#include <QString>

QStringList screenshotNameFilters();
bool writeScreenshot(const QString& path, const QImage& image, QString* error);
QString writeTemporaryScreenshot(const QImage& image, QString* error);
