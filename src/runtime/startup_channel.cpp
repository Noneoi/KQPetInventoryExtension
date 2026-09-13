#include "startup_channel.h"
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <array>
#include <cwchar>

namespace kqpet::startup {
namespace {
constexpr DWORD kMagic = 0x3250514b;
constexpr wchar_t kPrefix[] = L"Local\\KQPetStartup-";
bool validName(const std::wstring& name) {
  const std::wstring prefix(kPrefix);
  if (name.size() != prefix.size() + 32 || name.compare(0, prefix.size(), prefix)) return false;
  return std::all_of(name.begin() + prefix.size(), name.end(), [](wchar_t c) {
    return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f');
  });
}
bool userSecurity(PSECURITY_DESCRIPTOR* descriptor) {
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> bytes(size);
  const bool valid = size && GetTokenInformation(token, TokenUser, bytes.data(), size, &size);
  CloseHandle(token);
  if (!valid) return false;
  LPWSTR sid = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid, &sid)) return false;
  const std::wstring sddl = L"D:P(A;;GA;;;" + std::wstring(sid) + L")";
  LocalFree(sid);
  return ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                              descriptor, nullptr);
}
}

struct Channel::Shared {
  DWORD magic;
  DWORD protocol;
  DWORD bytes;
  DWORD pid;
  ULONGLONG creationTime;
  wchar_t nonce[33];
  wchar_t releaseId[96];
  wchar_t profileId[96];
  ULONG_PTR childMappingKeeper;
  ULONG_PTR childEventKeeper;
  volatile LONG keepersClaimed;
  alignas(8) volatile LONG64 status; // State and error form one atomic publication.
};

ULONGLONG processCreationTime(HANDLE process) {
  FILETIME creation{}, exit{}, kernel{}, user{};
  if (!GetProcessTimes(process, &creation, &exit, &kernel, &user)) return 0;
  return (ULONGLONG(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
}

Channel::~Channel() {
  if (data_) UnmapViewOfFile(data_);
  if (event_) CloseHandle(event_);
  if (mapping_) CloseHandle(mapping_);
}

std::shared_ptr<Channel> Channel::create(const std::wstring& releaseId,
    const std::wstring& profileId, std::wstring* error) {
  if (releaseId.empty() || releaseId.size() >= 96 || profileId.empty() || profileId.size() >= 96) return {};
  auto channel = std::shared_ptr<Channel>(new Channel);
  std::array<unsigned char, 16> random{};
  if (BCryptGenRandom(nullptr, random.data(), DWORD(random.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return {};
  std::wstring nonce;
  for (unsigned char value : random) { nonce += L"0123456789abcdef"[value >> 4]; nonce += L"0123456789abcdef"[value & 15]; }
  channel->name_ = kPrefix + nonce;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!userSecurity(&descriptor)) { if (error) *error = L"current-user channel ACL could not be created"; return {}; }
  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
  channel->mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, &security, PAGE_READWRITE, 0,
      sizeof(Shared), channel->name_.c_str());
  const bool newMapping = channel->mapping_ && GetLastError() != ERROR_ALREADY_EXISTS;
  if (newMapping)
    channel->event_ = CreateEventW(&security, FALSE, FALSE, (channel->name_ + L"-event").c_str());
  const bool newEvent = channel->event_ && GetLastError() != ERROR_ALREADY_EXISTS;
  LocalFree(descriptor);
  if (!newMapping || !newEvent) { if (error) *error = L"startup channel creation failed or collided"; return {}; }
  channel->data_ = static_cast<Shared*>(MapViewOfFile(channel->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
  if (!channel->data_) return {};
  Shared& data = *channel->data_;
  data.magic = kMagic; data.protocol = kProtocol; data.bytes = sizeof(Shared);
  wcscpy_s(data.nonce, nonce.c_str());
  wcscpy_s(data.releaseId, releaseId.c_str());
  wcscpy_s(data.profileId, profileId.c_str());
  InterlockedExchange64(&data.status, LONG64(State::Prepared));
  return channel;
}

bool Channel::bindChild(HANDLE process, DWORD pid) {
  if (!data_ || !pid || data_->pid || state() != State::Prepared) return false;
  const auto created = processCreationTime(process);
  if (!created || GetProcessId(process) != pid) return false;
  HANDLE mappingKeeper = nullptr;
  HANDLE eventKeeper = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), mapping_, process, &mappingKeeper, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
  if (!DuplicateHandle(GetCurrentProcess(), event_, process, &eventKeeper, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
    HANDLE cleanup = nullptr;
    if (DuplicateHandle(process, mappingKeeper, GetCurrentProcess(), &cleanup, 0, FALSE,
                        DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) CloseHandle(cleanup);
    return false;
  }
  // These handles belong to the child, so they survive even if the loader times
  // out before DllMain's initialization worker gets any CPU time.
  data_->childMappingKeeper = reinterpret_cast<ULONG_PTR>(mappingKeeper);
  data_->childEventKeeper = reinterpret_cast<ULONG_PTR>(eventKeeper);
  data_->creationTime = created;
  data_->pid = pid;
  MemoryBarrier();
  return true;
}

bool Channel::identityMatches(const Identity& identity) const {
  if (!data_ || data_->magic != kMagic || data_->protocol != kProtocol || data_->bytes != sizeof(Shared)) return false;
  const auto equals = [](const wchar_t* value, std::size_t capacity, const std::wstring& expected) {
    return expected.size() < capacity && std::wmemcmp(value, expected.c_str(), expected.size() + 1) == 0;
  };
  return data_->pid == identity.pid && data_->creationTime == identity.creationTime &&
      equals(data_->nonce, 33, name_.substr(std::wstring(kPrefix).size())) &&
      equals(data_->releaseId, 96, identity.releaseId) && equals(data_->profileId, 96, identity.profileId);
}

std::shared_ptr<Channel> Channel::openForCurrentProcess(const std::wstring& releaseId,
    const std::wstring& profileId, std::wstring* error) {
  wchar_t name[256]{};
  const DWORD size = GetEnvironmentVariableW(kEnvironmentKey, name, 256);
  if (!size || size >= 256 || !validName(name)) { if (error) *error = L"private startup channel not supplied"; return {}; }
  auto channel = std::shared_ptr<Channel>(new Channel);
  channel->name_ = name;
  channel->mapping_ = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name);
  if (channel->mapping_)
    channel->data_ = static_cast<Shared*>(MapViewOfFile(channel->mapping_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
  channel->event_ = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, (channel->name_ + L"-event").c_str());
  const Identity identity{GetCurrentProcessId(), processCreationTime(GetCurrentProcess()), releaseId, profileId};
  if (!channel->event_ || !channel->identityMatches(identity)) {
    if (error) *error = L"startup identity/nonce/PID/creation time/release/profile mismatch";
    return {};
  }
  if (InterlockedCompareExchange(&channel->data_->keepersClaimed, 1, 0) == 0) {
    CloseHandle(reinterpret_cast<HANDLE>(channel->data_->childMappingKeeper));
    CloseHandle(reinterpret_cast<HANDLE>(channel->data_->childEventKeeper));
  }
  return channel;
}

State Channel::state() const {
  return data_ ? State(DWORD(InterlockedCompareExchange64(&data_->status, 0, 0))) : State::Failed;
}
DWORD Channel::errorCode() const {
  return data_ ? DWORD(ULONGLONG(InterlockedCompareExchange64(&data_->status, 0, 0)) >> 32) : ERROR_INVALID_HANDLE;
}
bool Channel::publish(State next, DWORD errorCode) {
  if (!data_) return false;
  const LONG64 observed = InterlockedCompareExchange64(&data_->status, 0, 0);
  const State previous = State(DWORD(observed));
  if (previous == State::Ready || previous == State::Failed ||
      (next != State::Failed && LONG(next) != LONG(previous) + 1)) return false;
  const LONG64 desired = LONG64((ULONGLONG(next == State::Failed ? errorCode : 0) << 32) | DWORD(next));
  if (InterlockedCompareExchange64(&data_->status, desired, observed) != observed) return false;
  SetEvent(event_);
  return true;
}
bool Channel::wait(DWORD milliseconds) const {
  return event_ && WaitForSingleObject(event_, milliseconds) == WAIT_OBJECT_0;
}

std::vector<wchar_t> Channel::childEnvironment() const {
  std::vector<std::wstring> entries;
  LPWCH environment = GetEnvironmentStringsW();
  if (!environment) return {};
  const std::wstring privatePrefix = std::wstring(kEnvironmentKey) + L"=";
  for (const wchar_t* entry = environment; *entry; entry += std::wcslen(entry) + 1)
    if (_wcsnicmp(entry, privatePrefix.c_str(), privatePrefix.size())) entries.emplace_back(entry);
  FreeEnvironmentStringsW(environment);
  entries.push_back(privatePrefix + name_);
  std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
  std::vector<wchar_t> block;
  for (const auto& entry : entries) { block.insert(block.end(), entry.begin(), entry.end()); block.push_back(0); }
  block.push_back(0);
  return block;
}
const wchar_t* stateName(State state) {
  switch (state) {
    case State::Prepared: return L"PREPARED";
    case State::ImageLoaded: return L"IMAGE_LOADED";
    case State::Compatible: return L"COMPATIBLE";
    case State::BridgeReady: return L"BRIDGE_READY";
    case State::Ready: return L"READY";
    case State::Failed: return L"FAILED";
  }
  return L"INVALID";
}
}
