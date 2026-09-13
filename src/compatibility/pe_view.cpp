#include "pe_view.h"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace kqpet::compatibility {
namespace {
bool range(std::size_t offset, std::size_t length, std::size_t extent) {
  return offset <= extent && length <= extent - offset;
}
bool overlap(std::uint64_t a, std::uint64_t sizeA, std::uint64_t b, std::uint64_t sizeB) {
  return sizeA && sizeB && a < b + sizeB && b < a + sizeA;
}
bool readable(const void* address, std::size_t size) {
  std::uintptr_t cursor = reinterpret_cast<std::uintptr_t>(address);
  if (size > std::numeric_limits<std::uintptr_t>::max() - cursor) return false;
  const std::uintptr_t end = cursor + size;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION page{};
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &page, sizeof(page)) != sizeof(page) ||
        page.State != MEM_COMMIT || (page.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const DWORD access = page.Protect & 0xff;
    if (access != PAGE_READONLY && access != PAGE_READWRITE && access != PAGE_WRITECOPY &&
        access != PAGE_EXECUTE_READ && access != PAGE_EXECUTE_READWRITE &&
        access != PAGE_EXECUTE_WRITECOPY) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(page.BaseAddress);
    if (page.RegionSize > std::numeric_limits<std::uintptr_t>::max() - base ||
        base + page.RegionSize <= cursor) return false;
    cursor = (std::min)(end, base + page.RegionSize);
  }
  return true;
}
}

std::size_t PeSection::fileBackedSize() const {
  return (std::min)(rawSize, virtualSize ? virtualSize : rawSize);
}

PeView::PeView(const void* bytes, std::size_t size, ImageLayout layout)
    : bytes_(static_cast<const unsigned char*>(bytes)), size_(size), layout_(layout) {
  if (!parse() && error_.empty()) error_ = "invalid or truncated PE";
}

bool PeView::readOffset(std::size_t offset, void* output, std::size_t size) const {
  if (!bytes_ || !range(offset, size, size_)) return false;
  const auto address = reinterpret_cast<std::uintptr_t>(bytes_);
  if (offset > std::numeric_limits<std::uintptr_t>::max() - address) return false;
  const void* source = reinterpret_cast<const void*>(address + offset);
  if (layout_ == ImageLayout::Mapped && !readable(source, size)) return false;
  if (size && output) std::memcpy(output, source, size);
  return true;
}

bool PeView::parse() {
  IMAGE_DOS_HEADER dos{};
  if (!readOffset(0, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
      dos.e_lfanew < static_cast<LONG>(sizeof(dos))) return false;
  const auto ntOffset = static_cast<std::size_t>(dos.e_lfanew);
  DWORD signature = 0;
  IMAGE_FILE_HEADER file{};
  if (!readOffset(ntOffset, &signature, sizeof(signature)) || signature != IMAGE_NT_SIGNATURE ||
      !readOffset(ntOffset + sizeof(DWORD), &file, sizeof(file)) ||
      file.Machine != IMAGE_FILE_MACHINE_AMD64 || file.NumberOfSections == 0 ||
      file.NumberOfSections > 96 || file.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64)) return false;
  const std::size_t optionalOffset = ntOffset + sizeof(DWORD) + sizeof(file);
  IMAGE_OPTIONAL_HEADER64 optional{};
  if (!readOffset(optionalOffset, &optional, sizeof(optional)) ||
      optional.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || !optional.SizeOfImage ||
      optional.SizeOfImage > 512u * 1024u * 1024u ||
      !optional.SizeOfHeaders || optional.SizeOfHeaders > optional.SizeOfImage ||
      !optional.SectionAlignment || !optional.FileAlignment || optional.NumberOfRvaAndSizes > 16) return false;
  imageSize_ = optional.SizeOfImage;
  headerSize_ = optional.SizeOfHeaders;
  timestamp_ = file.TimeDateStamp;
  fileCharacteristics_ = file.Characteristics;
  if (layout_ == ImageLayout::Mapped && imageSize_ > size_) return false;
  if (headerSize_ > size_) return false;
  const std::size_t sectionOffset = optionalOffset + file.SizeOfOptionalHeader;
  if (!range(sectionOffset, static_cast<std::size_t>(file.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER),
             headerSize_)) return false;
  if (optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_EXPORT) {
    exportRva_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress;
    exportSize_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].Size;
    if ((exportRva_ == 0) != (exportSize_ == 0) || !range(exportRva_, exportSize_, imageSize_)) return false;
  }
  if (optional.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT) {
    importRva_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    importSize_ = optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size;
    if ((importRva_ == 0) != (importSize_ == 0) || !range(importRva_, importSize_, imageSize_)) return false;
  }
  for (WORD index = 0; index < file.NumberOfSections; ++index) {
    IMAGE_SECTION_HEADER header{};
    if (!readOffset(sectionOffset + index * sizeof(header), &header, sizeof(header))) return false;
    PeSection section{header.VirtualAddress, header.Misc.VirtualSize,
                      header.PointerToRawData, header.SizeOfRawData, header.Characteristics};
    const std::size_t virtualExtent = (std::max)(section.virtualSize, section.rawSize);
    if (!virtualExtent || section.rva < headerSize_ || !range(section.rva, virtualExtent, imageSize_) ||
        (section.rawSize && section.rawOffset < headerSize_) ||
        (layout_ == ImageLayout::Raw && !range(section.rawOffset, section.rawSize, size_))) return false;
    for (const auto& previous : sections_) {
      if (overlap(section.rva, virtualExtent, previous.rva,
                   (std::max)(previous.virtualSize, previous.rawSize)) ||
          overlap(section.rawOffset, section.rawSize, previous.rawOffset, previous.rawSize)) {
        error_ = "overlapping PE sections";
        return false;
      }
    }
    sections_.push_back(section);
  }
  return true;
}

bool PeView::readRva(std::uint32_t rva, void* output, std::size_t size,
                      bool executableOnly) const {
  if (!valid()) return false;
  if (!executableOnly && rva < headerSize_ && range(rva, size, headerSize_))
    return readOffset(rva, output, size);
  for (const auto& section : sections_) {
    if (rva < section.rva || (executableOnly && !(section.characteristics & IMAGE_SCN_MEM_EXECUTE))) continue;
    const std::size_t relative = rva - section.rva;
    // Both layouts use exactly the same file-supported bytes. Mapped zero-fill
    // and raw padding past VirtualSize must never become signature evidence.
    if (!range(relative, size, section.fileBackedSize())) continue;
    const std::size_t offset = layout_ == ImageLayout::Raw ? section.rawOffset + relative : rva;
    return readOffset(offset, output, size);
  }
  return false;
}

EndpointMatch PeView::resolve(const EndpointProfile& endpoint) const {
  EndpointMatch result;
  if (!valid()) { result.error = error_; return result; }
  const std::size_t length = endpoint.signatureSize;
  if (!length || length > endpoint.signature.size() || endpoint.rva > UINT32_MAX) {
    result.error = "invalid endpoint profile"; return result;
  }
  const auto rva = static_cast<std::uint32_t>(endpoint.rva);
  const auto matches = [&endpoint, length](const unsigned char* bytes) {
    for (std::size_t i = 0; i < length; ++i) {
      const unsigned char mask = endpoint.masked ? endpoint.signatureMask[i] : 0xff;
      if ((bytes[i] & mask) != (endpoint.signature[i] & mask)) return false;
    }
    return true;
  };
  std::array<unsigned char, 32> known{};
  if (endpoint.matchPolicy != MatchPolicy::UniqueSignature &&
      (!readRva(rva, known.data(), length, true) || !matches(known.data()))) {
    result.method = MatchMethod::Conflict;
    result.error = "reviewed RVA differs or is not readable executable code; fallback forbidden";
    return result;
  }
  if (endpoint.matchPolicy == MatchPolicy::KnownRva) {
    result.rva = rva; result.method = MatchMethod::KnownRva; result.matchCount = 1;
    result.observedBytes = known;
    return result;
  }
  for (const auto& section : sections_) {
    if (!(section.characteristics & IMAGE_SCN_MEM_EXECUTE) || section.fileBackedSize() < length) continue;
    constexpr std::size_t chunkBudget = 64 * 1024;
    std::vector<unsigned char> code(chunkBudget + endpoint.signature.size());
    for (std::size_t begin = 0; begin < section.fileBackedSize(); begin += chunkBudget) {
      const std::size_t remaining = section.fileBackedSize() - begin;
      const std::size_t chunk = (std::min)(remaining, chunkBudget);
      const std::size_t readLength = (std::min)(remaining, chunk + length - 1);
      if (!readRva(static_cast<std::uint32_t>(section.rva + begin), code.data(), readLength, true)) {
        result.error = "unreadable executable section"; return result;
      }
      for (std::size_t offset = 0; offset < chunk && offset + length <= readLength; ++offset) {
        if (!matches(code.data() + offset)) continue;
        result.rva = static_cast<std::uint32_t>(section.rva + begin + offset);
        std::copy_n(code.data() + offset, length, result.observedBytes.begin());
        if (++result.matchCount > 1) {
          result.method = MatchMethod::AmbiguousSignature;
          result.error = "signature is not unique in file-backed executable ranges";
          return result;
        }
      }
    }
  }
  if (result.matchCount == 1) { result.method = MatchMethod::UniqueSignature; }
  else result.error = "signature not found";
  return result;
}

std::uint32_t PeView::executableExport(const std::string& name, std::string* error) const {
  return exportAddress(name, 0, true, error);
}

std::uint32_t PeView::exportAddress(const std::string& name, std::uint16_t ordinal,
                                  bool executableOnly, std::string* error) const {
  const auto fail = [error](const char* reason) -> std::uint32_t {
    if (error) *error = reason;
    return 0;
  };
  if (!valid() || (name.empty() && !ordinal) || name.size() > 65535 ||
      !exportRva_ || exportSize_ < sizeof(IMAGE_EXPORT_DIRECTORY)) return fail("missing/invalid export directory");
  IMAGE_EXPORT_DIRECTORY directory{};
  constexpr DWORD exportBudget = 65536;
  if (!readRva(exportRva_, &directory, sizeof(directory)) ||
      !directory.NumberOfFunctions || directory.NumberOfFunctions > imageSize_ / sizeof(DWORD) ||
      directory.NumberOfNames > imageSize_ / sizeof(DWORD) ||
      directory.NumberOfFunctions > exportBudget || directory.NumberOfNames > exportBudget)
    return fail("invalid export counts or 65536-entry compatibility budget exceeded");
  if (!readRva(directory.AddressOfFunctions, nullptr, directory.NumberOfFunctions * sizeof(DWORD)) ||
      !readRva(directory.AddressOfNames, nullptr, directory.NumberOfNames * sizeof(DWORD)) ||
      !readRva(directory.AddressOfNameOrdinals, nullptr, directory.NumberOfNames * sizeof(WORD)))
    return fail("invalid export arrays");
  std::vector<DWORD> functions(directory.NumberOfFunctions), names(directory.NumberOfNames);
  std::vector<WORD> ordinals(directory.NumberOfNames);
  if (!readRva(directory.AddressOfFunctions, functions.data(), functions.size() * sizeof(DWORD)) ||
      !readRva(directory.AddressOfNames, names.data(), names.size() * sizeof(DWORD)) ||
      !readRva(directory.AddressOfNameOrdinals, ordinals.data(), ordinals.size() * sizeof(WORD)))
    return fail("invalid export arrays");
  std::vector<char> candidate(name.size() + 1);
  std::uint32_t result = 0;
  const auto validateTarget = [&](std::uint32_t target) {
    unsigned char byte = 0;
    return target && !(target >= exportRva_ && target - exportRva_ < exportSize_) &&
        readRva(target, &byte, 1, executableOnly);
  };
  if (name.empty()) {
    if (ordinal < directory.Base || ordinal - directory.Base >= functions.size() ||
        !validateTarget(functions[ordinal - directory.Base])) return fail("required ordinal export is invalid");
    if (error) error->clear();
    return functions[ordinal - directory.Base];
  }
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (!readRva(names[index], candidate.data(), candidate.size())) continue;
    if (std::memcmp(candidate.data(), name.c_str(), candidate.size()) != 0) continue;
    if (result) return fail("duplicate export name");
    if (ordinals[index] >= functions.size()) return fail("invalid export ordinal");
    const std::uint32_t target = functions[ordinals[index]];
    if (!target || (target >= exportRva_ && target - exportRva_ < exportSize_))
      return fail("null or forwarded export is not an authorized executable entry");
    if (!validateTarget(target)) return fail("export target does not have required section access");
    result = target;
  }
  if (!result) return fail("required export not found");
  if (error) error->clear();
  return result;
}

bool PeView::importedSymbols(std::vector<ImportedSymbol>* result, std::string* error) const {
  const auto fail = [error](const char* reason) { if (error) *error = reason; return false; };
  if (!valid() || !result) return fail("invalid import input");
  result->clear();
  if (!importRva_) return true;
  const auto readString = [this](std::uint32_t rva, std::string* text) {
    text->clear();
    for (std::uint32_t i = 0; i < 65536 && rva <= UINT32_MAX - i; ++i) {
      char ch = 0;
      if (!readRva(rva + i, &ch, 1)) return false;
      if (!ch) return !text->empty();
      text->push_back(ch);
    }
    return false;
  };
  for (std::uint32_t offset = 0; offset + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= importSize_;
       offset += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
    IMAGE_IMPORT_DESCRIPTOR descriptor{};
    if (!readRva(importRva_ + offset, &descriptor, sizeof(descriptor))) return fail("unreadable import descriptor");
    if (!descriptor.Name && !descriptor.FirstThunk && !descriptor.OriginalFirstThunk) return true;
    std::string module;
    if (!readString(descriptor.Name, &module) || !descriptor.OriginalFirstThunk)
      return fail("invalid import name or missing original thunk");
    bool terminated = false;
    for (std::uint32_t index = 0; index < 65536; ++index) {
      const std::uint64_t thunkRva = std::uint64_t(descriptor.OriginalFirstThunk) + index * sizeof(ULONGLONG);
      ULONGLONG thunk = 0;
      if (thunkRva > UINT32_MAX || !readRva(static_cast<DWORD>(thunkRva), &thunk, sizeof(thunk)))
        return fail("invalid import thunk");
      if (!thunk) { terminated = true; break; }
      ImportedSymbol symbol; symbol.module = module;
      if (IMAGE_SNAP_BY_ORDINAL64(thunk)) symbol.ordinal = static_cast<WORD>(IMAGE_ORDINAL64(thunk));
      else if (thunk > UINT32_MAX - 2 || !readString(static_cast<DWORD>(thunk) + 2, &symbol.name))
        return fail("invalid import symbol");
      result->push_back(std::move(symbol));
      if (result->size() > 65536) return fail("too many imported symbols");
    }
    if (!terminated) return fail("unterminated import thunk list");
  }
  return fail("unterminated import descriptor list");
}

}  // namespace kqpet::compatibility
