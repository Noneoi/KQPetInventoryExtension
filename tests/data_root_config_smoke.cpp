#include "../src/loader/data_root_config.h"
#include "../src/bootstrap/strict_json.h"

#include <windows.h>
#include <cstdio>
#include <clocale>
#include <fstream>
#include <iterator>

namespace {
bool check(bool condition, const char* message) {
  if (!condition) std::fprintf(stderr, "FAIL: %s\n", message);
  return condition;
}
}

int main() {
  std::setlocale(LC_ALL, ".UTF8");
  wchar_t temporary[32768]{}, generated[32768]{};
  const DWORD count = GetTempPathW(32768, temporary);
  if (!count || count >= 32768 || !GetTempFileNameW(temporary, L"kqd", 0, generated)) return 1;
  const std::filesystem::path root(generated);
  if (!DeleteFileW(generated) || !std::filesystem::create_directory(root)) return 1;
  const auto client = root / L"客户端";
  std::filesystem::create_directory(client);
  const auto overrideRoot = root / L"环境缓存";
  const auto selectedRoot = root / L"长期缓存";
  const auto pendingRoot = root / L"新位置";
  const auto write = [&](const std::string& bytes) {
    std::ofstream file(client / L"KQPetDataRoot.json", std::ios::binary | std::ios::trunc);
    file << bytes;
    return bool(file);
  };
  const auto configuration = [&](const std::filesystem::path& data, const std::filesystem::path& pending = {}) {
    return std::string("{\"schema\":1,\"dataRoot\":") + kqpet::release::json::quote(data.u8string()) +
        (pending.empty() ? "" : ",\"pendingRoot\":" + kqpet::release::json::quote(pending.u8string())) + "}";
  };
  auto result = kqpet::launcher::resolveDataRoot(client);
  bool ok = check(result.dataRoot == client / L"KQPetData" && !result.configured && result.warning.empty(),
      "default data root was release-dependent or absent");
  result = kqpet::launcher::resolveDataRoot(client, overrideRoot.wstring());
  ok &= check(result.dataRoot == overrideRoot && !result.configured, "existing environment override was ignored");
  ok &= check(write(configuration(selectedRoot)), "configuration fixture write failed");
  result = kqpet::launcher::resolveDataRoot(client, overrideRoot.wstring());
  ok &= check(result.configured && result.dataRoot == selectedRoot && result.warning.empty(),
      "configured Unicode path did not take precedence over old environment");
  ok &= check(write("\xef\xbb\xbf" + configuration(selectedRoot, pendingRoot)), "pending fixture write failed");
  result = kqpet::launcher::resolveDataRoot(client, overrideRoot.wstring());
  ok &= check(result.dataRoot == selectedRoot && result.pendingRoot == pendingRoot,
      "an uncommitted migration changed the active data root");
  ok &= check(write(configuration(selectedRoot) + "broken"), "invalid fixture write failed");
  result = kqpet::launcher::resolveDataRoot(client, overrideRoot.wstring());
  ok &= check(!result.configured && result.dataRoot == overrideRoot && !result.warning.empty(),
      "corrupt configuration did not retain the prior root with an explicit warning");
  ok &= check(write("{\"schema\":1,\"dataRoot\":\"relative-cache\"}"), "relative fixture write failed");
  result = kqpet::launcher::resolveDataRoot(client);
  ok &= check(!result.configured && result.dataRoot == client / L"KQPetData" && !result.warning.empty(),
      "relative configuration was resolved against an unstable working directory");
  ok &= check(write(configuration(selectedRoot)), "environment fixture write failed");
  result = kqpet::launcher::resolveDataRoot(client);
  std::wstring error;
  ok &= check(kqpet::launcher::applyDataRootEnvironment(result, client, &error) &&
      kqpet::launcher::inheritedDataRoot() == selectedRoot.wstring(), "selected root was not passed to child environment");
  const auto sourceDetail = selectedRoot / L"accounts/test/details/7.json";
  std::filesystem::create_directories(sourceDetail.parent_path());
  { std::ofstream file(sourceDetail, std::ios::binary); file << "synthetic-detail-7"; }
  const auto read = [](const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
  };
  ok &= check(write(configuration(selectedRoot, pendingRoot)), "migration fixture write failed");
  result = kqpet::launcher::resolveDataRoot(client);
  const bool migrated = kqpet::launcher::prepareDataRootMigration(GetModuleHandleW(nullptr), client, &result, true, &error);
  if (!migrated) std::fwprintf(stderr, L"Migration diagnostic: %ls\n", error.c_str());
  ok &= check(migrated &&
      result.dataRoot == pendingRoot && result.pendingRoot.empty() &&
      read(pendingRoot / L"accounts/test/details/7.json") == "synthetic-detail-7" && read(sourceDetail) == "synthetic-detail-7",
      "embedded migration failed to copy/activate the new root or removed the old original");
  const auto occupied = root / L"非空目标";
  std::filesystem::create_directory(occupied);
  { std::ofstream file(occupied / L"existing.txt"); file << "keep-this"; }
  ok &= check(write(configuration(selectedRoot, occupied)), "failed migration fixture write failed");
  result = kqpet::launcher::resolveDataRoot(client);
  error.clear();
  ok &= check(!kqpet::launcher::prepareDataRootMigration(GetModuleHandleW(nullptr), client, &result, true, &error) &&
      result.dataRoot == selectedRoot && result.pendingRoot.empty() && !error.empty() &&
      read(occupied / L"existing.txt") == "keep-this", "failed migration changed roots or overwrote an occupied destination");
  const auto failedConfig = read(client / L"KQPetDataRoot.json");
  result = kqpet::launcher::resolveDataRoot(client);
  ok &= check(result.dataRoot == selectedRoot && result.pendingRoot.empty() &&
      failedConfig.find("migrationError") != std::string::npos &&
      kqpet::launcher::prepareDataRootMigration(GetModuleHandleW(nullptr), client, &result, true, &error) &&
      read(client / L"KQPetDataRoot.json") == failedConfig,
      "failed migration was retried at every launch or lost the settings error");
  const auto blockedTarget = root / L"运行中不迁移";
  ok &= check(write(configuration(selectedRoot, blockedTarget)), "host-running fixture write failed");
  result = kqpet::launcher::resolveDataRoot(client);
  ok &= check(!kqpet::launcher::prepareDataRootMigration(GetModuleHandleW(nullptr), client, &result, false, &error) &&
      result.dataRoot == selectedRoot && !std::filesystem::exists(blockedTarget),
      "migration copied a cache while its host was still running");
  // Cleanup is restricted to the unique directory allocated by GetTempFileName.
  const auto resolved = std::filesystem::weakly_canonical(root);
  if (resolved.parent_path() == std::filesystem::weakly_canonical(std::filesystem::path(temporary)) &&
      resolved.filename().wstring().rfind(L"kqd", 0) == 0) std::filesystem::remove_all(resolved);
  if (ok) std::puts("PASS: stable data root, embedded migration/rollback, one-shot failures and active-host exclusion");
  return ok ? 0 : 1;
}
