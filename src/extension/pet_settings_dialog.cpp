#include "pet_settings_dialog.h"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace {

QSpinBox* makeSpinBox(int minimum, int maximum, const QString& suffix,
                      QWidget* parent) {
  auto* box = new QSpinBox(parent);
  box->setRange(minimum, maximum);
  box->setSuffix(suffix);
  box->setAccelerated(true);
  box->setMinimumWidth(180);
  return box;
}

}  // namespace

PetSettingsDialog::PetSettingsDialog(const PetRefreshController::Timings& timings,
                                     QWidget* parent)
    : QDialog(parent) {
  setWindowTitle(QStringLiteral("精灵仓库 · 刷新设置"));
  setModal(true);
  setMinimumWidth(520);

  auto* root = new QVBoxLayout(this);
  auto* explanation = new QLabel(
      QStringLiteral("参数会保存到 KQPetData\\settings.json，并在下次启动时继续使用。\n"
                     "批量仓库详情刷新期间，自动背包/仓库刷新始终暂停。"),
      this);
  explanation->setWordWrap(true);
  root->addWidget(explanation);

  auto* listGroup = new QGroupBox(QStringLiteral("背包 / 仓库列表刷新"), this);
  auto* listForm = new QFormLayout(listGroup);
  automaticIntervalSeconds_ = makeSpinBox(1, 3600, QStringLiteral(" 秒"), listGroup);
  listRequestGapMs_ = makeSpinBox(0, 10000, QStringLiteral(" 毫秒"), listGroup);
  listTimeoutSeconds_ = makeSpinBox(1, 120, QStringLiteral(" 秒"), listGroup);
  listForm->addRow(QStringLiteral("自动刷新间隔"), automaticIntervalSeconds_);
  listForm->addRow(QStringLiteral("背包到仓库请求间隔"), listRequestGapMs_);
  listForm->addRow(QStringLiteral("单个列表请求超时"), listTimeoutSeconds_);
  root->addWidget(listGroup);

  auto* detailGroup = new QGroupBox(QStringLiteral("仓库详情队列"), this);
  auto* detailForm = new QFormLayout(detailGroup);
  detailRequestGapMs_ = makeSpinBox(0, 10000, QStringLiteral(" 毫秒"), detailGroup);
  detailBatchSize_ = makeSpinBox(1, 100, QStringLiteral(" 只 / 批"), detailGroup);
  detailBatchRestSeconds_ = makeSpinBox(0, 120, QStringLiteral(" 秒"), detailGroup);
  detailTimeoutSeconds_ = makeSpinBox(1, 120, QStringLiteral(" 秒"), detailGroup);
  detailMaxRetries_ = makeSpinBox(0, 10, QStringLiteral(" 次"), detailGroup);
  detailForm->addRow(QStringLiteral("每只请求间隔"), detailRequestGapMs_);
  detailForm->addRow(QStringLiteral("每批数量"), detailBatchSize_);
  detailForm->addRow(QStringLiteral("每批完成后休息"), detailBatchRestSeconds_);
  detailForm->addRow(QStringLiteral("单只详情请求超时"), detailTimeoutSeconds_);
  detailForm->addRow(QStringLiteral("失败重试次数"), detailMaxRetries_);
  root->addWidget(detailGroup);

  auto* moveGroup = new QGroupBox(QStringLiteral("精灵移动"), this);
  auto* moveForm = new QFormLayout(moveGroup);
  moveRequestTimeoutSeconds_ =
      makeSpinBox(1, 120, QStringLiteral(" 秒"), moveGroup);
  moveForm->addRow(QStringLiteral("移动写请求响应超时"),
                   moveRequestTimeoutSeconds_);
  root->addWidget(moveGroup);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save |
                                            QDialogButtonBox::Cancel |
                                            QDialogButtonBox::RestoreDefaults,
                                        this);
  buttons->button(QDialogButtonBox::Save)->setText(QStringLiteral("保存"));
  buttons->button(QDialogButtonBox::Cancel)->setText(QStringLiteral("取消"));
  buttons->button(QDialogButtonBox::RestoreDefaults)->setText(QStringLiteral("恢复默认"));
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  connect(buttons->button(QDialogButtonBox::RestoreDefaults), &QPushButton::clicked,
          this, [this]() { applyTimings(PetRefreshController::Timings{}); });
  root->addWidget(buttons);

  applyTimings(timings);
}

void PetSettingsDialog::applyTimings(const PetRefreshController::Timings& timings) {
  automaticIntervalSeconds_->setValue(qMax(1, (timings.automaticIntervalMs + 999) / 1000));
  listRequestGapMs_->setValue(timings.listRequestGapMs);
  listTimeoutSeconds_->setValue(qMax(1, (timings.listTimeoutMs + 999) / 1000));
  detailRequestGapMs_->setValue(timings.detailRequestGapMs);
  detailBatchSize_->setValue(timings.detailBatchSize);
  detailBatchRestSeconds_->setValue((timings.detailBatchRestMs + 999) / 1000);
  detailTimeoutSeconds_->setValue(qMax(1, (timings.detailTimeoutMs + 999) / 1000));
  detailMaxRetries_->setValue(timings.detailMaxRetries);
  moveRequestTimeoutSeconds_->setValue(
      qMax(1, (timings.moveRequestTimeoutMs + 999) / 1000));
}

PetRefreshController::Timings PetSettingsDialog::timings() const {
  PetRefreshController::Timings result;
  result.automaticIntervalMs = automaticIntervalSeconds_->value() * 1000;
  result.listRequestGapMs = listRequestGapMs_->value();
  result.listTimeoutMs = listTimeoutSeconds_->value() * 1000;
  result.detailRequestGapMs = detailRequestGapMs_->value();
  result.detailBatchSize = detailBatchSize_->value();
  result.detailBatchRestMs = detailBatchRestSeconds_->value() * 1000;
  result.detailTimeoutMs = detailTimeoutSeconds_->value() * 1000;
  result.detailMaxRetries = detailMaxRetries_->value();
  result.moveRequestTimeoutMs = moveRequestTimeoutSeconds_->value() * 1000;
  return result;
}
