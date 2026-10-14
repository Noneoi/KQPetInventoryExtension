#include "public_dataset_store.h"

#include "battle_replay_catalog_pack.h"
#include "battle_replay_catalog_validation.h"
#include "catalog_legacy_compatibility.h"
#include "pet_detail_catalog.h"
#include "pet_skill_catalog.h"
#include "routine_overview_catalog.h"
#include "shop_exchange_catalog.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLockFile>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QTemporaryFile>
#include <QUuid>
#include <QtEndian>
#include <cmath>
#include <charconv>
#include <algorithm>
#include <vector>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace {
using Status = PublicDatasetStatus;
constexpr qint64 JsonLimit = 32 * 1024 * 1024;
struct Failure { Status status; QString error; };
[[noreturn]] void fail(const QString& error, Status status = Status::Invalid) { throw Failure{status, error}; }
void require(bool value, const QString& error, Status status = Status::Invalid) { if (!value) fail(error, status); }
QString sha(const QByteArray& bytes) {
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}
QByteArray json(const QJsonObject& object) { return QJsonDocument(object).toJson(QJsonDocument::Compact); }

// Python's independent quality reader keeps integer and floating-point JSON
// tokens distinct (1 != 1.0 for a content digest). QJson deliberately drops
// that distinction. Normalize the original JSON tokens instead of hashing a
// QJson reserialization, using Python's shortest-double spelling thresholds.
class QualityCanonicalJson {
public:
  explicit QualityCanonicalJson(const QByteArray& bytes, std::function<void()> cancellation)
      : bytes_(bytes), cancellation_(std::move(cancellation)) {}
  QByteArray normalized() {
    if (bytes_.startsWith("\xef\xbb\xbf")) cursor_ = 3;
    whitespace(); require(next() == '{', QStringLiteral("公共资料 JSON 对象无效"));
    auto result = value(0); whitespace();
    require(cursor_ == bytes_.size(), QStringLiteral("原文核对 JSON 含额外内容"));
    return result;
  }
private:
  void whitespace() {
    while (cursor_ < bytes_.size() && (bytes_[cursor_] == ' ' || bytes_[cursor_] == '\t' || bytes_[cursor_] == '\n' || bytes_[cursor_] == '\r')) ++cursor_;
  }
  char next() const { return cursor_ < bytes_.size() ? bytes_[cursor_] : '\0'; }
  void take(char wanted) {
    whitespace(); require(next() == wanted, QStringLiteral("原文核对 JSON 结构无效")); ++cursor_;
  }
  std::pair<QString, QByteArray> string() {
    whitespace(); require(next() == '"', QStringLiteral("原文核对 JSON 字符串无效"));
    const auto start = cursor_++;
    while (cursor_ < bytes_.size()) {
      const char current = bytes_[cursor_++];
      if (current == '\\') { require(cursor_ < bytes_.size(), QStringLiteral("JSON 转义被截断")); ++cursor_; }
      else if (current == '"') {
        QJsonParseError error{};
        const auto parsed = QJsonDocument::fromJson(QByteArray("[") + bytes_.mid(start, cursor_ - start) + "]", &error);
        require(error.error == QJsonParseError::NoError && parsed.isArray() && parsed.array().size() == 1 && parsed.array().first().isString(),
                QStringLiteral("JSON 字符串编码无效"));
        const auto text = parsed.array().first().toString();
        characters_ += text.size();
        require(characters_ <= 96 * 1024 * 1024, QStringLiteral("公共资料超过结构预算"));
        const auto encoded = QJsonDocument(QJsonArray{text}).toJson(QJsonDocument::Compact);
        return {text, encoded.mid(1, encoded.size() - 2)};
      }
    }
    fail(QStringLiteral("JSON 字符串被截断"));
  }
  QByteArray number() {
    const auto start = cursor_;
    while (cursor_ < bytes_.size() && QByteArray("-+0123456789.eE").contains(next())) ++cursor_;
    const auto raw = bytes_.mid(start, cursor_ - start);
    QJsonParseError error{};
    const auto parsed = QJsonDocument::fromJson(QByteArray("[") + raw + "]", &error);
    require(error.error == QJsonParseError::NoError && parsed.isArray() && parsed.array().size() == 1 && parsed.array().first().isDouble(),
            QStringLiteral("原文核对 JSON 数字无效"));
    if (!raw.contains('.') && !raw.contains('e') && !raw.contains('E')) return raw == "-0" ? QByteArray("0") : raw;
    bool valid = false; const double numeric = raw.toDouble(&valid);
    require(valid && std::isfinite(numeric), QStringLiteral("原文核对 JSON 数字越界"));
    if (numeric == 0) return std::signbit(numeric) ? QByteArray("-0.0") : QByteArray("0.0");
    char buffer[128];
    const auto rendered = std::to_chars(buffer, buffer + sizeof(buffer), numeric, std::chars_format::scientific);
    require(rendered.ec == std::errc{}, QStringLiteral("无法规范化原文核对数字"));
    const QByteArray scientific(buffer, rendered.ptr - buffer);
    const auto split = scientific.indexOf('e');
    const int exponent = scientific.mid(split + 1).toInt(&valid);
    require(valid && split > 0, QStringLiteral("原文核对数字指数无效"));
    const auto mantissa = scientific.left(split);
    if (exponent < -4 || exponent >= 16) {
      auto digits = QByteArray::number(std::abs(exponent));
      if (digits.size() < 2) digits.prepend('0');
      return mantissa + (exponent < 0 ? "e-" : "e+") + digits;
    }
    QByteArray digits = mantissa; const bool negative = digits.startsWith('-');
    if (negative) digits.remove(0, 1);
    digits.replace(".", "");
    const int point = exponent + 1;
    QByteArray result;
    if (point <= 0) result = QByteArray("0.") + QByteArray(-point, '0') + digits;
    else if (point >= digits.size()) result = digits + QByteArray(point - digits.size(), '0') + ".0";
    else result = digits.left(point) + '.' + digits.mid(point);
    return negative ? QByteArray("-") + result : result;
  }
  QByteArray value(int depth) {
    require(depth <= 64 && ++nodes_ <= 6000000, QStringLiteral("原文核对 JSON 超过结构预算"));
    if ((nodes_ & 4095) == 0 && cancellation_) cancellation_();
    whitespace();
    if (next() == '"') return string().second;
    if (next() == '{') {
      ++cursor_; whitespace();
      // Match Python's Unicode codepoint order. Compute each sort key once;
      // repeated toUcs4() in the comparator allocated on every comparison.
      std::vector<std::pair<QList<uint>, std::pair<QByteArray, QByteArray>>> rows;
      QSet<QString> seen;
      while (next() != '}') {
        auto key = string(); require(!seen.contains(key.first), QStringLiteral("公共资料 JSON 含重复对象键")); seen.insert(key.first);
        take(':'); auto child = value(depth + 1);
        rows.emplace_back(key.first.toUcs4(), std::make_pair(std::move(key.second), std::move(child)));
        whitespace(); if (next() == '}') break; take(','); whitespace();
        require(next() != '}', QStringLiteral("公共资料 JSON 对象含尾逗号"));
      }
      take('}');
      std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) {
        return std::lexicographical_compare(left.first.begin(), left.first.end(), right.first.begin(), right.first.end());
      });
      QByteArray out("{");
      for (const auto& row : rows) { if (out.size() > 1) out += ','; out += row.second.first; out += ':'; out += row.second.second; }
      out += '}'; return out;
    }
    if (next() == '[') {
      ++cursor_; whitespace(); QByteArray out("[");
      while (next() != ']') {
        if (out.size() > 1) out += ',';
        out += value(depth + 1); whitespace(); if (next() == ']') break; take(','); whitespace();
        require(next() != ']', QStringLiteral("公共资料 JSON 数组含尾逗号"));
      }
      take(']'); out += ']'; return out;
    }
    for (const auto* literal : {"true", "false", "null"}) {
      const auto text = QByteArray(literal);
      if (bytes_.mid(cursor_, text.size()) == text) { cursor_ += text.size(); return text; }
    }
    return number();
  }
  const QByteArray& bytes_;
  qsizetype cursor_ = 0;
  qint64 nodes_ = 0;
  qint64 characters_ = 0;
  std::function<void()> cancellation_;
};
QString qualityDigest(const QByteArray& bytes, std::function<void()> cancellation) {
  return sha(QualityCanonicalJson(bytes, std::move(cancellation)).normalized());
}
QString token() { return QUuid::createUuid().toString(QUuid::Id128); }
bool digestValid(const QString& value) {
  static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{64}$"));
  return expression.match(value).hasMatch();
}
bool tokenValid(const QString& value) {
  static const QRegularExpression expression(QStringLiteral("^[0-9a-f]{32}$"));
  return expression.match(value).hasMatch();
}
bool reparse(const QString& path) {
#ifdef Q_OS_WIN
  const DWORD attributes = GetFileAttributesW(reinterpret_cast<LPCWSTR>(path.utf16()));
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT);
#else
  return QFileInfo(path).isSymLink();
#endif
}
void safeAbsolute(const QString& path) {
  require(QDir::isAbsolutePath(path) && path.size() < 32760 && !path.contains(QChar::Null),
          QStringLiteral("公共资料路径不是有效绝对路径"));
  QString current = QDir::cleanPath(path);
  for (;;) {
    require(!reparse(current), QStringLiteral("公共资料路径包含链接或重解析点：%1").arg(current));
    const QString parent = QFileInfo(current).absolutePath();
    if (parent == current) break;
    current = parent;
  }
}
QJsonObject object(const QByteArray& bytes) {
  QJsonParseError error{};
  const auto document = QJsonDocument::fromJson(bytes, &error);
  require(error.error == QJsonParseError::NoError && document.isObject(), QStringLiteral("公共资料 JSON 对象无效"));
  return document.object();
}
QString componentFile(const QString& name) {
  if (name == QStringLiteral("pets")) return QStringLiteral("pet-detail-data.json");
  if (name == QStringLiteral("skills")) return QStringLiteral("pet-skill-data.json");
  if (name == QStringLiteral("shop")) return QStringLiteral("shop-exchange-data.json");
  if (name == QStringLiteral("routines")) return QStringLiteral("routine-overview.json");
  if (name == QStringLiteral("images")) return QStringLiteral("pet-image-index.json");
  if (name == QStringLiteral("icons")) return QStringLiteral("public-icon-index.json");
  return QStringLiteral("battle-replay-catalog.json");
}
QJsonObject refJson(const PublicDatasetObject& descriptor) {
  return {{"sha256", descriptor.sha256}, {"bytes", descriptor.bytes},
          {"format", descriptor.format}, {"origin", descriptor.origin}};
}
QJsonObject inputRef(const PublicDatasetObject& descriptor) {
  return {{"sha256", descriptor.sha256}, {"bytes", descriptor.bytes}, {"format", descriptor.format}};
}
QByteArray packedJson(const QByteArray& jsonBytes) {
  const auto compressed = qCompress(jsonBytes, 9).mid(4);
  require(!compressed.isEmpty() && jsonBytes.size() <= BattleReplayCatalogPack::MaximumJsonBytes,
          QStringLiteral("无法无损封装原有回放 JSON"));
  QByteArray result(BattleReplayCatalogPack::HeaderBytes, '\0');
  result.replace(qsizetype(0), qsizetype(8), QByteArray("KQBRPK1\n", 8));
  qToBigEndian<quint64>(quint64(jsonBytes.size()), result.data() + 8);
  qToBigEndian<quint64>(quint64(compressed.size()), result.data() + 16);
  result.replace(24, 32, QCryptographicHash::hash(jsonBytes, QCryptographicHash::Sha256));
  result.append(compressed);
  return result;
}
QString sourceLabel(const PublicDatasetObject& descriptor) {
  const QString kind = descriptor.origin.value(QStringLiteral("kind")).toString();
  if (kind == QStringLiteral("embedded-baseline")) return QStringLiteral("程序内置公共资料基线");
  if (kind == QStringLiteral("unavailable")) return QStringLiteral("此类公共资料尚不可用");
  if (kind.startsWith(QStringLiteral("legacy"))) return QStringLiteral("已迁移的共享公共资料");
  return QStringLiteral("工作台共享公共资料");
}
bool unavailable(const PublicDatasetObject& descriptor) {
  return descriptor.origin.value(QStringLiteral("kind")) == QStringLiteral("unavailable");
}
bool bounds(const QJsonValue& value, qint64& nodes, qint64& characters, int depth = 0) {
  if (depth > 64 || ++nodes > 6000000) return false;
  if (value.isString()) characters += value.toString().size();
  if (characters > 96 * 1024 * 1024) return false;
  if (value.isArray()) for (const auto& child : value.toArray()) if (!bounds(child, nodes, characters, depth + 1)) return false;
  if (value.isObject()) {
    const auto root = value.toObject();
    for (auto item = root.begin(); item != root.end(); ++item) {
      characters += item.key().size();
      if (!bounds(item.value(), nodes, characters, depth + 1)) return false;
    }
  }
  return true;
}
QSet<QString> stringSet(const QJsonValue& value) {
  require(value.isArray(), QStringLiteral("公共资料组件列表无效"));
  QSet<QString> result;
  for (const auto& item : value.toArray()) {
    require(item.isString() && PublicDatasetStore::componentKeys().contains(item.toString()) && !result.contains(item.toString()),
            QStringLiteral("公共资料组件列表含重复或未知组件"));
    result.insert(item.toString());
  }
  return result;
}
QJsonObject failureReceipt(const Failure& error) {
  return {{"schema", 1}, {"state", PublicDatasetStore::statusName(error.status)},
          {"error", error.error}, {"diskCommitted", false}, {"sessionApplied", false}};
}
template<class F> PublicDatasetResult attempt(F action) {
  try { return action(); }
  catch (const Failure& failure) { return {failure.status, failure.error, {}, {}, failureReceipt(failure)}; }
  catch (const std::exception&) { return {Status::IoError, QStringLiteral("公共资料处理异常，原版本未被替换"), {}, {}, {}}; }
}

class StoreIo {
public:
  StoreIo(QString root, PublicDatasetStoreOptions options) : root_(QDir::cleanPath(root)), options_(std::move(options)) {
    safeAbsolute(root_);
  }
  void cancel() const {
    if (options_.cancelled && options_.cancelled->load()) fail(QStringLiteral("公共资料操作已取消"), Status::Cancelled);
  }
  void boundary(const QString& name) const {
    if (options_.allowBoundary && !options_.allowBoundary(name)) fail(QStringLiteral("公共资料存储边界失败：%1").arg(name), Status::IoError);
  }
  QString path(const QString& relative) const {
    require(!relative.isEmpty() && !relative.startsWith(QLatin1Char('/')) && !relative.contains(QLatin1Char('\\')) &&
        !relative.contains(QLatin1Char(':')), QStringLiteral("公共资料相对路径无效"));
    for (const auto& part : relative.split(QLatin1Char('/')))
      require(!part.isEmpty() && part != QStringLiteral(".") && part != QStringLiteral(".."), QStringLiteral("公共资料路径越界"));
    const auto result = QDir(root_).filePath(relative);
    safeAbsolute(result);
    return result;
  }
  void directory(const QString& relative) const {
    const auto target = path(relative);
    require(QDir().mkpath(target), QStringLiteral("无法创建公共资料目录"), Status::IoError);
    safeAbsolute(target);
  }
  QByteArray readPath(const QString& target, qint64 maximum, bool cancellation = true) const {
    const bool resource = target.startsWith(QStringLiteral(":/kqpet/"));
    if (!resource) safeAbsolute(target);
    QFile file(target);
    require(file.open(QIODevice::ReadOnly) && file.size() > 0 && file.size() <= maximum,
            QStringLiteral("公共资料缺失或大小无效：%1").arg(target), Status::IoError);
    const qint64 expected = file.size();
    QByteArray result;
    result.reserve(static_cast<qsizetype>(expected));
    while (!file.atEnd()) {
      if (cancellation) cancel();
      const auto bytes = file.read(64 * 1024);
      require(!bytes.isEmpty(), QStringLiteral("读取公共资料失败"), Status::IoError);
      result.append(bytes);
      require(result.size() <= maximum, QStringLiteral("公共资料读取期间大小发生变化"));
    }
    require(result.size() == expected && file.size() == expected, QStringLiteral("公共资料读取不完整"));
    if (!resource) safeAbsolute(target);
    return result;
  }
  QByteArray read(const QString& relative, qint64 maximum, bool cancellation = true) const {
    return readPath(path(relative), maximum, cancellation);
  }
  QByteArray pointerBytes(bool cancellation = true) const {
    const auto target = path(QStringLiteral("public/current.json"));
    if (!QFileInfo::exists(target)) return {};
    return readPath(target, 16 * 1024, cancellation);
  }
  void requireInitialMigration() const {
    const auto transactions = path(QStringLiteral("public/transactions"));
    if (!QDir(transactions).exists()) return;
    QDirIterator directories(transactions, QDir::Dirs | QDir::NoDotAndDotDot);
    int count = 0;
    while (directories.hasNext()) {
      directories.next(); cancel();
      require(++count <= 10000, QStringLiteral("迁移恢复记录超过检查预算"));
      safeAbsolute(directories.filePath());
      const auto id = directories.fileName();
      if (!tokenValid(id)) continue;
      const auto value = receipt(id);
      require(value.value("diskCommitted") != QJsonValue(true) && value.value("sessionApplied") != QJsonValue(true),
              QStringLiteral("当前公共资料指针丢失，但已有已提交版本记录；保留资料并停止自动迁移"));
    }
  }
  QSet<QString> actualParserFiles() const {
    const QString directory = path(QStringLiteral("data-tools/scripts"));
    QSet<QString> result;
    QDirIterator top(directory, QStringList{QStringLiteral("*.py")}, QDir::Files | QDir::NoDotAndDotDot);
    while (top.hasNext()) {
      top.next(); safeAbsolute(top.filePath()); result.insert(top.fileName());
      require(result.size() <= 8192, QStringLiteral("解析器文件闭包超过检查预算"));
    }
    QStringList pending{QStringLiteral("battle_replay")}; int entries = 0;
    while (!pending.isEmpty()) {
      const auto relative = pending.takeLast();
      const auto current = path(QStringLiteral("data-tools/scripts/") + relative);
      if (!QDir(current).exists()) continue;
      QDirIterator children(current, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
      while (children.hasNext()) {
        children.next(); cancel(); safeAbsolute(children.filePath());
        require(++entries <= 16384, QStringLiteral("解析器目录闭包超过检查预算"));
        const auto name = relative + QLatin1Char('/') + children.fileName();
        if (children.fileInfo().isDir()) pending.append(name);
        else if (children.fileName().endsWith(QStringLiteral(".py"), Qt::CaseInsensitive)) result.insert(name);
      }
    }
    return result;
  }
  QJsonObject pointer(const QByteArray& bytes) const {
    if (bytes.isEmpty()) return {};
    const auto value = object(bytes);
    require(value.value("schema") == QJsonValue(1) && digestValid(value.value("revision").toString()) &&
            tokenValid(value.value("generation").toString()) && value.value("previousRevision").isString() &&
            (value.value("previousRevision").toString().isEmpty() || digestValid(value.value("previousRevision").toString())) &&
            (!value.contains("attempt") || tokenValid(value.value("attempt").toString())),
            QStringLiteral("当前公共资料指针损坏；不会自动覆盖为旧目录"));
    return value;
  }
  void write(const QString& relative, const QByteArray& bytes) const {
    directory(QFileInfo(relative).path());
    const auto target = path(relative);
    QSaveFile file(target);
    file.setDirectWriteFallback(false);
    require(file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit(),
            QStringLiteral("公共资料原子写入失败：%1").arg(relative), Status::IoError);
  }
  void immutable(const QString& relative, const QByteArray& bytes) const {
    directory(QFileInfo(relative).path());
    const auto target = path(relative);
    if (QFileInfo::exists(target)) {
      require(readPath(target, std::max<qint64>(bytes.size(), 1)) == bytes, QStringLiteral("不可变公共资料对象已有不同内容"));
      return;
    }
    QTemporaryFile temporary(QDir(QFileInfo(target).absolutePath()).filePath(QStringLiteral(".publish-XXXXXX")));
    require(temporary.open() && temporary.write(bytes) == bytes.size() && temporary.flush(), QStringLiteral("无法准备不可变公共资料对象"), Status::IoError);
    temporary.close();
    // QFile::rename never replaces an existing destination. A concurrent
    // writer can only win by providing these same content-addressed bytes.
    if (temporary.rename(target)) temporary.setAutoRemove(false);
    else
      require(QFileInfo::exists(target) && readPath(target, std::max<qint64>(bytes.size(), 1)) == bytes,
              QStringLiteral("公共资料对象发布失败或存在内容冲突：%1").arg(temporary.errorString()), Status::IoError);
  }
  PublicDatasetObject descriptor(const QJsonValue& value, const QString& key, bool attachment = false) const {
    require(value.isObject(), QStringLiteral("公共资料对象描述无效"));
    const auto record = value.toObject();
    PublicDatasetObject result;
    result.sha256 = record.value("sha256").toString();
    result.format = record.value("format").toString();
    result.origin = record.value("origin").toObject();
    const double length = record.value("bytes").toDouble(-1);
    const bool replay = key == QStringLiteral("battleReplay") && !attachment;
    const qint64 limit = replay ? BattleReplayCatalogPack::MaximumJsonBytes : JsonLimit;
    require(digestValid(result.sha256) && record.value("bytes").isDouble() && std::isfinite(length) && length > 0 &&
            length <= limit && std::floor(length) == length && !result.origin.value("kind").toString().isEmpty() &&
            (result.format == QStringLiteral("json") || (replay && result.format == QStringLiteral("brpack"))),
            QStringLiteral("公共资料对象散列、长度、格式或来源无效：%1").arg(key));
    result.bytes = static_cast<qint64>(length);
    result.path = path(QStringLiteral("public/objects/%1/data").arg(result.sha256));
    return result;
  }
  void retainedInputs(const PublicDatasetObject& record) const {
    for (const auto* name : {"rawObject", "embeddedObject"}) {
      if (!record.origin.contains(QLatin1String(name))) continue;
      auto value = record.origin.value(QLatin1String(name)).toObject();
      require(!value.isEmpty(), QStringLiteral("迁移原始资料对象描述无效"));
      value.insert("origin", QJsonObject{{"kind", "retained-migration-input"}});
      checkedObject(descriptor(value, QStringLiteral("migrationInput")));
    }
  }
  QByteArray checkedObject(const PublicDatasetObject& record, bool cancellation = true) const {
    const auto bytes = readPath(record.path, record.bytes, cancellation);
    require(bytes.size() == record.bytes && sha(bytes) == record.sha256, QStringLiteral("公共资料对象实际字节与清单不符"));
    if (record.format == QStringLiteral("brpack")) {
      require(bytes.size() >= BattleReplayCatalogPack::HeaderBytes && bytes.size() <= BattleReplayCatalogPack::MaximumPackBytes &&
              bytes.startsWith("KQBRPK1\n"), QStringLiteral("回放压缩对象标识无效"));
      const quint64 expanded = qFromBigEndian<quint64>(bytes.constData() + 8);
      require(expanded > 0 && expanded <= BattleReplayCatalogPack::MaximumJsonBytes &&
              qFromBigEndian<quint64>(bytes.constData() + 16) == quint64(bytes.size() - BattleReplayCatalogPack::HeaderBytes),
              QStringLiteral("回放压缩对象长度无效"));
    }
    return bytes;
  }
  QByteArray expanded(const PublicDatasetObject& record) const {
    auto bytes = checkedObject(record);
    if (record.format == QStringLiteral("brpack")) {
      BattleReplayCatalogPack::Decoder decoder(std::move(bytes));
      for (;;) {
        cancel();
        const auto state = decoder.step();
        require(state != BattleReplayCatalogPack::StepResult::Failed, decoder.error());
        if (state == BattleReplayCatalogPack::StepResult::Done) break;
      }
      bytes = decoder.takeJson();
    }
    return bytes;
  }
  QJsonObject decoded(const PublicDatasetObject& record, QByteArray* jsonBytes = nullptr) const {
    auto bytes = expanded(record);
    auto value = object(bytes);
    qint64 nodes = 0, characters = 0;
    require(bounds(value, nodes, characters), QStringLiteral("公共资料超过结构预算"));
    if (jsonBytes) *jsonBytes = std::move(bytes);
    return value;
  }
  PublicDatasetObject put(const QByteArray& bytes, const QString& format, const QJsonObject& origin) const {
    PublicDatasetObject result{sha(bytes), bytes.size(), format, origin, {}};
    const auto relative = QStringLiteral("public/objects/%1/data").arg(result.sha256);
    immutable(relative, bytes);
    result.path = path(relative);
    return result;
  }
  QJsonObject manifest(const QString& revision) const {
    require(digestValid(revision), QStringLiteral("公共资料版本身份无效"));
    const auto bytes = read(QStringLiteral("public/releases/%1/manifest.json").arg(revision), 2 * 1024 * 1024);
    require(sha(bytes) == revision, QStringLiteral("公共资料版本清单散列不匹配"));
    const auto root = object(bytes);
    require(root.value("schema") == QJsonValue(1) && root.value("dependencySchema") == QJsonValue(1) &&
            root.value("components").isObject() && root.value("attachments").isObject(), QStringLiteral("公共资料版本格式不兼容"));
    const auto components = root.value("components").toObject();
    require(components.size() == PublicDatasetStore::componentKeys().size(), QStringLiteral("公共资料版本必须含全部七类资料"));
    for (const auto& key : PublicDatasetStore::componentKeys()) descriptor(components.value(key), key);
    const auto attachments = root.value("attachments").toObject();
    for (auto item = attachments.begin(); item != attachments.end(); ++item) {
      require(PublicDatasetStore::attachmentNames().contains(item.key()), QStringLiteral("公共资料含未允许的附件路径"));
      descriptor(item.value(), item.key(), true);
    }
    return root;
  }
  PublicDatasetSnapshotHandle prepare(const QString& revision, bool replay) const {
    cancel();
    auto snapshot = std::make_shared<PublicDatasetSnapshot>();
    snapshot->revision = revision;
    snapshot->manifest = manifest(revision);
    const auto components = snapshot->manifest.value("components").toObject();
    for (const auto& key : PublicDatasetStore::componentKeys()) {
      cancel();
      const auto ref = descriptor(components.value(key), key);
      retainedInputs(ref);
      snapshot->components.insert(key, ref);
      if (key == QStringLiteral("battleReplay")) {
        snapshot->replayObject = ref;
        if (replay && !unavailable(ref)) snapshot->replay = replaySnapshot(ref);
        else checkedObject(ref); // Small brpack bytes are checked; expansion stays deferred.
        continue;
      }
      const auto root = decoded(ref);
      const auto label = sourceLabel(ref);
      const auto source = root.value("source").toObject();
      QDateTime updated = QDateTime::fromString(source.value("updatedUtc").toString(), Qt::ISODate);
      QString error;
      if (unavailable(ref)) {
        require(root.value("available") == QJsonValue(false) && !root.value("reason").toString().isEmpty(),
                QStringLiteral("不可用资料占位缺少明确说明"));
        if (key == QStringLiteral("pets")) snapshot->detail = std::make_shared<PetDetailCatalogSnapshot>();
        else if (key == QStringLiteral("skills")) snapshot->skill = std::make_shared<PetSkillCatalogSnapshot>();
        else if (key == QStringLiteral("shop")) snapshot->shop = std::make_shared<ShopCatalogSnapshot>();
        else if (key == QStringLiteral("routines")) snapshot->routine = std::make_shared<RoutineCatalogSnapshot>();
        continue;
      }
      if (key == QStringLiteral("pets")) {
        // A version object is complete. Never merge it with this executable's
        // potentially newer embedded baseline when retaining an old version.
        auto empty = std::make_shared<PetDetailCatalogSnapshot>(); empty->loaded = true;
        snapshot->detail = PetDetailCatalog::prepareOverlay(empty, root, label, updated, &error);
        require(bool(snapshot->detail), error);
      } else if (key == QStringLiteral("skills")) {
        snapshot->skill = PetSkillCatalog::prepare(root, label, updated, &error);
        require(bool(snapshot->skill), error);
      } else if (key == QStringLiteral("shop")) {
        snapshot->shop = ShopExchangeCatalog::prepare(root, label, updated, &error);
        require(bool(snapshot->shop), error);
      } else if (key == QStringLiteral("routines")) {
        snapshot->routine = RoutineOverviewCatalog::prepare(root, label, updated, &error);
        require(bool(snapshot->routine), error);
      } else if (key == QStringLiteral("images")) {
        require(root.value("schemaVersion") == QJsonValue(1) && root.value("images").isObject(), QStringLiteral("精灵图片索引结构无效"));
        snapshot->imageIndex = root;
      } else {
        require(root.value("schemaVersion") == QJsonValue(1) && root.value("stargods").isObject(), QStringLiteral("公共图标索引结构无效"));
        snapshot->iconIndex = root;
      }
    }
    const auto attachments = snapshot->manifest.value("attachments").toObject();
    for (auto item = attachments.begin(); item != attachments.end(); ++item) {
      const auto ref = descriptor(item.value(), item.key(), true);
      decoded(ref);
      snapshot->attachments.insert(item.key(), ref);
    }
    return snapshot;
  }
  BattleReplayCatalogHandle replaySnapshot(const PublicDatasetObject& ref) const {
    QByteArray bytes;
    const auto root = decoded(ref, &bytes);
    const QString error = validateBattleReplayCatalog(root);
    require(error.isEmpty(), error);
    auto result = std::make_shared<BattleReplayCatalogSnapshot>();
    result->root = root; result->sha256 = sha(bytes); result->sourcePath = ref.path;
    result->sourceLabel = sourceLabel(ref); result->loadedAt = QDateTime::currentDateTimeUtc();
    result->embedded = ref.origin.value("kind") == QStringLiteral("embedded-baseline");
    bool valid = false; result->revision = ref.sha256.left(15).toULongLong(&valid, 16);
    return result;
  }
  QJsonObject receipt(const QString& attemptId) const {
    require(tokenValid(attemptId), QStringLiteral("公共资料事务身份无效"));
    const QString relative = QStringLiteral("public/transactions/%1/receipt.json").arg(attemptId);
    if (!QFileInfo::exists(path(relative))) return {};
    return object(read(relative, 2 * 1024 * 1024, false));
  }
  void receipt(const QString& attemptId, QJsonObject value) const {
    require(tokenValid(attemptId), QStringLiteral("公共资料事务身份无效"));
    const auto now = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const auto previous = receipt(attemptId);
    auto created = previous.value("createdUtc").toString();
    if (created.isEmpty()) created = previous.value("recordedUtc").toString();
    if (created.isEmpty()) {
      const auto requestPath = path(QStringLiteral("public/transactions/%1/request.json").arg(attemptId));
      if (QFileInfo::exists(requestPath)) {
        // Request metadata can improve ordering, but a damaged request must
        // never prevent recording an already committed pointer or its ack.
        try {
          const auto request = object(readPath(requestPath, 2 * 1024 * 1024, false));
          const auto requested = request.value("createdUtc").toString();
          if (request.value("schema") == QJsonValue(1) && request.value("attempt") == attemptId &&
              QDateTime::fromString(requested, Qt::ISODateWithMs).isValid()) created = requested;
        } catch (const Failure&) {}
      }
    }
    value.insert("createdUtc", created.isEmpty() ? now : created);
    value.insert("schema", 1); value.insert("attempt", attemptId);
    value.insert("recordedUtc", now);
    write(QStringLiteral("public/transactions/%1/receipt.json").arg(attemptId), json(value));
    write(QStringLiteral("public/reports/latest.json"), json(value));
  }
  QJsonObject recoveredReceipt(const QJsonObject& current) const {
    const auto id = current.value("attempt").toString();
    if (!tokenValid(id)) return {};
    auto value = receipt(id);
    if (value.value("revision") == current.value("revision") && value.value("diskCommitted") == QJsonValue(true) &&
        value.value("plannedGeneration") == current.value("generation")) {
      value.insert("attempt", id); return value;
    }
    if (value.value("revision") != current.value("revision") ||
        (!value.value("plannedGeneration").toString().isEmpty() && value.value("plannedGeneration") != current.value("generation")))
      value = {{"revision", current.value("revision")}, {"checked", true}, {"candidatePassed", true}, {"sessionApplied", false}};
    value.insert("diskCommitted", true);
    value.insert("state", value.value("sessionApplied") == QJsonValue(true) ? QStringLiteral("sessionApplied") : QStringLiteral("diskCommitted"));
    value.insert("attempt", id);
    // Recovery is a durable fact derived from the atomic pointer, not from a
    // candidate's claim that it was applied before a process interruption.
    receipt(id, value);
    return value;
  }
  QString root() const { return root_; }
private:
  QString root_;
  PublicDatasetStoreOptions options_;
};
}

PublicDatasetStore::PublicDatasetStore(QString dataRoot, PublicDatasetStoreOptions options)
    : root_(QDir::cleanPath(dataRoot)), options_(std::move(options)) {}

QStringList PublicDatasetStore::componentKeys() {
  return {QStringLiteral("pets"), QStringLiteral("skills"), QStringLiteral("shop"), QStringLiteral("images"),
          QStringLiteral("icons"), QStringLiteral("routines"), QStringLiteral("battleReplay")};
}
QStringList PublicDatasetStore::attachmentNames() {
  return {QStringLiteral("activity-exchange-data.json"), QStringLiteral("activity-evolution-data.json")};
}
QString PublicDatasetStore::statusName(PublicDatasetStatus status) {
  switch (status) {
    case Status::Ready: return QStringLiteral("ready");
    case Status::NotFound: return QStringLiteral("notFound");
    case Status::Invalid: return QStringLiteral("invalid");
    case Status::Conflict: return QStringLiteral("conflict");
    case Status::Cancelled: return QStringLiteral("cancelled");
    case Status::Busy: return QStringLiteral("busy");
    case Status::IoError: return QStringLiteral("ioError");
    case Status::DiskCommitted: return QStringLiteral("diskCommitted");
    case Status::SessionApplied: return QStringLiteral("sessionApplied");
  }
  return QStringLiteral("invalid");
}

PublicDatasetResult PublicDatasetStore::prepareCurrent(bool loadReplay) const {
  return attempt([&] {
    StoreIo io(root_, options_);
    const auto bytes = io.pointerBytes();
    require(!bytes.isEmpty(), QStringLiteral("尚未建立共享公共资料版本"), Status::NotFound);
    const auto current = io.pointer(bytes);
    const auto snapshot = io.prepare(current.value("revision").toString(), loadReplay);
    require(io.pointerBytes() == bytes, QStringLiteral("读取期间公共资料已切换，请重新加载"), Status::Conflict);
    return PublicDatasetResult{Status::Ready, {}, snapshot, {}, io.recoveredReceipt(current)};
  });
}

PublicDatasetResult PublicDatasetStore::loadReplay(PublicDatasetSnapshotHandle snapshot) const {
  return attempt([&] {
    require(bool(snapshot), QStringLiteral("公共资料快照为空"));
    StoreIo io(root_, options_);
    const auto manifest = io.manifest(snapshot->revision);
    require(manifest == snapshot->manifest, QStringLiteral("已固定的公共资料清单发生变化"));
    const auto ref = io.descriptor(manifest.value("components").toObject().value("battleReplay"), QStringLiteral("battleReplay"));
    require(!unavailable(ref), QStringLiteral("此资料版本没有可用回放资料"));
    auto result = std::make_shared<PublicDatasetSnapshot>(*snapshot);
    if (!result->replay) result->replay = io.replaySnapshot(ref);
    return PublicDatasetResult{Status::Ready, {}, result, {}, {}};
  });
}

namespace {
void noFailedFindings(const QJsonObject& report, const QString& label) {
  require(report.value("passed") == QJsonValue(true) && report.value("findings").isArray(), label + QStringLiteral("未通过或缺少明细"));
  for (const auto& value : report.value("findings").toArray()) {
    const auto row = value.toObject();
    require(value.isObject() && row.value("severity") == QStringLiteral("info") &&
            row.value("kind").isString() && row.value("path").isString(), label + QStringLiteral("含错误或仍需核对的项"));
  }
}
void quality(StoreIo& io, const QString& transaction, const QString& key,
             const QJsonObject& evidence, const PublicDatasetObject& ref, const QJsonArray& evidenceFiles) {
  const auto file = evidence.value("file").toString();
  require(file == QStringLiteral("quality/%1.json").arg(key), QStringLiteral("独立质量报告路径不符合已登记位置"));
  const auto bytes = io.read(transaction + QLatin1Char('/') + file, JsonLimit);
  require(evidence.value("bytes") == QJsonValue(bytes.size()) && evidence.value("sha256") == sha(bytes),
          QStringLiteral("独立质量报告字节与候选绑定不符"));
  const auto report = object(bytes);
  QString content, payloadSha;
  {
    // The token reader validates the same root/depth/node/string budgets and
    // JSON scalar syntax while preserving floating token identity. Building
    // a complete QJson tree just to discard it before this pass doubled work
    // for large replay candidates. Snapshot readers still validate their
    // complete prepared types below, and commit rereads actual object bytes.
    const auto payload = io.expanded(ref);
    content = qualityDigest(payload, [&io] { io.cancel(); });
    if (key == QStringLiteral("battleReplay")) payloadSha = sha(payload);
  }
  require(evidence.value("candidateContentSha256") == content && report.value("candidateContentSha256") == content &&
          report.value("component") == key && report.value("schemaVersion") == QJsonValue(1),
          QStringLiteral("独立质量报告不对应实际候选内容：%1").arg(key));
  noFailedFindings(report, QStringLiteral("公共资料质量检查"));
  const auto summary = report.value("summary").toObject();
  require(summary.value("errors") == QJsonValue(0) && summary.value("reviewRequired") == QJsonValue(0) &&
          report.value("decision") == QStringLiteral("validated-not-yet-applied"), QStringLiteral("公共资料质量决定不允许发布"));
  if (key == QStringLiteral("skills") || key == QStringLiteral("battleReplay")) {
    const auto fidelity = report.value("sourceFidelity").toObject();
    require(fidelity.value("schemaVersion") == QJsonValue(1) && fidelity.value("candidateContentSha256") == content &&
            ((fidelity.value("inputs").isArray() && !fidelity.value("inputs").toArray().isEmpty()) ||
             (fidelity.value("inputs").isObject() && !fidelity.value("inputs").toObject().isEmpty())),
            QStringLiteral("缺少绑定实际官方输入的原文核对报告"));
    noFailedFindings(fidelity, QStringLiteral("独立官方原文核对"));
    QJsonArray inputs;
    if (fidelity.value("inputs").isArray()) inputs = fidelity.value("inputs").toArray();
    else {
      const auto rows = fidelity.value("inputs").toObject();
      for (auto row = rows.begin(); row != rows.end(); ++row) {
        auto value = row.value().toObject(); if (!value.contains("role")) value.insert("role", row.key());
        inputs.append(value);
      }
    }
    int readInputs = 0;
    for (const auto& input : inputs) {
      const auto row = input.toObject();
      if (!row.contains("sha256") && row.value("status") != QStringLiteral("read")) continue;
      const auto digest = row.value("sha256").toString();
      require(digestValid(digest) && !row.value("path").toString().isEmpty() && row.value("bytes").toDouble() > 0,
              QStringLiteral("原文核对的实际输入身份无效"));
      bool found = false;
      for (const auto& saved : evidenceFiles) {
        const auto record = saved.toObject();
        if (record.value("component") != key || record.value("originalPath") != row.value("path") || record.value("role") != row.value("role")) continue;
        require(!found && record.value("sha256") == digest && record.value("bytes") == row.value("bytes") &&
                record.value("file") == QStringLiteral("public/source-cache/evidence/%1/data").arg(digest),
                QStringLiteral("官方原文的保留证据与核对报告不符"));
        const auto bytes = io.read(record.value("file").toString(), JsonLimit);
        require(sha(bytes) == digest && record.value("bytes") == QJsonValue(bytes.size()), QStringLiteral("已核官方原文文件缺失或字节被改变"));
        found = true;
      }
      require(found, QStringLiteral("独立核对报告没有保留其实际官方原文")); ++readInputs;
    }
    require(readInputs > 0, QStringLiteral("原文核对报告没有实际读取的输入"));
  }
  if (key == QStringLiteral("battleReplay")) {
    const auto regression = report.value("replayRegression").toObject();
    const auto native = regression.value("nativeReport").toObject();
    const auto testedFile = regression.value("catalogFileSha256").toString();
    // Rebuilds run the real report regression before lossless packing; an
    // unchanged pack may itself be the tested input. Both routes must bind
    // these exact expanded bytes, and the two report layers must agree which
    // physical input was tested. The final packed object was checked above.
    require(regression.value("passed") == QJsonValue(true) && regression.value("candidateContentSha256") == content &&
            (testedFile == ref.sha256 || testedFile == payloadSha) &&
            regression.value("catalogSha256") == payloadSha && regression.value("processExitCode") == QJsonValue(0) &&
            regression.value("errors").isArray() && regression.value("errors").toArray().isEmpty() &&
            native.value("passed") == QJsonValue(true) && native.value("schemaVersion") == QJsonValue(1) &&
            native.value("mode") == QStringLiteral("candidate-regression") && native.value("catalogFileSha256") == testedFile &&
            native.value("catalogSha256") == payloadSha &&
            native.value("errors").isArray() && native.value("errors").toArray().isEmpty(),
            QStringLiteral("原生真实战报回归没有通过或与实际回放对象不符"));
    const auto manifestBytes = io.read(QStringLiteral("data-tools/scripts/battle-replay-regression-manifest.json"), 2 * 1024 * 1024);
    require(regression.value("fixtureSetSha256") == sha(manifestBytes) && native.value("fixtureSetSha256") == sha(manifestBytes),
            QStringLiteral("战报回归使用的依据不符合当前检查器"));
    const auto cases = object(manifestBytes).value("cases").toArray();
    const auto actual = native.value("cases").toArray();
    require(!cases.isEmpty() && actual.size() == cases.size() && native.value("caseCount") == QJsonValue(cases.size()),
            QStringLiteral("战报回归未覆盖全部固定案例"));
    for (qsizetype i = 0; i < cases.size(); ++i) {
      const auto expected = cases.at(i).toObject(), tested = actual.at(i).toObject();
      require(tested.value("passed") == QJsonValue(true) && tested.value("id") == expected.value("id") &&
              tested.value("fixtureSha256") == expected.value("sha256"), QStringLiteral("战报回归案例未通过或使用了其他战报"));
    }
    require(regression.value("checkerSha256") == sha(io.read(QStringLiteral("data-tools/scripts/KQBattleReplayUpdateCheck.exe"), 32 * 1024 * 1024)),
            QStringLiteral("战报回归检查器与当前工具不一致"));
    const auto runtimes = regression.value("runtimeSha256").toObject();
    require(!runtimes.isEmpty(), QStringLiteral("战报回归未登记运行库身份"));
    for (auto library = runtimes.begin(); library != runtimes.end(); ++library) {
      require(!library.key().contains('/') && !library.key().contains('\\') && library.key().endsWith(QStringLiteral(".dll")),
              QStringLiteral("战报回归运行库路径无效"));
      require(library.value() == sha(io.read(QStringLiteral("data-tools/scripts/") + library.key(), 32 * 1024 * 1024)),
              QStringLiteral("战报回归运行库与核对报告不符"));
    }
  }
}

PublicDatasetPreparedHandle candidate(StoreIo& io, const QString& attemptId) {
  require(tokenValid(attemptId), QStringLiteral("公共资料候选事务身份无效"));
  const auto transaction = QStringLiteral("public/transactions/%1").arg(attemptId);
  const auto root = object(io.read(transaction + QStringLiteral("/candidate.json"), 2 * 1024 * 1024));
  require(root.value("schema") == QJsonValue(1) && root.value("attempt") == attemptId,
          QStringLiteral("公共资料候选事务不匹配"));
  const auto revision = root.value("revision").toString();
  const auto base = root.value("baseRevision").toString();
  const auto baseSha = root.value("baseCurrentSha256").toString();
  require(digestValid(revision) && digestValid(base) && digestValid(baseSha),
          QStringLiteral("更新候选需要已迁移的完整公共资料基线"));
  const auto currentBytes = io.pointerBytes();
  const auto current = io.pointer(currentBytes);
  require(current.value("revision") == base && sha(currentBytes) == baseSha,
          QStringLiteral("资料已被另一更新切换，请基于当前版本重新检查"), Status::Conflict);
  const auto manifest = io.manifest(revision), previous = io.manifest(base);
  const auto validation = root.value("validation").toObject();
  const auto validationBytes = io.read(transaction + QStringLiteral("/validation.json"), JsonLimit);
  const auto report = object(validationBytes);
  require(validation.value("passed") == QJsonValue(true) && validation.value("manifestSha256") == revision &&
          validation.value("reportSha256") == sha(validationBytes) && report.value("passed") == QJsonValue(true),
          QStringLiteral("公共资料候选验证报告未通过或散列不符"));
  for (const auto* field : {"attempt", "baseRevision", "baseCurrentSha256", "revision", "requestedComponents", "effectiveComponents", "results"})
    require(report.value(QLatin1String(field)) == root.value(QLatin1String(field)), QStringLiteral("公共资料候选和检查报告记录不一致"));
  require(report.value("schema") == QJsonValue(1) && report.value("components") == manifest.value("components") &&
          report.value("attachments") == manifest.value("attachments"), QStringLiteral("候选检查报告未绑定全部实际对象"));
  const auto checker = report.value("checker").toObject();
  require(validation.value("checker") == checker && checker.value("kind") == QStringLiteral("public-dataset-python") &&
          checker.value("schema") == QJsonValue(1) && checker.value("sha256") ==
          sha(io.read(QStringLiteral("data-tools/scripts/public_dataset_store.py"), JsonLimit)),
          QStringLiteral("公共资料候选检查器身份不符"));
  const auto parser = report.value("parserIdentity").toObject();
  const auto parserFiles = parser.value("files").toObject();
  require(!parserFiles.isEmpty() && parser.value("formatSchema") == QJsonValue(1) && parser.value("sha256") == sha(json(parserFiles)),
          QStringLiteral("公共资料解析规则身份不完整"));
  const auto fileNames = parserFiles.keys();
  require(QSet<QString>(fileNames.begin(), fileNames.end()) == io.actualParserFiles(),
          QStringLiteral("公共资料检查报告没有绑定当前解析器的完整文件闭包"));
  for (auto file = parserFiles.begin(); file != parserFiles.end(); ++file) {
    require(file.key().endsWith(QStringLiteral(".py")) && file.value().isString(), QStringLiteral("公共资料解析器文件身份无效"));
    require(file.value() == sha(io.read(QStringLiteral("data-tools/scripts/") + file.key(), JsonLimit)),
            QStringLiteral("公共资料解析规则在候选检查后发生变化"));
  }
  const auto checks = report.value("checks").toObject();
  for (const auto* field : {"objects", "dependencies", "sourceStable"})
    require(checks.value(QLatin1String(field)) == QJsonValue(true), QStringLiteral("公共资料候选缺少必要检查"));
  const auto requested = stringSet(root.value("requestedComponents"));
  const auto effective = stringSet(root.value("effectiveComponents"));
  require(!requested.isEmpty() && (requested - effective).isEmpty(), QStringLiteral("候选没有处理用户选择的全部组件"));
  const QSet<QString> core{QStringLiteral("pets"), QStringLiteral("skills"), QStringLiteral("shop"), QStringLiteral("battleReplay")};
  const QSet<QString> pictures{QStringLiteral("images"), QStringLiteral("icons")};
  if (!(effective & (core | pictures)).isEmpty())
    require(((core | pictures) - effective).isEmpty(), QStringLiteral("公共资料候选没有完整扩展已登记依赖"));
  const auto refs = manifest.value("components").toObject(), prior = previous.value("components").toObject();
  const auto results = root.value("results").toObject();
  require(results.size() == effective.size(), QStringLiteral("公共资料候选组件结果不完整"));
  bool rejectedCore = false;
  for (const auto& key : effective) {
    const auto row = results.value(key).toObject();
    const auto status = row.value("status").toString();
    require(status == QStringLiteral("updated") || status == QStringLiteral("unchanged") ||
            status == QStringLiteral("retained"), QStringLiteral("公共资料候选含未完成或未知结果"));
    if (status == QStringLiteral("retained") || row.value("attemptStatus") == QStringLiteral("failed")) {
      require(refs.value(key) == prior.value(key), QStringLiteral("失败保旧组件却改变了公共资料对象"));
      rejectedCore |= core.contains(key);
    }
  }
  if (rejectedCore) for (const auto& key : core | pictures)
    require(refs.value(key) == prior.value(key), QStringLiteral("共享定义更新失败后混入了不一致派生资料"));
  QSet<QString> changed;
  for (const auto& key : PublicDatasetStore::componentKeys()) {
    if (!effective.contains(key)) require(refs.value(key) == prior.value(key), QStringLiteral("候选改写了检查范围外的组件"));
    if (refs.value(key).toObject().value("sha256") != prior.value(key).toObject().value("sha256")) {
      require(refs.value(key).toObject().value("origin").toObject().value("kind") != QStringLiteral("unavailable"),
              QStringLiteral("更新候选不能把完整资料降级为空占位"));
      changed.insert(key);
    }
  }
  require(changed == stringSet(report.value("changedComponents")), QStringLiteral("公共资料实际变更范围与报告不符"));
  if (!effective.contains(QStringLiteral("shop")) || rejectedCore)
    require(manifest.value("attachments") == previous.value("attachments"), QStringLiteral("未更新商店却改写了商店附件"));
  // A claim in candidate.validation is insufficient: independently read each
  // original quality report, including its source and native replay audits.
  const auto reports = report.value("qualityReports").toObject();
  const auto preparedEvidenceFiles = report.value("evidenceFiles").toArray();
  const QStringList checkedKinds{QStringLiteral("pets"), QStringLiteral("skills"), QStringLiteral("battleReplay")};
  for (const auto& key : checkedKinds) {
    const auto beforeOrigin = prior.value(key).toObject().value("origin").toObject();
    const auto afterOrigin = refs.value(key).toObject().value("origin").toObject();
    if (changed.contains(key) || beforeOrigin.value("parserSha256") != afterOrigin.value("parserSha256"))
      require(reports.value(key).isObject(), QStringLiteral("变化或重新核对解析规则的核心组件没有独立质量报告"));
  }
  for (auto report = reports.begin(); report != reports.end(); ++report) {
    require(checkedKinds.contains(report.key()), QStringLiteral("候选含未登记质量检查报告"));
    quality(io, transaction, report.key(), report.value().toObject(), io.descriptor(refs.value(report.key()), report.key()),
            preparedEvidenceFiles);
  }
  auto prepared = std::make_shared<PublicDatasetPrepared>();
  prepared->snapshot = io.prepare(revision, changed.contains(QStringLiteral("battleReplay")));
  prepared->attempt = attemptId; prepared->baseRevision = base; prepared->baseCurrentSha256 = baseSha;
  prepared->kind = QStringLiteral("update"); prepared->validation = report; prepared->candidate = root;
  return prepared;
}

PublicDatasetPreparedHandle nativePrepared(StoreIo& io, const QString& revision, const QByteArray& oldBytes,
                                          const QString& kind, bool replay) {
  const auto current = io.pointer(oldBytes);
  auto value = std::make_shared<PublicDatasetPrepared>();
  value->snapshot = io.prepare(revision, replay);
  if (replay && value->snapshot->replay) {
    auto deferred = std::make_shared<PublicDatasetSnapshot>(*value->snapshot);
    deferred->replay.reset(); value->snapshot = std::move(deferred);
  }
  value->attempt = token(); value->kind = kind;
  value->baseRevision = current.value("revision").toString();
  value->baseCurrentSha256 = oldBytes.isEmpty() ? QString{} : sha(oldBytes);
  value->validation = {{"checker", "native-public-dataset-store"}, {"schema", 1}, {"passed", true}};
  value->candidate = {{"schema", 1}, {"attempt", value->attempt}, {"kind", kind}, {"baseRevision", value->baseRevision},
      {"baseCurrentSha256", value->baseCurrentSha256}, {"revision", revision}, {"validation", value->validation}};
  io.write(QStringLiteral("public/transactions/%1/candidate.json").arg(value->attempt), json(value->candidate));
  io.receipt(value->attempt, {{"revision", revision}, {"kind", kind}, {"checked", true}, {"candidatePassed", true},
                            {"diskCommitted", false}, {"sessionApplied", false}, {"state", "candidatePassed"}});
  return value;
}
}

PublicDatasetResult PublicDatasetStore::prepareCandidate(const QString& id) const {
  auto result = attempt([&] {
    StoreIo io(root_, options_);
    require(tokenValid(id), QStringLiteral("公共资料候选事务身份无效"));
    const auto current = io.pointer(io.pointerBytes());
    if (current.value("attempt") == id) {
      const auto root = object(io.read(QStringLiteral("public/transactions/%1/candidate.json").arg(id), 2 * 1024 * 1024));
      require(root.value("revision") == current.value("revision"), QStringLiteral("已提交事务与当前指针不一致"));
      return PublicDatasetResult{Status::DiskCommitted, {}, io.prepare(current.value("revision").toString(), false), {}, io.recoveredReceipt(current)};
    }
    auto prepared = candidate(io, id);
    auto receipt = io.receipt(id);
    receipt.insert("revision", prepared->snapshot->revision); receipt.insert("checked", true);
    receipt.insert("candidatePassed", true); receipt.insert("diskCommitted", false);
    receipt.insert("sessionApplied", false); receipt.insert("state", "candidatePassed");
    receipt.insert("results", prepared->candidate.value("results"));
    io.receipt(id, receipt);
    return PublicDatasetResult{Status::Ready, {}, prepared->snapshot, prepared, io.receipt(id)};
  });
  if (!result.ok() && tokenValid(id)) {
    const auto recorded = attempt([&] {
      StoreIo io(root_, {}); auto receipt = io.receipt(id);
      if (receipt.value("diskCommitted") == QJsonValue(true)) {
        receipt.insert("lastRetryError", result.error); io.receipt(id, receipt);
        return PublicDatasetResult{Status::Ready, {}, {}, {}, io.receipt(id)};
      }
      receipt.insert("attempt", id); receipt.insert("checked", true); receipt.insert("candidatePassed", false);
      receipt.insert("diskCommitted", false); receipt.insert("sessionApplied", false);
      receipt.insert("state", statusName(result.status)); receipt.insert("error", result.error);
      io.receipt(id, receipt);
      return PublicDatasetResult{Status::Ready, {}, {}, {}, io.receipt(id)};
    });
    if (recorded.ok()) result.receipt = recorded.receipt;
  }
  return result;
}

PublicDatasetResult PublicDatasetStore::prepareMigration() const {
  return attempt([&] {
    StoreIo io(root_, options_);
    const auto currentBytes = io.pointerBytes();
    if (!currentBytes.isEmpty()) {
      const auto current = io.pointer(currentBytes);
      return PublicDatasetResult{Status::Ready, {}, io.prepare(current.value("revision").toString(), false), {}, io.recoveredReceipt(current)};
    }
    io.requireInitialMigration();
    QJsonObject refs, attachments;
    for (const auto& key : componentKeys()) {
      io.cancel();
      QString file = componentFile(key), format = QStringLiteral("json");
      if (key == QStringLiteral("battleReplay") && QFileInfo::exists(io.path(QStringLiteral("catalog/battle-replay-catalog.brpack")))) {
        file = QStringLiteral("battle-replay-catalog.brpack"); format = QStringLiteral("brpack");
      }
      const auto localPath = io.path(QStringLiteral("catalog/") + file);
      QByteArray bytes;
      QJsonObject origin;
      if (QFileInfo::exists(localPath)) {
        bytes = io.readPath(localPath, key == QStringLiteral("battleReplay") ? BattleReplayCatalogPack::MaximumJsonBytes : JsonLimit);
        origin = {{"kind", "legacy-unregistered"}, {"relativePath", QStringLiteral("catalog/") + file}, {"originalSha256", sha(bytes)},
                  {"originalModifiedUtc", QFileInfo(localPath).lastModified().toUTC().toString(Qt::ISODateWithMs)}};
        if (format == QStringLiteral("json")) {
          const auto source = object(bytes).value("source").toObject();
          if (!source.isEmpty()) { origin.insert("kind", "legacy-recorded-source"); origin.insert("source", source); }
        }
      } else {
        const auto embedded = QStringLiteral(":/kqpet/") + file;
        QString exportedFile = file;
        if (key == QStringLiteral("battleReplay") && QFileInfo::exists(io.path(QStringLiteral("data-tools/scripts/baseline/battle-replay-catalog.brpack")))) {
          exportedFile = QStringLiteral("battle-replay-catalog.brpack"); format = QStringLiteral("brpack");
        }
        const auto exported = io.path(QStringLiteral("data-tools/scripts/baseline/") + exportedFile);
        if (QFileInfo::exists(exported)) {
          bytes = io.readPath(exported, key == QStringLiteral("battleReplay") ? BattleReplayCatalogPack::MaximumJsonBytes : JsonLimit);
          origin = {{"kind", "embedded-baseline"}, {"resource", QStringLiteral("data-tools/scripts/baseline/") + exportedFile}, {"sha256", sha(bytes)}};
        } else if (key == QStringLiteral("routines")) bytes = json(RoutineOverviewCatalog::embeddedRoot());
        else if (key == QStringLiteral("battleReplay") && QFileInfo::exists(QStringLiteral(":/kqpet/battle-replay-catalog.json"))) {
          const auto staging = QStringLiteral("public/transactions/%1").arg(token()); io.directory(staging);
          const auto packed = io.path(staging + QStringLiteral("/embedded.brpack"));
          const auto error = BattleReplayCatalogPack::exportEmbeddedBaseline(packed, options_.cancelled);
          require(error.isEmpty(), error);
          bytes = io.readPath(packed, BattleReplayCatalogPack::MaximumPackBytes); format = QStringLiteral("brpack");
        } else if (QFileInfo::exists(embedded)) bytes = io.readPath(embedded, JsonLimit);
        if (!bytes.isEmpty()) {
          if (origin.isEmpty()) origin = {{"kind", "embedded-baseline"}, {"resource", key == QStringLiteral("routines") ? QStringLiteral("RoutineOverviewCatalog::embeddedRoot") : embedded}, {"sha256", sha(bytes)}};
        } else {
          require(key == QStringLiteral("images") || key == QStringLiteral("icons"),
                  QStringLiteral("缺少 %1 的现用资料和实际内置基线；请先导出完整工作台基线").arg(key));
          const QString reason = QStringLiteral("没有现用文件，也没有此类资料的可用内置基线");
          bytes = json({{"schema", 1}, {"available", false}, {"reason", reason}});
          origin = {{"kind", "unavailable"}, {"reason", reason}};
        }
      }
      if (key == QStringLiteral("pets") && origin.value("kind") != QStringLiteral("unavailable")) {
        origin.insert("rawObject", inputRef(io.put(bytes, QStringLiteral("json"), {{"kind", "retained-migration-input"}})));
        const auto root = object(bytes);
        auto embeddedPath = QStringLiteral(":/kqpet/pet-detail-data.json");
        if (!QFileInfo::exists(embeddedPath)) embeddedPath = io.path(QStringLiteral("data-tools/scripts/baseline/pet-detail-data.json"));
        auto baseline = std::make_shared<PetDetailCatalogSnapshot>(); baseline->loaded = true;
        if (QFileInfo::exists(embeddedPath)) {
          const auto baseBytes = io.readPath(embeddedPath, JsonLimit);
          baseline->root = object(baseBytes); baseline->contentDigest = QByteArray::fromHex(sha(baseBytes).toLatin1());
          origin.insert("embeddedBaselineSha256", sha(baseBytes));
          origin.insert("embeddedObject", inputRef(io.put(baseBytes, QStringLiteral("json"), {{"kind", "retained-embedded-input"}})));
        }
        QString error;
        auto complete = PetDetailCatalog::prepareOverlay(baseline, root, sourceLabel(PublicDatasetObject{{}, 0, {}, origin, {}}), {}, &error);
        const auto incomingDate = CatalogLegacyCompatibility::officialPetMetadataDate(root);
        const auto baselineDate = CatalogLegacyCompatibility::officialPetMetadataDate(baseline->root);
        if (incomingDate.isValid() && baselineDate.isValid() && incomingDate < baselineDate) {
          complete = PetDetailCatalog::prepareOverlay(baseline, baseline->root, QStringLiteral("迁移时沿用已启用的较新内置资料"), {}, &error);
          origin.insert("kind", "legacy-superseded-by-embedded");
          origin.insert("compatibilityRule", "existing-official-pet-date-guard");
        }
        require(bool(complete), error);
        bytes = json(complete->root);
      }
      if (key == QStringLiteral("shop") && origin.value("kind") != QStringLiteral("unavailable")) {
        auto baselinePath = QStringLiteral(":/kqpet/shop-exchange-data.json");
        if (!QFileInfo::exists(baselinePath)) baselinePath = io.path(QStringLiteral("data-tools/scripts/baseline/shop-exchange-data.json"));
        if (QFileInfo::exists(baselinePath)) {
          const auto baselineBytes = io.readPath(baselinePath, JsonLimit);
          const auto incomingDate = CatalogLegacyCompatibility::officialShopDate(object(bytes));
          const auto baselineDate = CatalogLegacyCompatibility::officialShopDate(object(baselineBytes));
          if (incomingDate.isValid() && baselineDate.isValid() && incomingDate < baselineDate) {
            origin.insert("rawObject", inputRef(io.put(bytes, QStringLiteral("json"), {{"kind", "retained-migration-input"}})));
            origin.insert("embeddedObject", inputRef(io.put(baselineBytes, QStringLiteral("json"), {{"kind", "retained-embedded-input"}})));
            origin.insert("kind", "legacy-superseded-by-embedded");
            origin.insert("compatibilityRule", "existing-official-shop-date-guard");
            bytes = baselineBytes;
          }
        }
      }
      if (key == QStringLiteral("battleReplay") && format == QStringLiteral("json") && origin.value("kind") != QStringLiteral("unavailable")) {
        object(bytes); origin.insert("originalFormat", "json");
        bytes = packedJson(bytes); format = QStringLiteral("brpack");
      }
      refs.insert(key, refJson(io.put(bytes, format, origin)));
    }
    for (const auto& file : attachmentNames()) {
      const auto path = io.path(QStringLiteral("catalog/") + file);
      if (!QFileInfo::exists(path)) continue;
      const auto bytes = io.readPath(path, JsonLimit); object(bytes);
      attachments.insert(file, refJson(io.put(bytes, QStringLiteral("json"),
          {{"kind", "legacy-recorded-source"}, {"relativePath", QStringLiteral("catalog/") + file}, {"originalSha256", sha(bytes)}})));
    }
    const QJsonObject manifest{{"schema", 1}, {"dependencySchema", 1}, {"components", refs}, {"attachments", attachments}};
    const auto bytes = json(manifest);
    const auto revision = sha(bytes);
    io.boundary(QStringLiteral("before-manifest"));
    io.immutable(QStringLiteral("public/releases/%1/manifest.json").arg(revision), bytes);
    const auto prepared = nativePrepared(io, revision, currentBytes, QStringLiteral("migration"), true);
    return PublicDatasetResult{Status::Ready, {}, prepared->snapshot, prepared, io.receipt(prepared->attempt)};
  });
}

PublicDatasetResult PublicDatasetStore::prepareRollback() const {
  return attempt([&] {
    StoreIo io(root_, options_);
    const auto bytes = io.pointerBytes(); const auto current = io.pointer(bytes);
    const auto previous = current.value("previousRevision").toString();
    require(!previous.isEmpty(), QStringLiteral("尚无可恢复的上一资料版本"), Status::NotFound);
    const auto prepared = nativePrepared(io, previous, bytes, QStringLiteral("rollback"), true);
    return PublicDatasetResult{Status::Ready, {}, prepared->snapshot, prepared, io.receipt(prepared->attempt)};
  });
}

PublicDatasetResult PublicDatasetStore::prepareRestoreRevision(const QString& revision) const {
  return attempt([&] {
    StoreIo io(root_, options_);
    // Explicit restore can establish an empty root. It still rejects a broken
    // existing pointer and accepts only an imported, content-addressed release
    // in this same root, through all the normal native readers and raw refs.
    const auto bytes = io.pointerBytes(); io.pointer(bytes);
    const auto prepared = nativePrepared(io, revision, bytes, QStringLiteral("restore"), true);
    return PublicDatasetResult{Status::Ready, {}, prepared->snapshot, prepared, io.receipt(prepared->attempt)};
  });
}

PublicDatasetResult PublicDatasetStore::commitPrepared(PublicDatasetPreparedHandle prepared) const {
  bool diskCommitted = false;
  QJsonObject finalReceipt;
  auto result = attempt([&] {
    require(prepared && prepared->snapshot && tokenValid(prepared->attempt), QStringLiteral("没有可提交的完整公共资料候选"));
    StoreIo io(root_, options_); io.cancel();
    io.directory(QStringLiteral("public"));
    QLockFile lock(io.path(QStringLiteral("public/publish.lock")));
    require(lock.tryLock(0), QStringLiteral("另一进程正在发布公共资料"), Status::Busy);
    const auto bytes = io.pointerBytes(); const auto current = io.pointer(bytes);
    if (current.value("attempt") == prepared->attempt && current.value("revision") == prepared->snapshot->revision)
      return PublicDatasetResult{Status::DiskCommitted, {}, io.prepare(prepared->snapshot->revision, false), prepared, io.recoveredReceipt(current)};
    require(current.value("revision").toString() == prepared->baseRevision &&
            (bytes.isEmpty() ? QString{} : sha(bytes)) == prepared->baseCurrentSha256,
            QStringLiteral("当前资料已变化，候选未覆盖新版本"), Status::Conflict);
    const auto revision = prepared->snapshot->revision;
    require(io.manifest(revision) == prepared->snapshot->manifest, QStringLiteral("候选清单在预读后被改变"));
    require(object(io.read(QStringLiteral("public/transactions/%1/candidate.json").arg(prepared->attempt), 2 * 1024 * 1024)) == prepared->candidate,
            QStringLiteral("候选事务在预读后被改变"));
    if (prepared->kind == QStringLiteral("update")) {
      const auto report = io.read(QStringLiteral("public/transactions/%1/validation.json").arg(prepared->attempt), JsonLimit);
      require(sha(report) == prepared->candidate.value("validation").toObject().value("reportSha256").toString(), QStringLiteral("候选检查报告在预读后被改变"));
      const auto evidence = prepared->validation.value("qualityReports").toObject();
      for (auto row = evidence.begin(); row != evidence.end(); ++row) {
        require(QStringList{QStringLiteral("pets"), QStringLiteral("skills"), QStringLiteral("battleReplay")}.contains(row.key()), QStringLiteral("未登记的质量报告"));
        const auto ref = row.value().toObject();
        require(ref.value("file") == QStringLiteral("quality/%1.json").arg(row.key()), QStringLiteral("质量报告路径无效"));
        const auto bytes = io.read(QStringLiteral("public/transactions/%1/quality/%2.json").arg(prepared->attempt, row.key()), JsonLimit);
        require(sha(bytes) == ref.value("sha256").toString() && ref.value("bytes") == QJsonValue(bytes.size()), QStringLiteral("质量报告在预读后被改变"));
      }
    }
    for (const auto& ref : prepared->snapshot->components) { io.checkedObject(ref); io.retainedInputs(ref); }
    for (const auto& ref : prepared->snapshot->attachments) io.checkedObject(ref);
    io.cancel(); io.boundary(QStringLiteral("before-commit"));
    const bool noChange = revision == prepared->baseRevision;
    const QString generation = noChange ? current.value("generation").toString() : token();
    finalReceipt = io.receipt(prepared->attempt);
    finalReceipt.insert("revision", revision); finalReceipt.insert("baseRevision", prepared->baseRevision);
    finalReceipt.insert("kind", prepared->kind); finalReceipt.insert("plannedGeneration", generation);
    finalReceipt.insert("checked", true); finalReceipt.insert("candidatePassed", true);
    finalReceipt.insert("diskCommitted", false); finalReceipt.insert("sessionApplied", false);
    finalReceipt.insert("state", "commitStarted"); finalReceipt.insert("attempt", prepared->attempt);
    io.receipt(prepared->attempt, finalReceipt);
    io.boundary(QStringLiteral("before-pointer"));
    // Cancellation before this point preserves the old pointer. Once the
    // atomic switch starts it completes, and any later failure is recoverable.
    io.cancel();
    require(io.pointerBytes(false) == bytes, QStringLiteral("提交前公共资料指针发生变化"), Status::Conflict);
    if (!noChange) io.write(QStringLiteral("public/current.json"), json({{"schema", 1}, {"revision", revision},
        {"previousRevision", prepared->baseRevision}, {"generation", generation}, {"attempt", prepared->attempt}}));
    diskCommitted = true;
    finalReceipt.insert("diskCommitted", true); finalReceipt.insert("state", "diskCommitted");
    io.boundary(QStringLiteral("after-pointer"));
    io.boundary(QStringLiteral("before-receipt"));
    io.receipt(prepared->attempt, finalReceipt);
    return PublicDatasetResult{Status::DiskCommitted, {}, prepared->snapshot, prepared, io.receipt(prepared->attempt)};
  });
  if (diskCommitted && !result.ok()) {
    result.status = Status::DiskCommitted;
    result.error = QStringLiteral("资料已保存，当前会话尚未应用；恢复记录待补：") + result.error;
    result.snapshot = prepared->snapshot; result.prepared = prepared; result.receipt = finalReceipt;
  } else if (!result.ok() && prepared && tokenValid(prepared->attempt)) {
    const auto recorded = attempt([&] {
      StoreIo io(root_, {}); auto receipt = io.receipt(prepared->attempt);
      if (receipt.value("diskCommitted") == QJsonValue(true)) {
        receipt.insert("lastRetryError", result.error); io.receipt(prepared->attempt, receipt);
        return PublicDatasetResult{Status::Ready, {}, {}, {}, io.receipt(prepared->attempt)};
      }
      receipt.insert("attempt", prepared->attempt); receipt.insert("diskCommitted", false); receipt.insert("sessionApplied", false);
      receipt.insert("state", statusName(result.status)); receipt.insert("error", result.error);
      io.receipt(prepared->attempt, receipt);
      return PublicDatasetResult{Status::Ready, {}, {}, {}, io.receipt(prepared->attempt)};
    });
    if (recorded.ok()) result.receipt = recorded.receipt;
  }
  return result;
}

PublicDatasetResult PublicDatasetStore::markSessionApplied(const QString& id, const QString& revision) const {
  return attempt([&] {
    StoreIo io(root_, options_);
    require(tokenValid(id) && digestValid(revision), QStringLiteral("公共资料应用身份无效"));
    io.directory(QStringLiteral("public"));
    QLockFile lock(io.path(QStringLiteral("public/publish.lock")));
    require(lock.tryLock(0), QStringLiteral("另一进程正在发布公共资料"), Status::Busy);
    const auto current = io.pointer(io.pointerBytes());
    auto receipt = io.receipt(id);
    require(current.value("revision") == revision && receipt.value("revision") == revision &&
            (current.value("attempt") == id || receipt.value("plannedGeneration") == current.value("generation")),
            QStringLiteral("资料已再次切换，不能把旧会话记为当前已应用"), Status::Conflict);
    receipt.insert("diskCommitted", true); receipt.insert("sessionApplied", true); receipt.insert("state", "sessionApplied");
    io.receipt(id, receipt);
    return PublicDatasetResult{Status::SessionApplied, {}, {}, {}, io.receipt(id)};
  });
}

QJsonObject PublicDatasetStore::inspect(QString* error) const {
  QJsonObject output;
  const auto result = attempt([&] {
    StoreIo io(root_, options_);
    const auto pointer = io.pointer(io.pointerBytes());
    require(!pointer.isEmpty(), QStringLiteral("尚无公共资料版本"), Status::NotFound);
    output.insert("current", pointer);
    output.insert("manifest", io.manifest(pointer.value("revision").toString()));
    output.insert("receipt", io.recoveredReceipt(pointer));
    output.insert("runtimeDeletionEnabled", false);
    output.insert("retention", QStringLiteral("Keep all referenced releases and objects until reliable cross-process leases exist"));
    // Listing only records a bounded possible collection set. No objects are
    // deleted while another process or retained page may still reference them.
    QSet<QString> referenced;
    const auto releases = QDir(io.path(QStringLiteral("public/releases"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    require(releases.size() <= 10000, QStringLiteral("公共资料版本目录超过检查预算"));
    for (const auto& revision : releases) {
      if (!digestValid(revision)) continue;
      const auto manifest = io.manifest(revision);
      for (const auto* section : {"components", "attachments"}) {
        const auto refs = manifest.value(QLatin1String(section)).toObject();
        for (const auto& value : refs) {
          const auto ref = value.toObject(); referenced.insert(ref.value("sha256").toString());
          const auto origin = ref.value("origin").toObject();
          for (const auto* field : {"rawObject", "embeddedObject"})
            if (origin.value(QLatin1String(field)).isObject()) referenced.insert(origin.value(QLatin1String(field)).toObject().value("sha256").toString());
        }
      }
    }
    QJsonArray reclaimable;
    const auto objects = QDir(io.path(QStringLiteral("public/objects"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    require(objects.size() <= 100000, QStringLiteral("公共资料对象目录超过检查预算"));
    for (const auto& digest : objects) if (digestValid(digest) && !referenced.contains(digest)) reclaimable.append(digest);
    output.insert("unreferencedObjectsNotDeleted", reclaimable);
    return PublicDatasetResult{Status::Ready, {}, {}, {}, {}};
  });
  if (!result.ok() && error) *error = result.error;
  output.insert("status", statusName(result.status));
  return output;
}
