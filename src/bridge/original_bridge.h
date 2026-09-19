#pragma once

#include "inline_hook.h"
#include "protocol/inbound_queue.h"
#include "contracts/operation_types.h"

#include <QObject>
#include <QList>
#include <QString>
#include <QVariant>

#include <atomic>
#include <functional>

class OriginalBridge final : public QObject {
  Q_OBJECT

public:
  explicit OriginalBridge(QObject* parent = nullptr);

  bool install();
  bool send(const QString& extension, const QString& command, const QString& json);
  SubmissionOutcome sendSubmission(const QString& extension, const QString& command,
                                   const QString& json);
  SubmissionOutcome invokeFlashSubmission(const QString& method, const QString& argument);
  void disableCapture();
  static bool closing() { return shutdownRequested_.load(std::memory_order_acquire); }
  bool captureHealthy() const { return inbound_.healthy(); }
  // Set before install. The handler may run on a host dispatch thread and must
  // only revoke thread-safe send permits; it must not call GUI/Repository code.
  void setOverflowHandler(std::function<void()> handler) { overflowHandler_ = std::move(handler); }
  QString lastError() const { return lastError_; }

signals:
  void packetCaptured(const InboundEnvelope& envelope);
  void captureUncertain(const QString& reason);

private:
  using DispatchFunction = void(__fastcall*)(quintptr, quintptr, quintptr,
                                              const QString&, const QList<QVariant>&);
  using ServiceGetter = void*(__fastcall*)();
  using CommandSender = void(__fastcall*)(void*, const QString*);

  static void __fastcall dispatchDetour(quintptr a1, quintptr a2, quintptr a3,
                                        const QString& method,
                                        const QList<QVariant>& arguments);

  static std::atomic<OriginalBridge*> instance_;
  static std::atomic_bool captureEnabled_;
  static std::atomic<DispatchFunction> originalDispatch_;
  static std::atomic_bool installationClaimed_;
  static std::atomic_bool shutdownRequested_;
  void capture(quintptr a1, quintptr a2, quintptr a3, const QString& method,
               const QList<QVariant>& arguments);
  void scheduleDrain();
  void drainInbound();

  InlineHook hook_;
  ServiceGetter serviceGetter_ = nullptr;
  CommandSender commandSender_ = nullptr;
  QString lastError_;
  InboundQueue inbound_;
  std::atomic_bool drainScheduled_{false};
  std::atomic_bool uncertaintyReported_{false};
  std::function<void()> overflowHandler_;
};
