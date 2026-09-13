#include <windows.h>
#include <string>
int wmain(int argc, wchar_t** argv) {
  if (argc != 3) return 64;
  const auto valid = [](const std::wstring& name) {
    if (name.rfind(L"Local\\KQReleaseFixture-", 0) != 0 || name.size() > 128) return false;
    for (wchar_t c : name.substr(6))
      if (!(c >= L'0' && c <= L'9') && !(c >= L'A' && c <= L'Z') &&
          !(c >= L'a' && c <= L'z') && c != L'-') return false;
    return true;
  };
  if (!valid(argv[1]) || !valid(argv[2])) return 64;
  HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[1]);
  HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
  if (!ready || !stop) { if (ready) CloseHandle(ready); if (stop) CloseHandle(stop); return 2; }
  SetEvent(ready);
  const DWORD result = WaitForSingleObject(stop, 30000);
  CloseHandle(stop); CloseHandle(ready);
  return result == WAIT_OBJECT_0 ? 0 : 3;
}
