#pragma once

#include <QByteArray>
#include <QString>
#include <atomic>
#include <memory>

// Lossless container for the existing catalog JSON. This integrity check does
// not authenticate its author; the catalog validator still checks its schema
// and recorded official provenance after decoding.
namespace BattleReplayCatalogPack {
inline constexpr qint64 HeaderBytes = 56;
inline constexpr qint64 MaximumPackBytes = 32 * 1024 * 1024;
inline constexpr qint64 MaximumJsonBytes = 128 * 1024 * 1024;
inline constexpr qint64 OutputChunkBytes = 64 * 1024;

enum class StepResult { More, Done, Failed };

// Owned and stepped on Storage I/O. Each step consumes at most 64 KiB of
// compressed input and produces at most 64 KiB, allowing cancellation/yields.
class Decoder final {
public:
  explicit Decoder(QByteArray pack);
  ~Decoder();
  Decoder(const Decoder&) = delete;
  Decoder& operator=(const Decoder&) = delete;
  StepResult step();
  QString error() const;
  QByteArray takeJson();
  QByteArray sha256() const; // 32 raw bytes; available only after Done.
private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// Prepare the updater's baseline from the already embedded catalog, without
// another embedded copy or an expanded JSON file on disk. Called on Storage
// I/O. Atomic replacement preserves the previous baseline on any failure.
QString exportEmbeddedBaseline(const QString& output, const std::atomic_bool* cancelled = nullptr);
} // namespace BattleReplayCatalogPack
