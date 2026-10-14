#include "public_data_detail_dialog.h"
#include "domain/public_data_detail_query.h"
#include "html_document.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTextBrowser>
#include <QPlainTextEdit>
#include <QLabel>
#include <QToolButton>
#include <QScrollBar>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonArray>
#include <QSignalBlocker>
#include <QSet>
#include <QTimer>

namespace {
QString plain(const QString& value) {
  if (!value.contains('<') && !value.contains('&')) return value;
  QTextDocument text; text.setHtml(value); return text.toPlainText();
}
QString safe(const QString& value) { return plain(value).toHtmlEscaped().replace('\n',QStringLiteral("<br>")); }
QString rawText(const QJsonValue& value) {
  if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson(QJsonDocument::Indented));
  if (value.isArray()) return QString::fromUtf8(QJsonDocument(value.toArray()).toJson(QJsonDocument::Indented));
  if (value.isUndefined()) return QStringLiteral("原记录未收录");
  return QString::fromUtf8(QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Indented));
}
void sourceUrls(const QJsonValue& value, QSet<QString>& seen, QStringList& links, int depth=0) {
  if (depth>5 || links.size()>=12) return;
  if (value.isString()) {
    const QUrl url(value.toString());
    if ((url.scheme()==QStringLiteral("https") || url.scheme()==QStringLiteral("http")) && !url.host().isEmpty() && !seen.contains(url.toString())) {
      seen.insert(url.toString());
      const auto label=url.fileName().isEmpty() ? url.host() : url.fileName();
      links.append(QStringLiteral("<p><a href=\"%1\">%2</a></p>").arg(url.toString(QUrl::FullyEncoded).toHtmlEscaped(),label.toHtmlEscaped()));
    }
  } else if (value.isObject()) {
    const auto row=value.toObject(); for (auto it=row.begin();it!=row.end();++it) sourceUrls(it.value(),seen,links,depth+1);
  } else if (value.isArray()) for (const auto& item : value.toArray()) sourceUrls(item,seen,links,depth+1);
}
}

QUrl PublicDataDetailLinks::url(const PublicRecordKey& key) {
  QUrl url(QStringLiteral("kqpublic://record")); QUrlQuery query;
  query.addQueryItem(QStringLiteral("view"),QString::number(int(key.view)));
  query.addQueryItem(QStringLiteral("table"),key.table); query.addQueryItem(QStringLiteral("id"),key.id); url.setQuery(query);
  return url;
}
QString PublicDataDetailLinks::anchor(const PublicRecordKey& key, const QString& label) {
  return QStringLiteral("<a href=\"%1\">%2</a>").arg(url(key).toString(QUrl::FullyEncoded).toHtmlEscaped(),label.toHtmlEscaped());
}
bool PublicDataDetailLinks::decode(const QUrl& url, PublicRecordKey* key) {
  if (!key || url.scheme()!=QStringLiteral("kqpublic") || url.host()!=QStringLiteral("record") ||
      !url.path().isEmpty() || !url.fragment().isEmpty() || !url.userInfo().isEmpty()) return false;
  const QUrlQuery query(url);
  if (query.queryItems().size()!=3) return false;
  bool ok=false; const auto kind=query.queryItemValue(QStringLiteral("view"),QUrl::FullyDecoded); const int view=kind.toInt(&ok);
  const auto table=query.queryItemValue(QStringLiteral("table"),QUrl::FullyDecoded), id=query.queryItemValue(QStringLiteral("id"),QUrl::FullyDecoded);
  if (!ok || QString::number(view)!=kind || view<0 || view>int(PublicDataViewKind::Icons) || table.isEmpty() || id.isEmpty()) return false;
  *key={PublicDataViewKind(view),table,id}; return true;
}

PublicDataDetailDialog::PublicDataDetailDialog(QWidget* parent) : QDialog(parent,Qt::Window) {
  setObjectName(QStringLiteral("KQPublicDataDetail")); setWindowTitle(QStringLiteral("官方资料"));
  setAttribute(Qt::WA_DeleteOnClose); setModal(false); resize(650,720); setMinimumSize(430,400);
  auto* layout=new QVBoxLayout(this); layout->setContentsMargins(14,12,14,12); layout->setSpacing(8);
  auto* navigation=new QHBoxLayout;
  back_=new QToolButton(this); back_->setText(QStringLiteral("← 返回")); back_->setObjectName("publicDetailBack");
  forward_=new QToolButton(this); forward_->setText(QStringLiteral("前进 →")); forward_->setObjectName("publicDetailForward");
  contextLabel_=new QLabel(this); contextLabel_->setWordWrap(true); contextLabel_->setStyleSheet(QStringLiteral("color:#64748b"));
  navigation->addWidget(back_); navigation->addWidget(forward_); navigation->addWidget(contextLabel_,1); layout->addLayout(navigation);
  browser_=new QTextBrowser(this); browser_->setObjectName("publicDetailContent"); browser_->setOpenLinks(false); browser_->setOpenExternalLinks(false);
  browser_->setStyleSheet(QStringLiteral("QTextBrowser{border:1px solid #dce3eb;border-radius:8px;background:white;}")); layout->addWidget(browser_,1);
  sourceButton_=new QToolButton(this); sourceButton_->setText(QStringLiteral("资料来源")); sourceButton_->setCheckable(true); sourceButton_->setObjectName("publicDetailSources");
  sourceButton_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); sourceButton_->setArrowType(Qt::RightArrow); layout->addWidget(sourceButton_);
  sourcePanel_=new QWidget(this); auto* sources=new QVBoxLayout(sourcePanel_); sources->setContentsMargins(8,0,0,0);
  sourceText_=new QTextBrowser(sourcePanel_); sourceText_->setObjectName("publicDetailSourceLinks");
  sourceText_->setOpenExternalLinks(true); sourceText_->setMaximumHeight(110); sources->addWidget(sourceText_);
  rawButton_=new QToolButton(sourcePanel_); rawButton_->setText(QStringLiteral("技术编号与完整原记录")); rawButton_->setCheckable(true); rawButton_->setObjectName("publicDetailRawToggle"); sources->addWidget(rawButton_);
  rawText_=new QPlainTextEdit(sourcePanel_); rawText_->setReadOnly(true); rawText_->setObjectName("publicDetailRaw"); rawText_->setMinimumHeight(130); rawText_->setMaximumHeight(230); sources->addWidget(rawText_);
  layout->addWidget(sourcePanel_); sourcePanel_->hide(); rawText_->hide();
  connect(back_,&QToolButton::clicked,this,&PublicDataDetailDialog::goBack);
  connect(forward_,&QToolButton::clicked,this,&PublicDataDetailDialog::goForward);
  connect(sourceButton_,&QToolButton::toggled,this,[this](bool show) { sourcePanel_->setVisible(show); sourceButton_->setArrowType(show ? Qt::DownArrow : Qt::RightArrow); });
  connect(rawButton_,&QToolButton::toggled,this,&PublicDataDetailDialog::showRaw);
  connect(browser_,&QTextBrowser::anchorClicked,this,[this](const QUrl& url) {
    PublicRecordKey key; if (index_>=0 && PublicDataDetailLinks::decode(url,&key)) navigate(history_[index_].context,key);
  });
}
PublicDataDetailDialog* PublicDataDetailDialog::open(const PublicDataDetailContext& context, const PublicRecordKey& key, QWidget* owner) {
  auto* dialog=owner ? owner->findChild<PublicDataDetailDialog*>(QStringLiteral("KQPublicDataDetail"),Qt::FindDirectChildrenOnly) : nullptr;
  if (!dialog) dialog=new PublicDataDetailDialog(owner);
  dialog->navigate(context,key); dialog->show(); dialog->raise(); dialog->activateWindow(); return dialog;
}
void PublicDataDetailDialog::savePosition() {
  if (index_<0) return;
  auto& page=history_[index_]; page.scroll=browser_->verticalScrollBar()->value(); page.sources=sourceButton_->isChecked(); page.raw=rawButton_->isChecked();
}
void PublicDataDetailDialog::navigate(const PublicDataDetailContext& context, const PublicRecordKey& key) {
  // Copy first: callers may pass a context stored in history_ that reallocation
  // would invalidate. Every history page retains its own publication handle.
  Page next; next.context=context; next.detail=PublicDataDetailQuery::resolve(next.context,key);
  savePosition(); history_.resize(index_+1); history_.append(std::move(next)); ++index_; render();
}
void PublicDataDetailDialog::goBack() { if (index_>0) { savePosition(); --index_; render(); } }
void PublicDataDetailDialog::goForward() { if (index_+1<history_.size()) { savePosition(); ++index_; render(); } }
const PublicDataDetail& PublicDataDetailDialog::currentDetail() const {
  static const PublicDataDetail empty; return index_>=0 ? history_[index_].detail : empty;
}
void PublicDataDetailDialog::showRaw(bool visible) {
  if (visible && index_>=0) {
    const auto& d=history_[index_].detail;
    QJsonObject evidence{{"view",int(d.key.view)},{"table",d.key.table},{"id",d.key.id},{"revision",d.revision},
      {"source",d.source},{"raw",d.raw}};
    QJsonArray relations;
    for (const auto& link : d.relations) relations.append(QJsonObject{{"view",int(link.key.view)},{"table",link.key.table},
      {"id",link.key.id},{"evidence",link.evidence}});
    evidence.insert("explicitRelations",relations); rawText_->setPlainText(rawText(evidence));
  } else rawText_->clear();
  rawText_->setVisible(visible);
}
void PublicDataDetailDialog::render() {
  const auto& page=history_[index_]; const auto& d=page.detail;
  setWindowTitle(d.title+QStringLiteral(" · 官方资料"));
  contextLabel_->setText(d.replayReference ? QStringLiteral("使用本场固定资料 · 说明不代表本场已触发") : QStringLiteral("使用打开入口时的资料版本"));
  QString body=QStringLiteral("<p class='muted'>%1</p><h2>%2</h2>").arg(d.kind.toHtmlEscaped(),d.title.toHtmlEscaped());
  if (!d.formName.isEmpty()) body+=QStringLiteral("<p class='muted'>记录名称 / 形态：%1</p>").arg(d.formName.toHtmlEscaped());
  if (d.state==PublicValueState::Missing || d.state==PublicValueState::WrongView)
    body+=QStringLiteral("<p>当前资料版本未收录这条记录。</p>");
  else if (d.state==PublicValueState::InvalidContainer) body+=QStringLiteral("<p>记录结构无法唯一确定；原记录保留在资料来源中。</p>");
  else body+=d.description.isEmpty() ? QStringLiteral("<p class='muted'>这条官方记录未提供独立说明。</p>") : QStringLiteral("<p>%1</p>").arg(safe(d.description));
  for (const auto& field : d.fields) body+=QStringLiteral("<p><b>%1</b>　%2</p>").arg(safe(field.label),safe(field.text));
  if (!d.conditions.isEmpty()) {
    body+=QStringLiteral("<h3>适用条件</h3>"); for (const auto& value : d.conditions) body+=QStringLiteral("<p>%1</p>").arg(safe(value));
  }
  if (!d.relations.isEmpty()) {
    body+=QStringLiteral("<h3>相关资料</h3><p class='muted'>仅连接已明确记录的配置关系。</p>");
    for (const auto& link : d.relations) body+=QStringLiteral("<p><b>%1</b>　%2</p>").arg(safe(link.label),
      PublicDataDetailLinks::anchor(link.key,PublicDataDetailQuery::title(page.context,link.key)));
  }
  browser_->setHtml(kqpet::html::document(QStringLiteral("h3{font-size:14px;color:#24547f;margin-top:18px}p{line-height:155%}"),body));
  QStringList links; QSet<QString> seen; sourceUrls(d.source,seen,links);
  QString sourceSummary=QStringLiteral("<p>说明来自当前固定版本中保留的官方资料；未收录的说明保持为空。</p>")+links.join(QString());
  if (links.isEmpty()) sourceSummary+=QStringLiteral("<p>原记录未单独保存可打开的来源链接；文件与解析来源见下方原记录。</p>");
  sourceText_->setHtml(kqpet::html::document({},sourceSummary));
  { QSignalBlocker a(sourceButton_),b(rawButton_); sourceButton_->setChecked(page.sources); rawButton_->setChecked(page.raw); }
  sourcePanel_->setVisible(page.sources); sourceButton_->setArrowType(page.sources ? Qt::DownArrow : Qt::RightArrow); showRaw(page.raw);
  back_->setEnabled(index_>0); forward_->setEnabled(index_+1<history_.size());
  const int wanted=page.scroll, historyIndex=index_;
  browser_->verticalScrollBar()->setValue(wanted);
  QTimer::singleShot(0,this,[this,wanted,historyIndex] { if (index_==historyIndex) browser_->verticalScrollBar()->setValue(wanted); });
}
