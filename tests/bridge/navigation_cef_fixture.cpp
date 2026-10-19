#include <QVariant>
#include <QWidget>

// Test-only ABI fixture. Loaded as QCefView.dll in an isolated test process;
// never shipped with the extension or loaded into the user's client.
class __declspec(dllexport) QCefView final : public QWidget {
  Q_OBJECT
public:
  bool executeJavascriptWithResult(const qint64& frame, const QString& script,
                                   const QString& url, const QString& context);
signals:
  void reportJavascriptResult(int browserId, qint64 frameId, const QString& context, const QVariant& result);
};

bool QCefView::executeJavascriptWithResult(const qint64& frame, const QString& script,
                                          const QString& url, const QString& context) {
  setProperty("calls", property("calls").toInt() + 1);
  setProperty("frame", frame);
  setProperty("script", script);
  setProperty("url", url);
  setProperty("context", context);
  return !property("rejectExecution").toBool();
}

extern "C" __declspec(dllexport) QWidget* createNavigationFixture() { return new QCefView; }

#include "navigation_cef_fixture.moc"
