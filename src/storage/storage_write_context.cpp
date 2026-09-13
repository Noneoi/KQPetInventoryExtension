#include "storage_write_context.h"

#include <QUuid>
#include <utility>

StorageWriteContext::StorageWriteContext(QString account, QString root,
                                       QString directory, bool shared, bool readOnly)
    : account_(std::move(account)), dataRoot_(std::move(root)),
      directory_(std::move(directory)),
      id_(QUuid::createUuid().toString(QUuid::WithoutBraces)), shared_(shared), readOnly_(readOnly) {}
