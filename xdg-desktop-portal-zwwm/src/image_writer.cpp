#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

#include "image_writer.hpp"

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTemporaryFile>

namespace {

void appendBytes(void* context, void* data, int size) {
  if (size > 0) static_cast<QByteArray*>(context)->append(static_cast<const char*>(data), size);
}

QByteArray encode(const QImage& source, const QString& suffix) {
  const QImage image = source.convertToFormat(QImage::Format_RGB888);
  if (image.isNull()) return {};
  QByteArray bytes;
  const QByteArray extension = suffix.toLower().toLatin1();
  int result = 0;
  if (extension == "png")
    result = stbi_write_png_to_func(appendBytes, &bytes, image.width(), image.height(), 3,
                                    image.constBits(), image.bytesPerLine());
  else if (extension == "bmp")
    result = stbi_write_bmp_to_func(appendBytes, &bytes, image.width(), image.height(), 3, image.constBits());
  else if (extension == "tga")
    result = stbi_write_tga_to_func(appendBytes, &bytes, image.width(), image.height(), 3, image.constBits());
  else if (extension == "jpg" || extension == "jpeg")
    result = stbi_write_jpg_to_func(appendBytes, &bytes, image.width(), image.height(), 3, image.constBits(), 90);
  else if (extension == "ppm") {
    bytes = QByteArray("P6\n") + QByteArray::number(image.width()) + ' ' + QByteArray::number(image.height()) +
            QByteArray("\n255\n");
    for (int row = 0; row < image.height(); ++row)
      bytes.append(reinterpret_cast<const char*>(image.constScanLine(row)), image.width() * 3);
    result = 1;
  }
  return result == 0 ? QByteArray{} : bytes;
}

}  // namespace

QStringList screenshotNameFilters() {
  return {QStringLiteral("PNG image (*.png)"), QStringLiteral("JPEG image (*.jpg *.jpeg)"),
          QStringLiteral("Bitmap image (*.bmp)"), QStringLiteral("TGA image (*.tga)"),
          QStringLiteral("PPM image (*.ppm)")};
}

bool writeScreenshot(const QString& path, const QImage& image, QString* error) {
  const QByteArray bytes = encode(image, QFileInfo(path).suffix());
  if (bytes.isEmpty()) {
    if (error != nullptr) *error = QStringLiteral("unsupported or failed image encoding");
    return false;
  }
  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
    if (error != nullptr) *error = file.errorString();
    return false;
  }
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  return true;
}

QString writeTemporaryScreenshot(const QImage& image, QString* error) {
  QString directory = qEnvironmentVariable("XDG_RUNTIME_DIR");
  if (directory.isEmpty()) directory = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
  QTemporaryFile file(QDir(directory).filePath(QStringLiteral("zwwm-screenshot-XXXXXX.png")));
  const QByteArray bytes = encode(image, QStringLiteral("png"));
  if (bytes.isEmpty() || !file.open() || file.write(bytes) != bytes.size() || !file.flush()) {
    if (error != nullptr) *error = file.errorString();
    return {};
  }
  file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  file.setAutoRemove(false);
  const QString path = file.fileName();
  file.close();
  return path;
}
