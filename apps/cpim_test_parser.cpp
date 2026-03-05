//
// XCSP 解析器测试主函数
// 演示如何使用现代化的 LibXml2Parser + IntermediateModel
//
#include <filesystem>
#include <iostream>
#include <memory>
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

#include "Solver.h"
#include "model/intermediate_model.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"


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
ABSL_FLAG(std::string, ac_algorithm, "AC3bit",
          "Arc consistency algorithm: AC3, AC3bit, RPC3, lMaxRPC, NSAC, MSAC3bit");
ABSL_FLAG(int32_t, msac_max_probes, -1,
          "Maximum number of SAC probes (-1 = unlimited)");
ABSL_FLAG(int32_t, msac_max_time_ms, -1,
          "Maximum SAC probe time in milliseconds (-1 = unlimited)");
ABSL_FLAG(int32_t, msac_depth_limit, -1,
          "Maximum search depth for SAC (-1 = all levels)");
ABSL_FLAG(std::string, msac_mode, "SAC1",
          "SAC mode: SAC1 (full scan) or SAC3 (incremental queue)");
ABSL_FLAG(bool, msac_verbose_stats, false,
          "Output verbose MSAC statistics after search");

// ============================================================================
// AC 算法选择辅助函数
// ============================================================================

cpim::ACAlgorithm ParseACAlgorithm(const std::string& name) {
  if (name == "AC3") return cpim::AC_3;
  if (name == "AC3bit") return cpim::AC_3bit;
  if (name == "RPC3") return cpim::CA_RPC3;
  if (name == "lMaxRPC") return cpim::CA_LMRPC_BIT;
  if (name == "NSAC") return cpim::A_NSAC;
  if (name == "MSAC3bit") return cpim::A_MSAC3bit;  // Phase 1: MSAC with AC3bit kernel
  if (name == "SAC1" || name == "SAC3") {
    LOG(WARNING) << "SAC1/SAC3 not yet integrated via ACAlgorithm enum, falling back to AC3bit";
    return cpim::AC_3bit;
  }

  LOG(WARNING) << "Unknown AC algorithm: " << name << ", falling back to AC3bit";
  return cpim::AC_3bit;
}

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

  // 对解析结果执行模型归一化：域索引从 0 开始，约束语义统一为 supports。
  ModelNormalizer normalizer;
  auto normalized_or = normalizer.Normalize(*model_or);
  if (!normalized_or.ok()) {
    LOG(ERROR) << "Normalization failed: " << normalized_or.status();
    return;
  }
  const auto& model = *normalized_or;

  // 展示归一化后的域映射，帮助理解取值如何被重标记。
  for (const DomainRemap& remap : normalizer.domain_remaps()) {
    const auto& normalized_domain = model.GetDomain(remap.normalized_id);
    LOG(INFO) << absl::StrFormat(
        "Domain %s -> [0, %d): %s", normalized_domain.name,
        static_cast<int>(remap.canonical_to_original.size()),
        absl::StrJoin(remap.canonical_to_original, ", "));
  }

  // 中间模型现已归一化，可直接供后续求解或调试。

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

  // 将中间模型直接接入旧版 CPU 求解器并运行 MAC+AC。
  LOG(INFO) << "\n========== CPU MAC Solver ==========";
  cpim::Network network(model);

  // 调试信息：使用 VLOG(1) 控制，通过 --v=1 启用
  VLOG(1) << "Network: tabs=" << network.tabs.size()
          << " max_dom=" << network.max_domain_size()
          << " max_arity=" << network.max_arity();

  // 解析 AC 算法参数
  cpim::ACAlgorithm ac_alg = ParseACAlgorithm(absl::GetFlag(FLAGS_ac_algorithm));
  LOG(INFO) << "Using AC algorithm: " << absl::GetFlag(FLAGS_ac_algorithm);

  cpim::MAC mac(&network, ac_alg, cpim::Heuristic::VRH_DOM_MIN,
                cpim::Heuristic::VLH_MIN);

  // Phase 1: 配置 MSAC3bit 参数（如果使用了 MSAC）
  if (ac_alg == cpim::A_MSAC3bit) {
    // 解析 SAC 模式
    std::string mode_str = absl::GetFlag(FLAGS_msac_mode);
    cpim::MSACConfig::Mode mode = cpim::MSACConfig::SAC1;  // 默认 SAC1
    if (mode_str == "SAC3") {
      mode = cpim::MSACConfig::SAC3;
    } else if (mode_str == "SAC_SDS") {
      mode = cpim::MSACConfig::SAC_SDS;
    }

    cpim::MSACConfig msac_config(
        absl::GetFlag(FLAGS_msac_max_probes),
        absl::GetFlag(FLAGS_msac_max_time_ms),
        absl::GetFlag(FLAGS_msac_depth_limit),
        mode);

    mac.ConfigureMSAC(msac_config);

    LOG(INFO) << "MSAC3bit configuration:";
    LOG(INFO) << "  mode: " << mode_str;
    LOG(INFO) << "  max_probes: " << msac_config.max_probes;
    LOG(INFO) << "  max_time_ms: " << msac_config.max_time_ms;
    LOG(INFO) << "  depth_limit: " << msac_config.depth_limit;
  }

  constexpr int kCpuSolverTimeLimitMs = 900000;
  cpim::SearchStatistics solve_stats = mac.enforce(kCpuSolverTimeLimitMs);
  // NOTE: MAC::enforce() already calls get_solution() internally when a solution is found.
  // Do NOT call get_solution() unconditionally here, as it would fill solution vector
  // even when no solution exists (UNSAT case).

  // 验证解是否满足所有约束（仅在 VLOG(1) 或更高级别时输出详情）
  if (!mac.solution.empty() && VLOG_IS_ON(1)) {
    VLOG(1) << "Verifying solution against all constraints...";
    int violations = 0;
    for (auto* tab : network.tabs) {
      std::vector<int> tuple = {mac.solution[tab->scope[0]->id()],
                                 mac.solution[tab->scope[1]->id()]};
      if (!tab->sat(tuple)) {
        LOG(ERROR) << "VIOLATION: constraint " << tab->id()
                   << " (V" << tab->scope[0]->id() << ", V" << tab->scope[1]->id() << ")"
                   << " tuple=(" << tuple[0] << ", " << tuple[1] << ")";
        ++violations;
      }
    }
    if (violations == 0) {
      VLOG(1) << "All constraints satisfied!";
    } else {
      LOG(ERROR) << "Found " << violations << " constraint violations!";
    }
  }

  std::vector<const DomainRemap*> remap_lookup(model.num_domains(), nullptr);
  for (const DomainRemap& remap : normalizer.domain_remaps()) {
    if (remap.normalized_id.IsValid() &&
        remap.normalized_id.value < static_cast<int>(remap_lookup.size())) {
      remap_lookup[remap.normalized_id.value] = &remap;
    }
  }

  if (!mac.solution.empty()) {
    std::vector<int> original_solution;
    original_solution.reserve(mac.solution.size());
    for (size_t idx = 0; idx < mac.solution.size(); ++idx) {
      const auto& var = model.GetVariable(VariableId{static_cast<int>(idx)});
      const DomainRemap* remap = remap_lookup[var.domain.value];
      int canonical_value = mac.solution[idx];
      int original_value = canonical_value;
      if (remap && canonical_value >= 0 &&
          canonical_value < static_cast<int>(remap->canonical_to_original.size())) {
        original_value = remap->canonical_to_original[canonical_value];
      }
      original_solution.push_back(original_value);
    }
    LOG(INFO) << "MAC solution (canonical indices): " << mac.sol_str;
    LOG(INFO) << "MAC solution (original values): "
              << absl::StrJoin(original_solution, " ");
  } else if (solve_stats.time_out) {
    LOG(WARNING) << "MAC search timed out before finding a solution.";
  } else {
    LOG(WARNING) << "MAC search did not find a solution.";
  }

  LOG(INFO) << absl::StrFormat(
      "MAC stats: time=%d ms, positives=%d, negatives=%d, nodes=%d, timeout=%s",
      static_cast<int>(solve_stats.solve_time), solve_stats.num_positive,
      solve_stats.num_negative, solve_stats.nodes,
      solve_stats.time_out ? "true" : "false");

  // Phase 1: 输出 MSAC 统计信息
  if (absl::GetFlag(FLAGS_msac_verbose_stats)) {
    const auto* msac_stats = mac.GetMSACStats();
    if (msac_stats) {
      msac_stats->Print();
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
