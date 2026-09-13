#pragma once

#include "profile.h"

namespace kqpet::compatibility {

enum class ImageLayout { Raw, Mapped };
enum class MatchMethod { None, KnownRva, UniqueSignature, AmbiguousSignature, Conflict };

struct EndpointMatch {
  std::uint32_t rva = 0;
  MatchMethod method = MatchMethod::None;
  std::size_t matchCount = 0;
  std::string error;
  std::array<unsigned char, 32> observedBytes{};
  bool matched() const {
    return rva != 0 && (method == MatchMethod::KnownRva || method == MatchMethod::UniqueSignature);
  }
};

struct PeSection {
  std::uint32_t rva = 0;
  std::uint32_t virtualSize = 0;
  std::uint32_t rawOffset = 0;
  std::uint32_t rawSize = 0;
  std::uint32_t characteristics = 0;
  std::size_t fileBackedSize() const;
};

struct ImportedSymbol {
  std::string module;
  std::string name;
  std::uint16_t ordinal = 0;
};

class PeView {
public:
  PeView(const void* bytes, std::size_t size, ImageLayout layout);
  bool valid() const { return error_.empty(); }
  const std::string& error() const { return error_; }
  std::uint32_t imageSize() const { return imageSize_; }
  std::uint32_t headerSize() const { return headerSize_; }
  std::uint32_t timestamp() const { return timestamp_; }
  std::uint16_t fileCharacteristics() const { return fileCharacteristics_; }
  const std::vector<PeSection>& sections() const { return sections_; }
  bool readRva(std::uint32_t rva, void* output, std::size_t size,
               bool executableOnly = false) const;
  EndpointMatch resolve(const EndpointProfile& endpoint) const;
  std::uint32_t executableExport(const std::string& name, std::string* error = nullptr) const;
  std::uint32_t exportAddress(const std::string& name, std::uint16_t ordinal = 0,
                              bool executableOnly = false, std::string* error = nullptr) const;
  bool importedSymbols(std::vector<ImportedSymbol>* result, std::string* error = nullptr) const;

private:
  bool readOffset(std::size_t offset, void* output, std::size_t size) const;
  bool parse();
  const unsigned char* bytes_ = nullptr;
  std::size_t size_ = 0;
  ImageLayout layout_;
  std::uint32_t imageSize_ = 0;
  std::uint32_t headerSize_ = 0;
  std::uint32_t timestamp_ = 0;
  std::uint16_t fileCharacteristics_ = 0;
  std::uint32_t exportRva_ = 0;
  std::uint32_t exportSize_ = 0;
  std::uint32_t importRva_ = 0;
  std::uint32_t importSize_ = 0;
  std::vector<PeSection> sections_;
  std::string error_;
};

class RawPeView final : public PeView {
public:
  RawPeView(const void* bytes, std::size_t size) : PeView(bytes, size, ImageLayout::Raw) {}
};
class MappedPeView final : public PeView {
public:
  MappedPeView(const void* bytes, std::size_t size) : PeView(bytes, size, ImageLayout::Mapped) {}
};

}  // namespace kqpet::compatibility
