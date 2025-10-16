#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "absl/status/statusor.h"

namespace cpim::model {

class IntermediateModel;

enum class ParserType {
  kLibXml2,
  kPugiXml,
  kTinyXml2,
};

enum class BenchPathKind { kFile, kDirectory };

struct BenchFileInfo {
  std::filesystem::path path;
  std::string xcsp_format;
};

struct BenchEntry {
  std::filesystem::path original_path;
  std::filesystem::path resolved_path;
  BenchPathKind kind;
  std::vector<BenchFileInfo> files;
};

class XcspParser {
 public:
  virtual ~XcspParser() = default;

  virtual absl::StatusOr<IntermediateModel> Parse(
      std::filesystem::path path) = 0;

  virtual absl::StatusOr<BenchEntry> DescribeBenchPath(
      std::filesystem::path path) = 0;

  virtual absl::StatusOr<std::vector<BenchEntry>> LoadBenchManifest(
      std::filesystem::path manifest_path) = 0;

  static std::unique_ptr<XcspParser> Create(
      ParserType type = ParserType::kLibXml2);

 protected:
  XcspParser() = default;

  XcspParser(const XcspParser&) = delete;
  XcspParser& operator=(const XcspParser&) = delete;
  XcspParser(XcspParser&&) = delete;
  XcspParser& operator=(XcspParser&&) = delete;
};

}  // namespace cpim::model
