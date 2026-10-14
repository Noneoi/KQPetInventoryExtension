#pragma once
#include "contracts/public_data_detail.h"
#include <QDialog>
#include <QUrl>

class QTextBrowser;
class QPlainTextEdit;
class QToolButton;
class QLabel;

namespace PublicDataDetailLinks {
QUrl url(const PublicRecordKey& key);
QString anchor(const PublicRecordKey& key, const QString& label);
bool decode(const QUrl& url, PublicRecordKey* key);
}

class PublicDataDetailDialog final : public QDialog {
  Q_OBJECT
public:
  explicit PublicDataDetailDialog(QWidget* parent = nullptr);
  static PublicDataDetailDialog* open(const PublicDataDetailContext& context,
                                     const PublicRecordKey& key, QWidget* owner);
  void navigate(const PublicDataDetailContext& context, const PublicRecordKey& key);
  void goBack();
  void goForward();
  const PublicDataDetail& currentDetail() const;

private:
  struct Page {
    PublicDataDetailContext context;
    PublicDataDetail detail;
    int scroll = 0;
    bool sources = false, raw = false;
  };
  void savePosition();
  void render();
  void showRaw(bool visible);
  QVector<Page> history_;
  int index_ = -1;
  QTextBrowser* browser_ = nullptr;
  QTextBrowser* sourceText_ = nullptr;
  QPlainTextEdit* rawText_ = nullptr;
  QLabel* contextLabel_ = nullptr;
  QToolButton* back_ = nullptr;
  QToolButton* forward_ = nullptr;
  QToolButton* sourceButton_ = nullptr;
  QToolButton* rawButton_ = nullptr;
  QWidget* sourcePanel_ = nullptr;
};
