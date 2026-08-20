#include "pet_search.h"

#include <QCoreApplication>

#include <cstdio>

namespace {

bool require(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}

}  // namespace

int main(int argc, char* argv[]) {
  QCoreApplication application(argc, argv);
  const QString skin = QStringLiteral("[灵初]不移之月·影月");
  const QString original = QStringLiteral("神运月影王");
  bool ok = true;
  ok &= require(petPinyinInitials(skin).contains(QStringLiteral("byz")),
                "pinyin initials do not contain byz");
  ok &= require(petQueryMatches(QStringLiteral("yy"), {skin, original}),
                "yy did not match the skin name initials");
  ok &= require(petQueryMatches(QStringLiteral("BYZ"), {skin, original}),
                "uppercase initials were not case insensitive");
  ok &= require(petQueryMatches(QStringLiteral("不移之"), {skin, original}),
                "continuous Chinese query did not match");
  ok &= require(petQueryMatches(QStringLiteral("syy"), {skin, original}),
                "original name initials did not match");
  ok &= require(!petQueryMatches(QStringLiteral("yzb"), {skin, original}),
                "non-contiguous initials matched unexpectedly");
  const QPair<int, int> byz = petQueryHighlightRange(QStringLiteral("byz"), skin);
  ok &= require(byz.first >= 0 && skin.mid(byz.first, byz.second) == QStringLiteral("不移之"),
                "pinyin highlight did not select the corresponding Chinese characters");
  const QPair<int, int> chinese = petQueryHighlightRange(QStringLiteral("影月"), skin);
  ok &= require(chinese.first >= 0 && skin.mid(chinese.first, chinese.second) == QStringLiteral("影月"),
                "Chinese highlight range was incorrect");
  const QString diana = QStringLiteral("即刻永恒·黛安娜");
  ok &= require(petQueryMatches(QStringLiteral("dan"), {diana}),
                "dan did not match Diana's Chinese initials");
  const QPair<int, int> dan = petQueryHighlightRange(QStringLiteral("dan"), diana);
  ok &= require(dan.first >= 0 && diana.mid(dan.first, dan.second) == QStringLiteral("黛安娜"),
                "dan highlight did not select Diana's Chinese name");
  if (!ok) return 1;
  std::fprintf(stdout, "PASS: Chinese substring and pinyin-initial search\n");
  return 0;
}
