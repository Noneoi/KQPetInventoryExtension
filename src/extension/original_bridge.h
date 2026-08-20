#pragma once

#include "inline_hook.h"

#include <QObject>
#include <QList>
#include <QString>
#include <QVariant>

class OriginalBridge final : public QObject {
  Q_OBJECT

public:
  explicit OriginalBridge(QObject* parent = nullptr);

  bool install();
  bool send(const QString& extension, const QString& command, const QString& json);
  QString lastError() const { return lastError_; }

signals:
  void packetReceived(const QString& method, const QString& payload);

private:
  using DispatchFunction = void(__fastcall*)(quintptr, quintptr, quintptr,
                                              const QString&, const QList<QVariant>&);
  using ServiceGetter = void*(__fastcall*)();
  using CommandSender = void(__fastcall*)(void*, const QString*);

  static void __fastcall dispatchDetour(quintptr a1, quintptr a2, quintptr a3,
                                        const QString& method,
                                        const QList<QVariant>& arguments);

  static OriginalBridge* instance_;
  static DispatchFunction originalDispatch_;

  InlineHook hook_;
  ServiceGetter serviceGetter_ = nullptr;
  CommandSender commandSender_ = nullptr;
  QString lastError_;
};
