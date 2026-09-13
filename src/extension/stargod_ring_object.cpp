#include "stargod_ring_object.h"
#include <QAbstractTextDocumentLayout>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextFragment>
#include <QTextCursor>
#include <QTextImageFormat>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <cmath>

QString stargodRingUrl(const QVector<StargodRingSlot>& cells) {
  QJsonArray values;
  for (const auto& cell : cells) values.append(QJsonObject{{"id",cell.imageId},{"q",cell.quality},
      {"lv",cell.level},{"n",cell.name},{"empty",cell.empty},{"center",cell.center}});
  return QStringLiteral("kqstargodring://ring/") + QString::fromLatin1(
      QJsonDocument(values).toJson(QJsonDocument::Compact).toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}
QVector<StargodRingSlot> stargodRingSlots(const QUrl& url) {
  if (url.scheme() != QStringLiteral("kqstargodring") || url.host() != QStringLiteral("ring") || url.path().size() > 65536) return {};
  const auto document = QJsonDocument::fromJson(QByteArray::fromBase64(url.path().mid(1).toLatin1(),QByteArray::Base64UrlEncoding));
  if (!document.isArray() || document.array().size() > 64) return {};
  QVector<StargodRingSlot> result;
  for (const auto& value : document.array()) {
    if (!value.isObject()) return {};
    const auto o = value.toObject();
    const int id = o.value("id").toInt(-1);
    if (id < 0 || id > 100000 || o.value("n").toString().size() > 256) return {};
    result.append({id,o.value("q").toInt(),o.value("lv").toInt(),o.value("n").toString(),
                   o.value("empty").toBool(),o.value("center").toBool()});
  }
  return result;
}
StargodRingObject::StargodRingObject(std::function<QImage(const QString&)> images, QObject* parent)
    : QObject(parent), images_(std::move(images)) {}
QSizeF StargodRingObject::intrinsicSize(QTextDocument* document, int, const QTextFormat&) {
  const qreal side = qBound<qreal>(120, document->textWidth() - 16, 540);
  return {side, side};
}
void StargodRingObject::drawObject(QPainter* painter, const QRectF& rect, QTextDocument*, int, const QTextFormat& format) {
  const auto cells = stargodRingSlots(QUrl(format.stringProperty(DataProperty)));
  QVector<StargodRingSlot> outer, center;
  for (const auto& cell : cells) (cell.center ? center : outer).append(cell);
  painter->save(); painter->translate(rect.topLeft()); painter->scale(rect.width()/400,rect.height()/400);
  painter->setRenderHint(QPainter::Antialiasing); painter->setRenderHint(QPainter::SmoothPixmapTransform);
  QRadialGradient background(QPointF(200,200),196); background.setColorAt(0,QColor("#72b9dc")); background.setColorAt(1,QColor("#d8f0fa"));
  painter->setBrush(background); painter->setPen(QPen(QColor("#84bbd4"),2)); painter->drawEllipse(QRectF(5,5,390,390));
  painter->setBrush(Qt::NoBrush); painter->setPen(QPen(QColor("#9bcfe3"),3));
  painter->drawEllipse(QPointF(200,200),145,145); painter->drawEllipse(QPointF(200,200),181,181);
  const auto drawCell = [&](const StargodRingSlot& cell, QPointF point, qreal diameter) {
    const QRectF circle(point.x()-diameter/2,point.y()-diameter/2,diameter,diameter);
    painter->setPen(QPen(QColor("#6a5220"),5)); painter->setBrush(QColor("#101722")); painter->drawEllipse(circle);
    painter->setPen(QPen(QColor("#fff095"),3)); painter->drawEllipse(circle.adjusted(1,1,-1,-1));
    if (cell.empty) return;
    const auto image = images_(QStringLiteral("kqstargod://icon/%1").arg(cell.imageId));
    if (!image.isNull()) {
      painter->save(); QPainterPath clip; clip.addEllipse(circle.adjusted(5,5,-5,-5)); painter->setClipPath(clip);
      painter->drawImage(circle.adjusted(6,4,-6,-10),image); painter->restore();
    }
    QFont font(QStringLiteral("Microsoft YaHei UI")); font.setBold(true); font.setPixelSize(qMax(9,int(diameter/7)));
    painter->setFont(font);
    const QColor color = cell.quality >= 6 ? QColor("#ff3d3d") : cell.quality == 5 ? QColor("#ffdb48") : cell.quality == 4 ? QColor("#d49aff") : QColor(Qt::white);
    const QRectF text = circle.adjusted(2,diameter*0.54,-2,-13);
    painter->setPen(Qt::black); painter->drawText(text.translated(1,1),Qt::AlignCenter|Qt::TextWordWrap,cell.name);
    painter->setPen(color); painter->drawText(text,Qt::AlignCenter|Qt::TextWordWrap,cell.name);
    if (cell.level > 0) {
      font.setPixelSize(qMax(9,int(diameter/7))); painter->setFont(font); painter->setPen(QColor("#ffff50"));
      painter->drawText(circle.adjusted(0,diameter-19,0,-2),Qt::AlignCenter,QStringLiteral("Lv.%1").arg(cell.level));
    }
  };
  constexpr qreal pi = 3.14159265358979323846;
  const qreal diameter = outer.size() > 8 ? qMax<qreal>(22,2*143*std::sin(pi/outer.size())-9) : 86;
  for (int i=0;i<outer.size();++i) {
    const qreal angle = -pi/2 + 2*pi*i/outer.size();
    drawCell(outer[i],QPointF(200+143*std::cos(angle),200+143*std::sin(angle)),diameter);
  }
  if (!center.isEmpty()) {
    QFont font(QStringLiteral("Microsoft YaHei UI")); font.setBold(true); font.setPixelSize(17); painter->setFont(font);
    painter->setPen(QColor("#164b79")); painter->drawText(QRectF(130,137,140,24),Qt::AlignCenter,QStringLiteral("万变格"));
    const qreal width = center.size()==1 ? 90 : qMin<qreal>(75,130.0/center.size());
    for(int i=0;i<center.size();++i) drawCell(center[i],QPointF(200+(i-(center.size()-1)/2.0)*width,207),width-3);
  }
  painter->restore();
}
void StargodRingObject::install(QTextDocument* document, StargodRingObject* object) {
  document->documentLayout()->registerHandler(ObjectType,object);
  struct Replacement { int position; QString url; };
  QVector<Replacement> replacements;
  for (auto block=document->begin();block.isValid();block=block.next()) for (auto it=block.begin();!it.atEnd();++it) {
    const auto fragment=it.fragment(); if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) continue;
    const QString name=fragment.charFormat().toImageFormat().name();
    if (!stargodRingSlots(QUrl(name)).isEmpty()) replacements.append({fragment.position(),name});
  }
  for (const auto& replacement : replacements) {
    QTextCursor cursor(document); cursor.setPosition(replacement.position); cursor.movePosition(QTextCursor::NextCharacter,QTextCursor::KeepAnchor);
    QTextCharFormat format; format.setObjectType(ObjectType); format.setProperty(DataProperty,replacement.url); cursor.setCharFormat(format);
  }
}
