#pragma once
#include "account_resource_view.h"
#include <QDateTime>
#include <QJsonObject>

struct PublicPriceQuote {
  bool known = false;
  bool availabilityKnown = false;
  bool available = true;
  qint64 amount = 0;
  bool discounted = false;
  bool optionalPayment = false;
  QString reason;
};

// Pure evaluation: callers supply time and already reliable account facts.
PublicPriceQuote evaluatePublicPriceRule(const QJsonObject& rule, const QDateTime& at,
    const AccountResourceView* resources = nullptr, const QJsonObject& facts = {});
QDateTime nextPublicPriceChange(const QJsonObject& rule, const QDateTime& after);
