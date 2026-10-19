#include "game_navigation_script.h"
#include "contracts/game_navigation.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace GameNavigationScript {

bool isAllowedLink(const QString& link) {
  return GameNavigation::isAllowedLink(link);
}

QString build(const QString& link) {
  if (!isAllowedLink(link)) return {};
  const QString route = QString::fromUtf8(
      QJsonDocument(QJsonArray{link}).toJson(QJsonDocument::Compact));
  // KQPro 1.1.4's embedded SocketClient registers gotogetpet. Mode -1 passes
  // the complete button key to the official ClickEffectHelper.doAuto; mode 0
  // would instead jump scenes. Flash has no JavaScript AQLib object.
  // A void return from either helper acknowledges dispatch, not panel display.
  return QStringLiteral(
      "return (function(){var v=%1;try{var f=document.myFlash;"
      "if(f){if(typeof f.gotogetpet!=='function')return 'flash-not-ready';"
      "f.gotogetpet(v[0],-1);return 'submitted-flash';}"
      "var a=window.AQLib;if(!a)return 'missing-game-runtime';"
      "if(!a.ClickEffectHelper||typeof a.ClickEffectHelper.doAuto!=='function')"
      "return 'h5-not-ready';"
      "a.ClickEffectHelper.doAuto(v[0]);return 'submitted-h5';"
      "}catch(e){return 'error:'+(e&&e.message?e.message:String(e));}})();").arg(route);
}

Result classify(const QString& value) {
  if (value == QStringLiteral("submitted-flash") || value == QStringLiteral("submitted-h5"))
    return Result::Submitted;
  if (value == QStringLiteral("missing-game-runtime") ||
      value == QStringLiteral("flash-not-ready") || value == QStringLiteral("h5-not-ready"))
    return Result::NotReady;
  if (value.startsWith(QStringLiteral("error:"))) return Result::Failed;
  return Result::Unconfirmed;
}

QString message(const QString& value) {
  if (classify(value) == Result::Submitted)
    return QStringLiteral("已调用对应游戏入口，请在游戏窗口查看");
  if (value == QStringLiteral("flash-not-ready"))
    return QStringLiteral("Flash 游戏入口尚未就绪，请等待游戏加载完成");
  if (value == QStringLiteral("h5-not-ready"))
    return QStringLiteral("H5 游戏活动入口尚未就绪，请等待游戏加载完成");
  if (value == QStringLiteral("missing-game-runtime"))
    return QStringLiteral("当前页面未找到游戏入口，请先进入游戏");
  if (classify(value) == Result::Failed)
    return QStringLiteral("游戏入口执行异常：%1；请先查看游戏窗口").arg(value.mid(6).left(200));
  return QStringLiteral("未收到有效的跳转结果，请先查看游戏窗口；本次不会重复调用");
}

}  // namespace GameNavigationScript
