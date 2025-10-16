//
// XCSP 解析器测试主函数
// 演示如何使用现代化的 LibXml2Parser + IntermediateModel
//
#include <filesystem>
#include <iostream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include <glog/logging.h>
#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "model/xcsp_parser.h"
#include "model/intermediate_model.h"


// -----------------------------------------------------------------------------
// 解析与建模流程说明（面向新参与者）
// 1. manifest / bench 路径交给 XcspParser::LoadBenchManifest /
//    DescribeBenchPath 解析，统一产出 BenchEntry。
// 2. LibXml2Parser::Parse 负责打开 XML（XCSP2），逐段读取 domains、
//    variables、relations、constraints，并调用 ModelBuilder 写入中间结构。
// 3. ModelBuilder 聚合上述信息并最终构造 IntermediateModel：内部保存
//    Domain/Variable/Constraint/Relation 向量，以及各种索引和邻接关系。
// 4. 本示例通过 IntermediateModel 的查询接口打印 scope、元组、统计数据，
//    后续 Solver 模块也将基于该模型进行求解。
// -----------------------------------------------------------------------------

using namespace cpim::model;

ABSL_FLAG(std::string, bench_manifest, "samples/bench/BMPath.xml",
          "Path to a bench manifest (e.g. BMPath.xml).");
ABSL_FLAG(std::string, bench_path, "",
          "Direct path to a bench file or directory (overrides manifest).");
ABSL_FLAG(bool, list_only, false,
          "List discovered bench files without parsing them.");

// ============================================================================
// 测试新解析器
// ============================================================================

void TestNewParser(const BenchFileInfo& bench_file) {
  LOG(INFO) << "========================================";
  LOG(INFO) << "Testing XCSP Parser";
  LOG(INFO) << "========================================";
  LOG(INFO) << "Benchmark: " << bench_file.path;
  LOG(INFO) << "Detected format: "
             << (bench_file.xcsp_format.empty() ? "unknown"
                                                : bench_file.xcsp_format);

  // 创建解析器
  auto parser = XcspParser::Create(ParserType::kLibXml2);

  // 解析模型
  absl::Time start_time = absl::Now();
  auto model_or = parser->Parse(bench_file.path);
  absl::Duration parse_duration = absl::Now() - start_time;

  if (!model_or.ok()) {
    LOG(ERROR) << "Failed to parse model: " << model_or.status();
    return;
  }

  LOG(INFO) << absl::StrFormat("Parse completed in %s",
                               absl::FormatDuration(parse_duration));

  // IntermediateModel 此时已经持有 domains/variables/constraints/relations
  // 及其索引，供解析阶段和求解阶段共享使用。
  const auto& model = *model_or;

  // 打印基本信息
  LOG(INFO) << "\n" << model.ToString();

  // 获取统计信息
  auto stats = model.GetStatistics();
  LOG(INFO) << "\n" << stats.ToString();

  // 打印详细信息（可选，用于调试）
  // model.PrintDetailed();

  // 示例：遍历前10个变量
  LOG(INFO) << "\n========== Sample Variables ==========";
  int count = 0;
  for (const auto& var : model.variables()) {
    const auto& domain = model.GetDomain(var.domain);
    LOG(INFO) << absl::StrFormat("  %s (id=%d): domain=%s (size=%d)",
                                 var.name, var.id.value, domain.name,
                                 domain.Size());
    if (++count >= 10) {
      if (model.num_variables() > 10) {
        LOG(INFO) << absl::StrFormat("  ... (%d more variables)",
                                     model.num_variables() - 10);
      }
      break;
    }
  }

  // 示例：遍历前10个约束
  LOG(INFO) << "\n========== Sample Constraints ==========";
  count = 0;
  for (const auto& constraint : model.constraints()) {
    auto scope = constraint.GetScope();
    std::vector<std::string> scope_names;
    for (VariableId vid : scope) {
      scope_names.push_back(model.GetVariable(vid).name);
    }

    std::string type = GetConstraintTypeName(constraint.data);
    LOG(INFO) << absl::StrFormat("  [%d] %s: type=%s arity=%d",
                                 constraint.id.value, constraint.name, type,
                                 constraint.Arity());

    if (const auto* ext = std::get_if<ExtensionConstraint>(&constraint.data)) {
      const char* semantics =
          ext->semantics == ExtensionConstraint::Semantics::kSupports ? "supports"
                                                                      : "conflicts";
      LOG(INFO) << absl::StrFormat("      semantics=%s tuples=%d", semantics,
                                   ext->NumTuples());
      int tuple_idx = 0;
      constexpr int kMaxPreviewTuples = 5;
      for (const auto& tuple : ext->tuples) {
        LOG(INFO) << absl::StrFormat("        [%d] %s", tuple_idx,
                                     absl::StrJoin(tuple, ", "));
        ++tuple_idx;
        if (tuple_idx >= kMaxPreviewTuples) {
          if (ext->NumTuples() > kMaxPreviewTuples) {
            LOG(INFO) << absl::StrFormat("        ... (%d more tuples)",
                                         ext->NumTuples() - kMaxPreviewTuples);
          }
          break;
        }
      }
    }

    if (++count >= 10) {
      if (model.num_constraints() > 10) {
        LOG(INFO) << absl::StrFormat("  ... (%d more constraints)",
                                     model.num_constraints() - 10);
      }
      break;
    }
  }

  // 示例：查询拓扑关系
  if (model.num_variables() > 0) {
    VariableId first_var{0};
    auto constraints_for_var = model.GetConstraintsForVariable(first_var);
    LOG(INFO) << absl::StrFormat(
        "\nVariable %s participates in %d constraints",
        model.GetVariable(first_var).name, constraints_for_var.size());
  }

  LOG(INFO) << "\n========================================";
  LOG(INFO) << "New Parser Test Completed Successfully!";
  LOG(INFO) << "========================================\n";
}

// ============================================================================
// 主函数
// ============================================================================

int main(int argc, char* argv[]) {
  const char* program_name = argv[0];
  absl::ParseCommandLine(argc, argv);

  google::InitGoogleLogging(program_name);
  FLAGS_logtostderr = 1;
  FLAGS_colorlogtostderr = true;

  LOG(INFO) << "CPIM - XCSP Parser Test";
  LOG(INFO) << "======================\n";

  auto parser = XcspParser::Create(ParserType::kLibXml2);

  std::vector<BenchEntry> entries;
  std::string bench_path_flag = absl::GetFlag(FLAGS_bench_path);
  if (!bench_path_flag.empty()) {
    auto entry_or = parser->DescribeBenchPath(bench_path_flag);
    if (!entry_or.ok()) {
      LOG(ERROR) << "Failed to describe bench path: " << entry_or.status();
      google::ShutdownGoogleLogging();
      return 1;
    }
    entries.push_back(std::move(*entry_or));
  } else {
    std::string manifest_path = absl::GetFlag(FLAGS_bench_manifest);
    if (manifest_path.empty()) {
      manifest_path = "samples/bench/BMPath.xml";
    }
    auto manifest_or = parser->LoadBenchManifest(manifest_path);
    if (!manifest_or.ok()) {
      LOG(ERROR) << "Failed to load bench manifest: " << manifest_or.status();
      google::ShutdownGoogleLogging();
      return 1;
    }
    entries = std::move(*manifest_or);
  }

  LOG(INFO) << "Discovered " << entries.size() << " bench source(s):";
  for (const auto& entry : entries) {
    LOG(INFO) << "  Source: " << entry.original_path
              << " (resolved: " << entry.resolved_path << ")";
    LOG(INFO) << "    Type: "
              << (entry.kind == BenchPathKind::kDirectory ? "directory"
                                                          : "file");
    for (const auto& file : entry.files) {
      if (file.xcsp_format.empty()) {
        LOG(INFO) << "      - " << file.path;
      } else {
        LOG(INFO) << "      - " << file.path << " [" << file.xcsp_format
                  << "]";
      }
    }
  }

  if (entries.empty()) {
    LOG(ERROR) << "No bench sources resolved.";
    google::ShutdownGoogleLogging();
    return 1;
  }

  if (absl::GetFlag(FLAGS_list_only)) {
    google::ShutdownGoogleLogging();
    return 0;
  }

  const BenchFileInfo* target_file = nullptr;
  for (const auto& entry : entries) {
    if (!entry.files.empty()) {
      target_file = &entry.files.front();
      break;
    }
  }

  if (!target_file) {
    LOG(ERROR) << "No XCSP XML files available to parse.";
    google::ShutdownGoogleLogging();
    return 1;
  }

  TestNewParser(*target_file);

  LOG(INFO) << "Next Steps:";
  LOG(INFO) << "  - Phase 7: NetworkAdapter (CPU solver)";
  LOG(INFO) << "  - Phase 8: CModelAdapter (GPU solver)";
  LOG(INFO) << "  - Phase 9: Run actual solving with MAC/CModel\n";

  google::ShutdownGoogleLogging();
  return 0;
}
