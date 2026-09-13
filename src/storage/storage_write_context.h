#pragma once

#include <QString>
#include <memory>

class StorageService;

// Immutable capability for one fixed account directory (or shared-data scope).
// Creating a context performs no disk I/O. Its account lock is acquired by the
// I/O worker before the first write and retained while this context or any
// accepted task referring to it remains alive.
class StorageWriteContext final {
public:
  QString account() const { return account_; }
  QString dataRoot() const { return dataRoot_; }
  QString directory() const { return directory_; }
  QString id() const { return id_; }
  bool isShared() const { return shared_; }
  bool isReadOnly() const { return readOnly_; }

private:
  friend class StorageService;
  StorageWriteContext(QString account, QString root, QString directory, bool shared,
                      bool readOnly = false);
  const QString account_;
  const QString dataRoot_;
  const QString directory_;
  const QString id_;
  const bool shared_;
  const bool readOnly_;
};

using StorageContext = std::shared_ptr<const StorageWriteContext>;
