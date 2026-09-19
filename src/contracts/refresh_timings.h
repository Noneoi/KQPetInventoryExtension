#pragma once
// Application command/preferences value. GUI has no controller dependency.
struct RefreshTimings {
  // Stored for settings compatibility only. Nothing schedules a read because
  // time passed: every query follows a manual refresh or a pet selection.
  int automaticIntervalMs = 60000;
  int listRequestGapMs = 1000;
  int listTimeoutMs = 10000;
  int detailRequestGapMs = 1000;
  int detailBatchRestMs = 2000;
  int detailTimeoutMs = 8000;
  int detailBatchSize = 12;
  int detailMaxRetries = 1;
  int moveRequestTimeoutMs = 10000;
};
