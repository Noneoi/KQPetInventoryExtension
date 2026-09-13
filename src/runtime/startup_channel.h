#pragma once
#include <windows.h>
#include <memory>
#include <string>
#include <vector>

namespace kqpet::startup {
inline constexpr unsigned kProtocol = 1;
inline constexpr wchar_t kEnvironmentKey[] = L"KQPET_STARTUP_CHANNEL_V1";
enum class State : LONG { Prepared, ImageLoaded, Compatible, BridgeReady, Ready, Failed };
struct Identity {
  DWORD pid = 0;
  ULONGLONG creationTime = 0;
  std::wstring releaseId;
  std::wstring profileId;
};

// Both sides own their handles independently. A loader timeout cannot destroy
// the DLL's late-reporting channel. The nonce is private to one child launch.
class Channel final {
public:
  ~Channel();
  Channel(const Channel&) = delete;
  Channel& operator=(const Channel&) = delete;
  static std::shared_ptr<Channel> create(const std::wstring& releaseId,
      const std::wstring& profileId, std::wstring* error);
  static std::shared_ptr<Channel> openForCurrentProcess(const std::wstring& releaseId,
      const std::wstring& profileId, std::wstring* error);
  bool bindChild(HANDLE process, DWORD pid);
  bool publish(State state, DWORD errorCode = 0);
  bool identityMatches(const Identity& identity) const;
  State state() const;
  DWORD errorCode() const;
  bool wait(DWORD milliseconds) const;
  std::vector<wchar_t> childEnvironment() const;
  const std::wstring& name() const { return name_; }

private:
  Channel() = default;
  struct Shared;
  HANDLE mapping_ = nullptr;
  HANDLE event_ = nullptr;
  Shared* data_ = nullptr;
  std::wstring name_;
};

ULONGLONG processCreationTime(HANDLE process);
const wchar_t* stateName(State state);
}
