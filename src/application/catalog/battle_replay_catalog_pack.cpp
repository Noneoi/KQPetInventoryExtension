#include "battle_replay_catalog_pack.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QtEndian>
#include <QtZlib/zlib.h>
#include <algorithm>
#include <array>
#include <utility>

namespace BattleReplayCatalogPack {
struct Decoder::Impl {
  explicit Impl(QByteArray value) : pack(std::move(value)) {
    if (pack.size() < HeaderBytes || pack.size() > MaximumPackBytes ||
        QByteArrayView(pack.constData(), 8) != QByteArrayView("KQBRPK1\n", 8)) {
      fail(QStringLiteral("回放压缩包标识、版本或文件大小无效")); return;
    }
    const quint64 declaredJson = qFromBigEndian<quint64>(pack.constData() + 8);
    const quint64 declaredBody = qFromBigEndian<quint64>(pack.constData() + 16);
    if (!declaredJson || declaredJson > MaximumJsonBytes || !declaredBody ||
        declaredBody != static_cast<quint64>(pack.size() - HeaderBytes)) {
      fail(QStringLiteral("回放压缩包长度无效或超过限制")); return;
    }
    expectedSize = static_cast<qsizetype>(declaredJson);
    expectedHash = pack.mid(24, 32);
    // This is a checked upper bound, unlike qUncompress's size hint. Reserve
    // once so a late reallocation cannot copy the whole catalog in one slice.
    json.reserve(expectedSize);
    if (inflateInit(&stream) != Z_OK) {
      fail(QStringLiteral("无法初始化回放资料解压")); return;
    }
    initialized = true;
  }
  ~Impl() { endStream(); }
  void endStream() {
    if (initialized) { inflateEnd(&stream); initialized = false; }
  }
  StepResult fail(QString value) {
    message = std::move(value);
    result = StepResult::Failed;
    endStream();
    pack.clear(); json.clear();
    return result;
  }
  StepResult step() {
    if (result != StepResult::More) return result;
    if (!stream.avail_in && inputOffset < pack.size()) {
      const auto chunk = std::min<qsizetype>(OutputChunkBytes, pack.size() - inputOffset);
      // inflate does not modify its input. Avoid detaching an implicitly
      // shared QByteArray merely because zlib's older API accepts Bytef*.
      stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(pack.constData() + inputOffset));
      stream.avail_in = static_cast<uInt>(chunk);
      inputOffset += chunk;
    }
    std::array<char, OutputChunkBytes> output;
    stream.next_out = reinterpret_cast<Bytef*>(output.data());
    stream.avail_out = static_cast<uInt>(output.size());
    const uInt beforeInput = stream.avail_in;
    const int status = inflate(&stream, Z_NO_FLUSH);
    const qsizetype produced = static_cast<qsizetype>(output.size() - stream.avail_out);
    if (produced > expectedSize - json.size())
      return fail(QStringLiteral("回放资料实际解压大小超过声明或限制"));
    if (produced) {
      json.append(output.data(), produced);
      hash.addData(QByteArrayView(output.data(), produced));
    }
    if (status == Z_STREAM_END) {
      if (stream.avail_in || inputOffset != pack.size() || json.size() != expectedSize)
        return fail(QStringLiteral("回放压缩包长度不匹配或包含额外内容"));
      digest = hash.result();
      if (digest != expectedHash)
        return fail(QStringLiteral("回放压缩包内容校验失败"));
      endStream();
      pack.clear();
      result = StepResult::Done;
      return result;
    }
    if (status != Z_OK && status != Z_BUF_ERROR)
      return fail(QStringLiteral("回放压缩数据损坏或使用了不支持的字典"));
    if (!produced && beforeInput == stream.avail_in)
      return fail(QStringLiteral("回放压缩数据截断或无法继续解压"));
    return StepResult::More;
  }
  QByteArray pack, json, expectedHash, digest;
  QCryptographicHash hash{QCryptographicHash::Sha256};
  z_stream stream{};
  qsizetype expectedSize = 0, inputOffset = HeaderBytes;
  bool initialized = false;
  StepResult result = StepResult::More;
  QString message;
};

Decoder::Decoder(QByteArray pack) : impl_(std::make_unique<Impl>(std::move(pack))) {}
Decoder::~Decoder() = default;
StepResult Decoder::step() { return impl_->step(); }
QString Decoder::error() const { return impl_->message; }
QByteArray Decoder::takeJson() {
  return impl_->result == StepResult::Done ? std::move(impl_->json) : QByteArray{};
}
QByteArray Decoder::sha256() const {
  return impl_->result == StepResult::Done ? impl_->digest : QByteArray{};
}

QString exportEmbeddedBaseline(const QString& output, const std::atomic_bool* cancelled) {
  const auto stopping = [cancelled] { return cancelled && cancelled->load(); };
  if (stopping()) return QStringLiteral("回放资料准备已停止");
  QFile input(QStringLiteral(":/kqpet/battle-replay-catalog.json"));
  if (!input.open(QIODevice::ReadOnly) || input.size() <= 0 || input.size() > MaximumJsonBytes)
    return QStringLiteral("内置回放资料不可读或超过大小限制");
  if (!QDir().mkpath(QFileInfo(output).absolutePath())) return QStringLiteral("无法准备回放资料目录");
  QSaveFile target(output);
  if (!target.open(QIODevice::WriteOnly) || target.write(QByteArray(HeaderBytes, '\0')) != HeaderBytes)
    return QStringLiteral("无法准备回放资料临时文件");
  struct Compressor {
    z_stream stream{};
    bool active = false;
    ~Compressor() { if (active) deflateEnd(&stream); }
  } compressor;
  if (deflateInit(&compressor.stream, 9) != Z_OK) return QStringLiteral("无法初始化回放资料压缩");
  compressor.active = true;
  auto& stream = compressor.stream;
  QCryptographicHash hash(QCryptographicHash::Sha256);
  qint64 rawBytes = 0, bodyBytes = 0;
  bool complete = false;
  std::array<char, OutputChunkBytes> buffer;
  while (!complete) {
    if (stopping()) return QStringLiteral("回放资料准备已停止");
    const QByteArray part = input.read(OutputChunkBytes);
    if (input.error() != QFileDevice::NoError || (part.isEmpty() && !input.atEnd()) ||
        part.size() > MaximumJsonBytes - rawBytes) return QStringLiteral("读取内置回放资料失败");
    rawBytes += part.size();
    hash.addData(part);
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(part.constData()));
    stream.avail_in = static_cast<uInt>(part.size());
    const int flush = input.atEnd() ? Z_FINISH : Z_NO_FLUSH;
    do {
      if (stopping()) return QStringLiteral("回放资料准备已停止");
      stream.next_out = reinterpret_cast<Bytef*>(buffer.data());
      stream.avail_out = static_cast<uInt>(buffer.size());
      const int status = deflate(&stream, flush);
      if (status != Z_OK && status != Z_STREAM_END) return QStringLiteral("压缩回放资料失败");
      const qint64 produced = static_cast<qint64>(buffer.size() - stream.avail_out);
      if (produced > MaximumPackBytes - HeaderBytes - bodyBytes ||
          target.write(buffer.data(), produced) != produced) return QStringLiteral("写入回放资料失败或文件过大");
      bodyBytes += produced;
      complete = status == Z_STREAM_END;
    } while (!complete && (stream.avail_in || !stream.avail_out || flush == Z_FINISH));
  }
  if (rawBytes != input.size() || !bodyBytes || stopping()) return QStringLiteral("回放资料准备未完成");
  QByteArray header(HeaderBytes, '\0');
  header.replace(qsizetype(0), qsizetype(8), "KQBRPK1\n", qsizetype(8));
  qToBigEndian<quint64>(static_cast<quint64>(rawBytes), header.data() + 8);
  qToBigEndian<quint64>(static_cast<quint64>(bodyBytes), header.data() + 16);
  header.replace(24, 32, hash.result());
  if (!target.seek(0) || target.write(header) != header.size() || stopping() || !target.commit())
    return QStringLiteral("无法保存回放资料，原文件已保留");
  return {};
}
} // namespace BattleReplayCatalogPack
