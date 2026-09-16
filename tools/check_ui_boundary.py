#!/usr/bin/env python3
"""Architectural regression check for KQPetUi's actual CMake source boundary.

This complements linking/UI tests; it is not a full CMake/C++ evaluator or a
security sandbox. Unresolved source/link expressions require explicit review.
QtGui, QtWidgets and native Windows UI APIs are legitimate in this boundary.
"""
from __future__ import annotations

import argparse
from collections import defaultdict, deque
from pathlib import Path
import re
import sys
sys.dont_write_bytecode = True

from check_domain_boundary import INCLUDE, INCLUDE_LINE, masked, relative_to


UI_TARGET = "KQPetUi"
UI_LIBRARIES = {"KQPetDomain", "KQPetReadViews", "KQPetImages"}
FORBIDDEN_LIBRARIES = {"KQPetCore", "KQPetApplication", "KQPetApplicationCore", "KQPetBridge"}
# Exact read/command/lease interfaces, not permission for application/*.h.
PORTS = {
    "src/application/inventory_read_view.h", "src/application/analysis_read_view.h",
    "src/application/image_service.h",
    "src/application/contracts/refresh_timings.h", "src/application/contracts/operation_types.h",
    "src/contracts/pet_record_types.h", "src/contracts/pet_derivation_types.h",
    "src/contracts/pet_detail_types.h", "src/contracts/observation_types.h",
    # Header-only value contracts: one snapshot struct plus an inline key helper
    # for cultivation materials, and the local stargod statistics result. They
    # carry no implementation and their own includes are still inspected.
    "src/contracts/cultivation_material_inventory.h", "src/contracts/local_stargod_statistics.h",
}
# Value wrappers and header-only presentation helpers not listed as library
# sources. Their contents/includes are recursively checked, never trusted blind.
VALUE_HELPERS = {
    "src/extension/asset_analysis_types.h", "src/extension/asset_analysis_version.h",
    "src/extension/asset_snapshot_comparator.h", "src/extension/recommendation_types.h",
    "src/extension/pet_identity.h", "src/extension/pet_move_policy.h",
    "src/extension/pet_detail_view_model.h", "src/extension/pet_facts_ui.h",
    "src/extension/build_info.h",
}
FORBIDDEN_APIS = {
    "runtime implementation type": re.compile(
        r"\b(?:PetRepository|ApplicationRuntime|ExtensionContext|OriginalBridge|StorageService|"
        r"CatalogIoService|PetDerivationCache|PetDetailPreparationService|"
        r"PetRefreshController|AssetAnalysisController|ShopExchangeController|"
        r"RoutineOverviewController|PetDetailCatalog|ShopExchangeCatalog|RoutineOverviewCatalog|"
        r"PetDetailAnalyzer)\b"),
    "runtime catalog singleton": re.compile(r"\b\w*Catalog\s*::\s*instance\s*\("),
    "cultivation calculation in UI": re.compile(
        r"\b(?:calculatePetBattlePower|derivePetAnalysisFacts)\s*\(|"
        r"\bAssetDerivation\s*::\s*derivePet(?:WithPower)?\s*\("),
}
SCOPE_WORDS = {"PUBLIC", "PRIVATE", "INTERFACE", "STATIC", "SHARED", "MODULE", "OBJECT", "EXCLUDE_FROM_ALL"}
CMAKE_TOKEN = re.compile(r'"(?:\\.|[^"\\])*"|[^\s;]+')
CMAKE_START = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\(")


def cmake_commands(text: str):
    # Preserve offsets for diagnostics. Current target declarations use quoted
    # or bare arguments, not arbitrary configure-time generated source lists.
    cleaned = re.sub(r"#\[(=*)\[.*?\]\1\]", lambda m: "\n" * m.group().count("\n"), text, flags=re.S)
    cleaned = re.sub(r"(?m)^\s*#.*$", "", cleaned)
    cursor = 0
    while match := CMAKE_START.search(cleaned, cursor):
        start = match.end()
        at, depth, quoted = start, 1, False
        while at < len(cleaned) and depth:
            char = cleaned[at]
            if char == "\\" and quoted:
                at += 2
                continue
            if char == '"':
                quoted = not quoted
            elif not quoted and char == "#":
                end = cleaned.find("\n", at)
                at = len(cleaned) if end < 0 else end
                continue
            elif not quoted and char == "(":
                depth += 1
            elif not quoted and char == ")":
                depth -= 1
            at += 1
        if depth:
            raise ValueError(f"unclosed CMake command {match.group(1)}")
        body = re.sub(r"(?m)#.*$", "", cleaned[start:at - 1])
        tokens = [token[1:-1] if token.startswith('"') else token for token in CMAKE_TOKEN.findall(body)]
        yield match.group(1).lower(), tokens
        cursor = at


def build_boundary(text: str):
    sources: list[str] = []
    links: dict[str, list[str]] = defaultdict(list)
    aliases: dict[str, str] = {}
    targets: set[str] = set()
    errors: list[str] = []
    for command, arguments in cmake_commands(text):
        if not arguments:
            continue
        owner = arguments[0]
        if command in {"add_library", "add_executable"}:
            targets.add(owner)
            if len(arguments) >= 3 and arguments[1] == "ALIAS":
                aliases[owner] = arguments[2]
            elif owner == UI_TARGET:
                sources.extend(value for value in arguments[1:] if value not in SCOPE_WORDS)
        elif command == "target_sources" and owner == UI_TARGET:
            sources.extend(value for value in arguments[1:] if value not in SCOPE_WORDS)
        elif command == "target_link_libraries":
            links[owner].extend(value for value in arguments[1:] if value not in SCOPE_WORDS)
        elif command == "set_property" and owner == "TARGET" and "PROPERTY" in arguments:
            at = arguments.index("PROPERTY")
            if at + 1 < len(arguments) and arguments[at + 1] in {"LINK_LIBRARIES", "INTERFACE_LINK_LIBRARIES"}:
                for target in arguments[1:at]:
                    if target not in {"APPEND", "APPEND_STRING"}:
                        links[target].extend(arguments[at + 2:])
        elif command == "set_target_properties" and "PROPERTIES" in arguments:
            at = arguments.index("PROPERTIES")
            for index in range(at + 1, len(arguments) - 1, 2):
                if arguments[index] in {"LINK_LIBRARIES", "INTERFACE_LINK_LIBRARIES"}:
                    for target in arguments[:at]:
                        links[target].append(arguments[index + 1])

    def canonical(name: str) -> str:
        seen: set[str] = set()
        while name in aliases and name not in seen:
            seen.add(name)
            name = aliases[name]
        return name

    if UI_TARGET not in targets or not sources:
        errors.append("CMakeLists.txt: KQPetUi has no explicit auditable source list")
    pending = deque([(UI_TARGET, [UI_TARGET])])
    visited: set[str] = set()
    while pending:
        target, chain = pending.popleft()
        if target in visited:
            continue
        visited.add(target)
        if target in FORBIDDEN_LIBRARIES:
            errors.append("CMakeLists.txt: forbidden UI link chain: " + " -> ".join(chain))
        for value in links[target]:
            if "${" in value:
                errors.append(f"CMakeLists.txt: unresolved link expression reachable from UI: {value}")
                continue
            if "$<" in value:
                words = re.findall(r"[A-Za-z_][A-Za-z0-9_]*(?:::[A-Za-z0-9_]+)*", value)
                references = [word for word in words if word in targets or word in aliases or word in FORBIDDEN_LIBRARIES]
            else:
                references = value.split(";")
            for reference in references:
                dependency = canonical(reference)
                if target == UI_TARGET and dependency in targets and dependency not in UI_LIBRARIES:
                    errors.append(f"CMakeLists.txt: unreviewed direct UI library: {reference}")
                if dependency in targets or dependency in FORBIDDEN_LIBRARIES:
                    pending.append((dependency, chain + [dependency]))
    for source in sources:
        if "$" in source or source.startswith("-") or Path(source).suffix not in {".h", ".hpp", ".cpp", ".cc", ".qrc"}:
            errors.append(f"CMakeLists.txt: unresolved/unsupported UI source expression: {source}")
    return sources, errors


def inspect_source(path: Path, project: Path, source: str, ui_files: set[Path]):
    errors: list[str] = []
    children: list[Path] = []
    approved = {project / name for name in PORTS | VALUE_HELPERS} | ui_files
    domain = project / "src/domain"
    roots = [path.parent, project / "src/extension", project / "src/application",
             domain, project / "src/contracts", project / "src", project]
    label = path.relative_to(project).as_posix() if relative_to(path, project) else str(path)
    for directive in INCLUDE_LINE.finditer(masked(source, False)):
        match = INCLUDE.fullmatch(directive.group())
        line = source.count("\n", 0, directive.start()) + 1
        if not match:
            errors.append(f"{label}:{line}: macro/multiline include requires explicit review")
            continue
        kind, name = match.groups()
        resolved = next((candidate.resolve() for root in roots if (candidate := root / name).is_file()), None)
        if resolved is None:
            if kind == '"':
                errors.append(f"{label}:{line}: unresolved quoted UI include: {name}")
            elif "/application/" in "/" + name or name.endswith(("_repository.h", "_controller.h", "_catalog.h")):
                errors.append(f"{label}:{line}: runtime header is not a UI port: {name}")
            # System Qt/standard/Win32 headers are allowed: UI is not Domain.
            continue
        if relative_to(resolved, domain):
            continue  # Independently audited by check_domain_boundary.
        if resolved not in approved:
            errors.append(f"{label}:{line}: implementation/unreviewed local UI include: {name}")
        else:
            children.append(resolved)
    code = masked(source, True)
    for reason, expression in FORBIDDEN_APIS.items():
        for match in expression.finditer(code):
            line = source.count("\n", 0, match.start()) + 1
            errors.append(f"{label}:{line}: {reason}: {match.group().strip()}")
    return children, errors


def check(project: Path):
    sources, errors = build_boundary((project / "CMakeLists.txt").read_text(encoding="utf-8-sig"))
    ui_files = {project / name for name in sources if "$" not in name}
    pending = list(ui_files)
    visited: set[Path] = set()
    while pending:
        path = pending.pop()
        if path in visited or path.suffix == ".qrc":
            continue
        visited.add(path)
        if not relative_to(path.resolve(), project) or path.resolve() != path or not path.is_file():
            errors.append(f"{path}: missing/noncanonical UI source or source symlink")
            continue
        children, found = inspect_source(path, project, path.read_text(encoding="utf-8-sig"), ui_files)
        errors.extend(found)
        pending.extend(children)
    return len(ui_files), len(visited), errors


def self_test(project: Path):
    base = "add_library(KQPetUi STATIC src/extension/shop_window.cpp)\n"
    assert not build_boundary(base + "target_link_libraries(KQPetUi PUBLIC Qt6::Widgets user32)")[1]
    assert not build_boundary(base + "set_property(TARGET AnotherTarget PROPERTY LINK_LIBRARIES KQPetCore)")[1]
    negative = [
        "target_link_libraries(KQPetUi PRIVATE KQPetCore)",
        "target_link_libraries(KQPetUi INTERFACE KQPetApplication)",
        "add_library(KQPet::Core ALIAS KQPetCore)\ntarget_link_libraries(KQPetUi PUBLIC KQPet::Core)",
        "target_link_libraries(KQPetUi PRIVATE $<LINK_ONLY:KQPetBridge>)",
        "add_library(HiddenAlias ALIAS KQPetBridge)\ntarget_link_libraries(KQPetUi PRIVATE $<LINK_ONLY:HiddenAlias>)",
        "add_library(KQPetImages STATIC images.cpp)\ntarget_link_libraries(KQPetUi PRIVATE KQPetImages)\n"
        "target_link_libraries(KQPetImages PRIVATE KQPetApplicationCore)",
        "target_link_libraries(KQPetUi PRIVATE ${hidden_dependencies})",
        "target_sources(KQPetUi PRIVATE $<TARGET_OBJECTS:hidden_app>)",
        "set_property(TARGET KQPetUi PROPERTY LINK_LIBRARIES KQPetCore)",
        'set_target_properties(KQPetUi PROPERTIES LINK_LIBRARIES "Qt6::Widgets;KQPetCore")',
    ]
    for source in negative:
        assert build_boundary(base + source)[1], f"missed negative link/source fixture: {source}"
    probe = project / "src/extension/ui_boundary_probe.cpp"
    allowed = '#include <QWidget>\n#include <QImage>\n#include <windows.h>\n#include "inventory_read_view.h"\n'
    allowed += '// PetDetailAnalyzer PetRepository\nauto s=R"x(PetDetailCatalog::instance())x";\n'
    assert not inspect_source(probe, project, allowed, set())[1]
    for source in [
        '#include "pet_detail_catalog.h"', '#include <pet_repository.h>',
        '#include "../application/application_runtime.h"', '#include "pet_refresh_controller.h"',
        '#include "shop_pet_eligibility.h"', '#include SECRET_HEADER',
        'auto x=ShopExchangeCatalog::instance();', 'PetDetailAnalyzer::analyze(value);',
        'calculatePetBattlePower(raw);', 'auto s="escaped\\\""; PetRepository* repository;',
    ]:
        assert inspect_source(probe, project, source, set())[1], f"missed negative include/API fixture: {source}"
    port = project / "src/application/inventory_read_view.h"
    assert inspect_source(port, project, '#include "application_runtime.h"', set())[1]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project-root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    project = args.project_root.resolve()
    if args.self_test:
        self_test(project)
    sources, inspected, errors = check(project)
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"PASS: KQPetUi links/source boundary; {sources} target sources and {inspected} UI/port/value files inspected")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
