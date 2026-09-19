#pragma once
#include <QObject>
#include <QTextObjectInterface>
#include <QTextFormat>
#include <QImage>
#include <QVector>
#include <QUrl>
#include <functional>

struct StargodRingSlot {
  int imageId = 0, quality = 0, level = 0;
  QString name;
  bool empty = false, center = false;
};
QString stargodRingUrl(const QVector<StargodRingSlot>& cells);
QVector<StargodRingSlot> stargodRingSlots(const QUrl& url);

class StargodRingObject final : public QObject, public QTextObjectInterface {
  Q_OBJECT
  Q_INTERFACES(QTextObjectInterface)
public:
  static constexpr int ObjectType = QTextFormat::UserObject + 41;
  static constexpr int DataProperty = QTextFormat::UserProperty + 41;
  explicit StargodRingObject(std::function<QImage(const QString&)> images, QObject* parent = nullptr);
  QSizeF intrinsicSize(QTextDocument*, int, const QTextFormat&) override;
  void drawObject(QPainter*, const QRectF&, QTextDocument*, int, const QTextFormat&) override;
  static void install(QTextDocument* document, StargodRingObject* object);
private:
  std::function<QImage(const QString&)> images_;
};
