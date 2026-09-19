#include "runtime/startup_channel.h"
#include <cstdio>
#include <thread>
#include <atomic>

using namespace kqpet::startup;

int wmain(int argc, wchar_t** argv) {
  if (argc > 1) {
    if (std::wstring(argv[1]) == L"--delayed") Sleep(100);
    std::wstring error;
    auto channel = Channel::openForCurrentProcess(
        std::wstring(argv[1]) == L"--wrong-release" ? L"other-release" : L"fixture-release", L"fixture-profile", &error);
    if (std::wstring(argv[1]) == L"--wrong-release") return channel ? 1 : 42;
    if (!channel || channel->publish(State::Ready)) return 2;
    if (!channel->publish(State::ImageLoaded) || !channel->publish(State::Compatible) ||
        !channel->publish(State::BridgeReady) || !channel->publish(State::Ready)) return 3;
    if (channel->publish(State::Failed, 99)) return 4;
    return 0;
  }
  bool ok = true;
  const auto check = [&](bool value, const char* message) {
    if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); ok = false; }
  };
  for (int scenario : {0, 1, 2}) {
    const bool wrong = scenario == 1;
    std::wstring error;
    auto channel = Channel::create(L"fixture-release", L"fixture-profile", &error);
    if (!channel) return 5;
    auto environment = channel->childEnvironment();
    wchar_t executable[32768]{};
    if (!GetModuleFileNameW(nullptr, executable, 32768)) return 6;
    std::wstring command = L"\"" + std::wstring(executable) +
        (wrong ? L"\" --wrong-release" : scenario == 2 ? L"\" --delayed" : L"\" --child");
    STARTUPINFOW startup{sizeof(STARTUPINFOW)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE,
        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW,
        environment.data(), nullptr, &startup, &process)) return 7;
    check(channel->bindChild(process.hProcess, process.dwProcessId), "child identity could not bind");
    Identity expected{process.dwProcessId, processCreationTime(process.hProcess), L"fixture-release", L"fixture-profile"};
    check(channel->identityMatches(expected), "bound identity does not match");
    ++expected.creationTime;
    check(!channel->identityMatches(expected), "stale PID creation time accepted");
    ResumeThread(process.hThread);
    if (scenario == 2) channel.reset(); // Simulate loader timeout before child opens the channel.
    check(WaitForSingleObject(process.hProcess, 5000) == WAIT_OBJECT_0, "test child timed out");
    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    check(exitCode == (wrong ? 42u : 0u), "child accepted mixed release or failed valid handshake");
    if (channel)
      check(channel->state() == (wrong ? State::Prepared : State::Ready), "handshake state was forged/skipped");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
  }
  for (int iteration = 0; iteration < 100; ++iteration) {
    std::wstring error;
    auto channel = Channel::create(L"fixture-release", L"fixture-profile", &error);
    std::atomic<DWORD> winner{0};
    std::thread a([&] { if (channel->publish(State::Failed, 101)) winner.store(101); });
    std::thread b([&] { if (channel->publish(State::Failed, 202)) winner.store(202); });
    a.join(); b.join();
    check(channel->state() == State::Failed && channel->errorCode() == winner.load(),
          "failed competing publication changed winner's error code");
  }
  if (ok) std::puts("PASS: private child environment, nonce/PID/time binding, ordered READY and mixed-release refusal");
  return ok ? 0 : 1;
}
