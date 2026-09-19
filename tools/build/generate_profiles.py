"""Validate reviewed profiles and emit a static C++ registry; standard library only."""
import argparse
import hashlib
import json
import pathlib
import re


def literal(value, wide=False):
    if not isinstance(value, str) or not value or not value.isascii() or "\0" in value:
        raise ValueError("profile text must be nonempty ASCII")
    return ("L" if wide else "") + json.dumps(value, ensure_ascii=True)


def generate(source):
    catalog = json.loads(source)
    if set(catalog) != {"schema", "profiles"} or catalog["schema"] != 1:
        raise ValueError("unsupported profile schema")
    profiles = catalog["profiles"]
    if not isinstance(profiles, list) or not profiles:
        raise ValueError("empty profile registry")
    ids, aliases = set(), set()
    output = ['// Generated; edit profiles/targets.json and reviewed evidence instead.',
              '#include "compatibility/profile.h"', 'namespace kqpet::compatibility {',
              'const std::vector<Profile>& registeredProfiles() {',
              '  static const std::vector<Profile> values = [] {',
              '    std::vector<Profile> result;']
    for profile in profiles:
        required = {"id", "version", "architecture", "executableNames", "qtVersion", "qtModules",
                    "qcefViewModule", "qcefExecuteJavascriptSymbol", "evidenceReference", "endpoints"}
        if not required <= set(profile) or set(profile) - required - {"executableSha256"}:
            raise ValueError("missing or unsupported profile field")
        if profile["id"] in ids or profile["architecture"] != "amd64":
            raise ValueError("duplicate identity or unsupported architecture")
        ids.add(profile["id"])
        if profile["qtVersion"] != "6.6.3" or len(profile["qtModules"]) != 4 or set(profile["qtModules"]) != {
            "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Network.dll"
        }:
            raise ValueError("unreviewed Qt ABI/module set")
        names = profile["executableNames"]
        if not names or len({name.lower() for name in names}) != len(names) or any(name.lower() in aliases for name in names):
            raise ValueError("ambiguous executable alias")
        for name in names:
            if name not in (f'KQProV{profile["version"]}.exe', f'KQPro{profile["version"]}.exe'):
                raise ValueError("executable alias must be exact")
            aliases.add(name.lower())
        digest = profile.get("executableSha256", "")
        if digest and not re.fullmatch(r"[A-Fa-f0-9]{64}", digest):
            raise ValueError("invalid executable SHA-256")
        if profile["version"] == "1.1.4" and not digest:
            raise ValueError("V1.1.4 requires the reviewed executable digest")
        output.append('    { Profile p;')
        for key in ("id", "version", "qtVersion", "qcefViewModule"):
            output.append(f'      p.{key} = {literal(profile[key], True)};')
        output.append('      p.qtCoreModule = L"Qt6Core.dll"; p.qtWidgetsModule = L"Qt6Widgets.dll";')
        for key in ("executableNames", "qtModules"):
            output.append(f'      p.{key} = {{{", ".join(literal(v, True) for v in profile[key])}}};')
        for key in ("qcefExecuteJavascriptSymbol", "evidenceReference"):
            output.append(f'      p.{key} = {literal(profile[key])};')
        if digest:
            output.append(f'      p.executableSha256 = {literal(digest.upper())};')
        roles = set()
        for endpoint in profile["endpoints"]:
            if set(endpoint) - {"signatureMask"} != {"role", "rva", "signature", "matchPolicy", "dispatchHookAuthorized"}:
                raise ValueError("invalid endpoint fields")
            role = endpoint["role"]
            if role not in {"Dispatch", "ServiceGetter", "CommandSender"} or role in roles:
                raise ValueError("invalid/duplicate endpoint role")
            roles.add(role)
            rva = int(endpoint["rva"], 0)
            signature = bytes.fromhex(endpoint["signature"])
            if not 0 < rva <= 0xFFFFFFFF or not 1 <= len(signature) <= 32:
                raise ValueError("invalid endpoint range/signature")
            policy = endpoint["matchPolicy"]
            if policy not in {"KnownRva", "UniqueAtKnownRva", "UniqueSignature"}:
                raise ValueError("unreviewed match policy")
            mask = bytes.fromhex(endpoint.get("signatureMask", ""))
            if mask and (role != "ServiceGetter" or len(mask) != len(signature) or
                         mask != bytes([255] * 9 + [0] * 4 + [255] * (len(signature) - 13))):
                raise ValueError("only the reviewed ServiceGetter RIP displacement can be masked")
            hook = endpoint["dispatchHookAuthorized"]
            if type(hook) is not bool or hook != (role == "Dispatch"):
                raise ValueError("only Dispatch may authorize a hook")
            field = role[0].lower() + role[1:]
            output += [f'      p.{field}.name = {literal(role, True)};',
                       f'      p.{field}.rva = 0x{rva:X};',
                       f'      p.{field}.signature = {{{", ".join(f"0x{b:02X}" for b in signature)}}};',
                       f'      p.{field}.signatureSize = {len(signature)};',
                       f'      p.{field}.role = EndpointRole::{role};',
                       f'      p.{field}.matchPolicy = MatchPolicy::{policy};']
            if mask:
                output += [f'      p.{field}.signatureMask = {{{", ".join(f"0x{b:02X}" for b in mask)}}};',
                           f'      p.{field}.masked = true;']
            if hook:
                output.append(f'      p.{field}.trampolinePolicy = TrampolinePolicy::ExactRelocationFreePrologue;')
        if len(roles) != 3:
            raise ValueError("three reviewed entry roles are required")
        output.append('      result.push_back(p); }')
    output += ['    return result;', '  }();', '  return values;', '}',
               f'const char* profileCatalogDigest() {{ return "{hashlib.sha256(source).hexdigest()}"; }}',
               '}  // namespace kqpet::compatibility', '']
    return "\n".join(output)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    generated = generate(args.input.read_bytes())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text(encoding="utf-8") != generated:
        args.output.write_text(generated, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    main()
