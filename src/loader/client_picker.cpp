#include "client_picker.h"
#include "client_target.h"
#include <windows.h>
#include <commdlg.h>

namespace kqpet::launcher {
bool chooseClientExecutable(const std::filesystem::path& installation) {
  wchar_t selected[32768]{};
  OPENFILENAMEW dialog{sizeof(dialog)};
  dialog.lpstrFilter = L"氪奇主程序 (*.exe)\0*.exe\0\0";
  dialog.lpstrFile = selected;
  dialog.nMaxFile = 32768;
  dialog.lpstrInitialDir = installation.c_str();
  dialog.lpstrTitle = L"选择氪奇主程序 EXE（选择后记住路径）";
  dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
  if (!GetOpenFileNameW(&dialog)) return false;
  if (saveClientSelection(installation, selected)) return true;
  MessageBoxW(nullptr, L"未能保存选择。请选择氪奇的 EXE，并确认启动器目录可以写入。",
      L"选择氪奇主程序", MB_OK | MB_ICONWARNING);
  return false;
}
}
