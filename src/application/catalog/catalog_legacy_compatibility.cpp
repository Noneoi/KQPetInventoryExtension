#include "catalog_legacy_compatibility.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonDocument>
#include <QRegularExpression>

namespace CatalogLegacyCompatibility {
QString officialShopUrl(const QString& version) {
  return QStringLiteral("https://aoqi.100bt.com/play/newactivityext/newact20260313/"
                        "storeexchangeframework/storeexchangeframework~%1.swf").arg(version);
}
QDate shopVersionDate(const QString& version) {
  static const QRegularExpression digits(QStringLiteral("^[0-9]{8,20}$"));
  if (!digits.match(version).hasMatch()) return {};
  return QDate::fromString(version.left(8), QStringLiteral("yyyyMMdd"));
}
QDate officialShopDate(const QJsonObject& root) {
  const auto source = root.value(QStringLiteral("source")).toObject();
  const QString version = source.value(QStringLiteral("shopVersion")).toString();
  const QDate date = shopVersionDate(version);
  const QString url = source.value(QStringLiteral("resources")).toObject()
      .value(QStringLiteral("shop")).toObject().value(QStringLiteral("url")).toString();
  if (date.isValid() && url == officialShopUrl(version)) return date;
  // Two precisely identified pre-versioned official catalogs: the old
  // description whitelist and the complete 2026-08-13 SEFConfig import.
  // Metadata/JSON whitespace are irrelevant; a user's changed catalog does
  // not match either hash and is not treated as an old official version.
  const QJsonObject content{{QStringLiteral("protocol"), root.value(QStringLiteral("protocol"))},
                            {QStringLiteral("shops"), root.value(QStringLiteral("shops"))}};
  const QByteArray digest = QCryptographicHash::hash(QJsonDocument(content).toJson(QJsonDocument::Compact),
      QCryptographicHash::Sha256).toHex();
  if (digest == "fa48dbb5ef19ce72bbbc847ee9da53ace95b46d254144d905c1a629d1c517bb4" ||
      digest == "dba434623f89630cf77d34e1a6e34ff853cfcc624bc476b5e83e66041bea41d8")
    return QDate(2026, 8, 13);
  return {};
}
QDate bundledShopDate() {
  // Called on Storage I/O. Keep the baseline tied to the executable resource,
  // not to whichever local overlay happened to be loaded most recently.
  QFile file(QStringLiteral(":/kqpet/shop-exchange-data.json"));
  if (!file.open(QIODevice::ReadOnly) || file.size() > 2 * 1024 * 1024) return {};
  const auto root = QJsonDocument::fromJson(file.readAll()).object();
  return officialShopDate(root);
}

QDate officialPetMetadataDate(const QJsonObject& root) {
  const auto source = root.value(QStringLiteral("source")).toObject();
  const QString version = source.value(QStringLiteral("petDictionaryVersion")).toString();
  const QDate date = shopVersionDate(version);
  const QString url = source.value(QStringLiteral("resources")).toObject()
      .value(QStringLiteral("petDictionaryUpdate")).toObject().value(QStringLiteral("url")).toString();
  if (date.isValid() && url == QStringLiteral("https://aoqi.100bt.com/play/pet/petdictionarydataupdate~%1.swf").arg(version))
    return date;
  // Exact pre-versioned 2026-08-13 pet table. Unrecognized user overlays stay
  // loadable; an old official full cache must not replace updated identities.
  const auto pets = root.value(QStringLiteral("pets")).toObject();
  if (!pets.isEmpty() && QCryptographicHash::hash(QJsonDocument(pets).toJson(QJsonDocument::Compact),
          QCryptographicHash::Sha256).toHex() == "3612ecf10ac6f21876770ee3ae1fa370d122889b96c675328ee25c82f6326613")
    return QDate(2026, 8, 13);
  return {};
}

}
