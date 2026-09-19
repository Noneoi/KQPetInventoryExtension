#include "original_window_locator.h"

#include <QApplication>
#include <QEvent>
#include <QTabBar>

OriginalWindowLocator::OriginalWindowLocator(QObject* parent) : QObject(parent) {
  retryTimer_.setInterval(1000);
  connect(&retryTimer_, &QTimer::timeout, this, &OriginalWindowLocator::refresh);
  connect(qApp, &QCoreApplication::aboutToQuit, this, [this] {
    stopping_ = true;
    retryTimer_.stop();
  });
}

QWidget* OriginalWindowLocator::selectMainWindow(const QWidgetList& windows) {
  QWidget* best = nullptr;
  int bestIdentity = 0;
  qint64 bestArea = 0;
  for (QWidget* candidate : windows) {
    if (!candidate || !candidate->isWindow() || !candidate->isVisible() ||
        candidate->windowType() != Qt::Window ||
        candidate->width() < 320 || candidate->height() < 240) continue;
    const QString objectName = candidate->objectName();
    if (objectName.startsWith(QStringLiteral("KQPet")) ||
        objectName.startsWith(QStringLiteral("KQAsset")) ||
        objectName.startsWith(QStringLiteral("KQRoutine"))) continue;
    const QString className = QString::fromLatin1(candidate->metaObject()->className());
    if (className == QStringLiteral("StartupDialog")) continue;
    const QString title = candidate->windowTitle();
    const bool namedClient = objectName.startsWith(QStringLiteral("KQPro"), Qt::CaseInsensitive) ||
                             className.startsWith(QStringLiteral("KQPro"), Qt::CaseInsensitive);
    const bool titledClient = title.contains(QStringLiteral("氪奇")) &&
                               title.contains(QStringLiteral("Pro"), Qt::CaseInsensitive);
    const int identity = namedClient ? 2 : titledClient ? 1 : 0;
    if (!identity) continue;
    const qint64 area = static_cast<qint64>(candidate->width()) * candidate->height();
    if (identity > bestIdentity || (identity == bestIdentity && area > bestArea)) {
      best = candidate;
      bestIdentity = identity;
      bestArea = area;
    }
  }
  return best;
}

QRect OriginalWindowLocator::singleEntryGeometry(QWidget* window) {
  if (!window || window->width() < 104 || window->height() < 60) return {};
  constexpr int width = 88;
  constexpr int height = 28;
  for (QTabBar* bar : window->findChildren<QTabBar*>()) {
    if (!bar->isVisibleTo(window) || bar->count() < 3) continue;
    for (int index = 0; index < bar->count(); ++index) {
      if (bar->tabText(index).remove(QLatin1Char('&')).trimmed() != QStringLiteral("资源")) continue;
      const QPoint position = bar->mapTo(window, QPoint());
      if (position.y() < height) continue;
      const int center = position.x() + bar->tabRect(index).center().x();
      return QRect(qBound(8, center - width / 2, window->width() - width - 8),
                   qMin(qMax(0, position.y() - height - 2), window->height() - height), width, height);
    }
  }
  // No guessed screen-percentage placement over an unrelated host control.
  return {};
}
void OriginalWindowLocator::start() {
  if (stopping_) return;
  retryTimer_.start();
  refresh();
}

void OriginalWindowLocator::refresh() {
  if (stopping_) return;
  QWidget* selected = selectMainWindow(QApplication::topLevelWidgets());
  if (!selected) return;  // A hidden/minimized host must not be shown by the extension.
  if (selected != window_) {
    if (window_) window_->removeEventFilter(this);
    disconnect(destroyedConnection_);
    window_ = selected;
    selected->installEventFilter(this);
    destroyedConnection_ = connect(selected, &QObject::destroyed, this, [this] {
      window_.clear();
      emit windowChanged(nullptr);
      QTimer::singleShot(0, this, &OriginalWindowLocator::refresh);
    });
    emit windowChanged(selected);
  }
  queueLayout();
}

void OriginalWindowLocator::queueLayout() {
  if (layoutQueued_ || stopping_) return;
  layoutQueued_ = true;
  QTimer::singleShot(0, this, [this] {
    layoutQueued_ = false;
    if (!stopping_ && window_) emit layoutNeeded();
  });
}

bool OriginalWindowLocator::eventFilter(QObject* watched, QEvent* event) {
  if (watched == window_ && (event->type() == QEvent::Resize ||
      event->type() == QEvent::Show || event->type() == QEvent::ChildAdded ||
      event->type() == QEvent::LayoutRequest || event->type() == QEvent::WindowStateChange)) {
    queueLayout();
  }
  // Raising the buttons emits ZOrderChange; deliberately ignore it.
  return QObject::eventFilter(watched, event);
}
