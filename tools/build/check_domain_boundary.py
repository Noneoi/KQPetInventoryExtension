#!/usr/bin/env python3
"""Check Domain's source boundary; complement the QtCore-only whole-archive link.

This is an architectural regression check, not a complete C++ parser/security
sandbox. Comments/literals are ignored for API checks. QtCore value headers are
explicitly allowed; new dependencies require an intentional list change.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys


QT_CORE_HEADERS = {
    "QBitArray", "QByteArray", "QChar", "QCryptographicHash", "QDate", "QDateTime",
    "QElapsedTimer",  # Work-slice/measurement budgets, never business dates.
    "QHash", "QHashFunctions", "QJsonArray", "QJsonDocument", "QJsonObject", "QJsonParseError",
    "QJsonValue", "QList", "QMap", "QMetaType", "QPair", "QRegularExpression",
    "QSet", "QString", "QStringList", "QStringView", "QTime", "QTimeZone",
    "QUrl", "QVariant", "QVector", "QtGlobal",
}
STANDARD_HEADERS = {
    "algorithm", "array", "atomic", "bit", "cassert", "cctype", "charconv",
    "chrono", "climits", "cmath", "cstddef", "cstdint", "cstring", "cwctype",
    "deque", "exception", "functional", "initializer_list", "iterator",
    "limits", "map", "memory", "numeric", "optional", "set", "span",
    "stdexcept", "string", "string_view", "tuple", "type_traits",
    "unordered_map", "unordered_set", "utility", "variant", "vector",
}
FORBIDDEN = {
    "runtime/application/IO object": re.compile(
        r"\b(?:PetRepository|InventoryReadView|AnalysisReadView|ApplicationRuntime|"
        r"StorageService|StorageWriteContext|CatalogIoService|OriginalBridge|"
        r"PetRefreshController|ShopExchangeController|RoutineOverviewController|"
        r"AssetAnalysisController|PetDetailCatalog|ShopExchangeCatalog|RoutineOverviewCatalog)\b"),
    "UI, event loop, filesystem or network Qt API": re.compile(
        r"\b(?:QObject|QThread|QTimer|QEventLoop|QCoreApplication|QApplication|"
        r"QWidget|Q[A-Za-z0-9_]*Widget|QPixmap|QImage|QImageReader|QPainter|"
        r"QAbstractItemModel|QAbstractTableModel|QFile|QFileInfo|QSaveFile|QDir|"
        r"QDirIterator|QSettings|QResource|QNetwork[A-Za-z0-9_]*|QProcess|QLockFile)\b"),
    "runtime catalog singleton": re.compile(r"\b\w*Catalog\s*::\s*instance\s*\("),
    "implicit business clock": re.compile(
        r"\b(?:QDate|QDateTime|QTime)\s*::\s*current\w*\s*\(|"
        r"\bQTimeZone\s*::\s*systemTimeZone\w*\s*\(|"
        r"\bsystem_clock\s*::\s*now\s*\(|\b(?:time|_time64)\s*\("),
    "Win32 adapter API": re.compile(
        r"\b(?:CreateFile[AW]?|GetFileAttributes\w*|ReadFile|WriteFile|CloseHandle|"
        r"CreateThread|WaitForSingleObject|GetModuleHandle\w*|GetModuleFileName\w*|"
        r"LoadLibrary\w*|GetProcAddress|VirtualAlloc\w*|VirtualProtect\w*|"
        r"WideCharToMultiByte|MultiByteToWideChar|GetLocalTime|GetSystemTime\w*)\s*\("),
    "C/C++ IO or process API": re.compile(
        r"\bstd\s*::\s*(?:filesystem|[iof]*fstream)\b|"
        r"\b(?:fopen|freopen|fread|fwrite|popen|_popen|system)\s*\("),
}
RAW_START = re.compile(r'(?:u8|u|U|L)?R"([^ ()\\\t\r\n]{0,16})\(')
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"\r\n]+)[>"]\s*$', re.M)
INCLUDE_LINE = re.compile(r'^\s*#\s*include\b[^\r\n]*', re.M)

# Reviewed shared value contracts only. Their transitive includes and API use
# are inspected below just like Domain sources; this is not a directory waiver.
# pet_detail_types -> pet_derivation_types -> pet_record_types contains frozen
# identities, QtCore value data and opaque lifetime leases, with no IO behavior.
REVIEWED_CONTRACTS = {"pet_detail_types.h", "pet_derivation_types.h", "pet_record_types.h"}


def reviewed_contracts(root: Path) -> set[Path]:
    return {(root.parent / "contracts" / name).resolve() for name in REVIEWED_CONTRACTS}


def masked(text: str, literals: bool) -> str:
    """Preserve line positions while masking C++ comments and optionally strings."""
    output = list(text)
    index = 0

    def erase(start: int, end: int) -> None:
        for at in range(start, end):
            if output[at] not in "\r\n":
                output[at] = " "

    while index < len(text):
        if text.startswith("//", index):
            end = text.find("\n", index)
            if end < 0:
                end = len(text)
            erase(index, end)
        elif text.startswith("/*", index):
            found = text.find("*/", index + 2)
            end = len(text) if found < 0 else found + 2
            erase(index, end)
        else:
            raw = RAW_START.match(text, index)
            if raw:
                delimiter = ")" + raw.group(1) + '"'
                found = text.find(delimiter, raw.end())
                end = len(text) if found < 0 else found + len(delimiter)
                if literals:
                    erase(index, end)
            elif text[index] in "\"'":
                quote = text[index]
                end = index + 1
                while end < len(text):
                    if text[end] == "\\":
                        end += 2
                    elif text[end] == quote:
                        end += 1
                        break
                    else:
                        end += 1
                end = min(end, len(text))
                if literals:
                    erase(index, end)
            else:
                index += 1
                continue
        index = end
    return "".join(output)


def relative_to(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False


def inspect(path: Path, root: Path, text: str) -> list[str]:
    errors: list[str] = []
    comments_removed = masked(text, False)
    for directive in INCLUDE_LINE.finditer(comments_removed):
        match = INCLUDE.fullmatch(directive.group())
        line = text.count("\n", 0, directive.start()) + 1
        if not match:
            errors.append(f"{path.name}:{line}: macro/multiline include is not an explicit Domain dependency")
            continue
        kind, name = match.groups()
        if kind == '"':
            # Same directory first, then the src/ include root used by the build.
            candidates = [(path.parent / name).resolve(), (root.parent / name).resolve()]
            resolved = next((candidate for candidate in candidates if candidate.is_file()), candidates[0])
            if (not relative_to(resolved, root) and resolved not in reviewed_contracts(root)) or not resolved.is_file():
                errors.append(f"{path.name}:{line}: local include leaves Domain or is missing: {name}")
        else:
            qt_name = name.removeprefix("QtCore/")
            if qt_name not in QT_CORE_HEADERS and name not in STANDARD_HEADERS:
                errors.append(f"{path.name}:{line}: non-value/unknown external header: {name}")
    code = masked(text, True)
    for reason, expression in FORBIDDEN.items():
        for match in expression.finditer(code):
            line = text.count("\n", 0, match.start()) + 1
            errors.append(f"{path.name}:{line}: {reason}: {match.group().strip()}")
    return errors


def self_test(root: Path) -> None:
    probe = root / "boundary_probe.cpp"
    allowed = '#include <QElapsedTimer>\n#include <QBitArray>\n// QFile QDate::currentDate()\nauto s=R"tag(QFile /* literal */)tag";\n'
    assert not inspect(probe, root, allowed)
    cases = [
        '#include "application/pet/pet_repository.h"\n', '#include <QWidget>\n',
        '#include "protocol/session_context.h"\n', '#include "application/runtime/application_runtime.h"\n',
        '#include "../extension/extension_context.h"\n', '#include "contracts/missing_contract.h"\n',
        '#include <windows.h>\n', '#include SOME_HEADER\n',
        'auto x=QDate::currentDate();', 'auto& x=PetDetailCatalog::instance();',
        'QObject object;', 'auto x=QFile(path);', 'CreateFileW(path);',
        'auto s="escaped\\\" text"; WriteFile(handle);',
    ]
    for source in cases:
        assert inspect(probe, root, source), f"checker missed negative fixture: {source}"
    contract_probe = root.parent / "contracts" / "pet_detail_types.h"
    assert inspect(contract_probe, root, '#include "application/runtime/application_runtime.h"\n')
    assert not inspect(contract_probe, root, '#include "domain/pet_analysis_facts.h"\n')
    assert not inspect(probe, root, '#include "contracts/pet_detail_types.h"\n')
    assert inspect(contract_probe, root, 'QObject* hiddenRuntime;\n')


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[2] / "src" / "domain")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = args.root.resolve()
    if args.self_test:
        self_test(root)
    files = sorted(path for path in root.rglob("*") if path.suffix in {".h", ".hpp", ".cpp", ".cc"})
    if not files:
        print(f"FAIL: no Domain C++ sources at {root}", file=sys.stderr)
        return 1
    errors = []
    for path in files:
        if not relative_to(path.resolve(), root):
            errors.append(f"{path.name}: source symlink leaves Domain")
            continue
        errors.extend(inspect(path, root, path.read_text(encoding="utf-8-sig")))
    contracts = sorted(reviewed_contracts(root))
    for path in contracts:
        if not path.is_file():
            errors.append(f"{path.name}: reviewed value contract is missing")
            continue
        errors.extend(inspect(path, root, path.read_text(encoding="utf-8-sig")))
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"PASS: {len(files)} Domain C++ files and {len(contracts)} reviewed value contracts; no forbidden runtime/IO/UI calls")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
