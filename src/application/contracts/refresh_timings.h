#pragma once
// Application command/preferences value. GUI has no controller dependency.
struct RefreshTimings {
  int automaticIntervalMs = 60000; // Legacy preference only; automatic querying is disabled.
  int listRequestGapMs = 1000;
  int listTimeoutMs = 10000;
  int detailRequestGapMs = 1000;
  int detailBatchRestMs = 2000;
  int detailTimeoutMs = 8000;
  int detailBatchSize = 12;
  int detailMaxRetries = 1;
  int moveRequestTimeoutMs = 10000;
};
