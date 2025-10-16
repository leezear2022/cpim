//
// XCSP3 新解析器测试主函数
// 演示如何使用现代化的 LibXml2Parser + IntermediateModel
//
#include <filesystem>
#include <iostream>

#include <glog/logging.h>
#include "absl/strings/str_format.h"
#include "absl/time/clock.h"
#include "absl/time/time.h"

#include "model/xcsp_parser.h"
#include "model/intermediate_model.h"

using namespace cpim::model;

// ============================================================================
// 测试新解析器
// ============================================================================

void TestNewParser(const std::filesystem::path& benchmark_path) {
  LOG(INFO) << "========================================";
  LOG(INFO) << "Testing New XCSP3 Parser";
  LOG(INFO) << "========================================";
  LOG(INFO) << "Benchmark: " << benchmark_path;

  // 创建解析器
  auto parser = XcspParser::Create(ParserType::kLibXml2);

  // 解析模型
  absl::Time start_time = absl::Now();
  auto model_or = parser->Parse(benchmark_path);
  absl::Duration parse_duration = absl::Now() - start_time;

  if (!model_or.ok()) {
    LOG(ERROR) << "Failed to parse model: " << model_or.status();
    return;
  }

  LOG(INFO) << absl::StrFormat("Parse completed in %s",
                               absl::FormatDuration(parse_duration));

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
  // 初始化 glog
  google::InitGoogleLogging(argv[0]);
  FLAGS_logtostderr = 1;
  FLAGS_colorlogtostderr = true;

  LOG(INFO) << "CPIM - New XCSP3 Parser Test";
  LOG(INFO) << "============================\n";

  // 测试文件路径
  std::filesystem::path benchmark_path;
  if (argc > 1) {
    benchmark_path = argv[1];
  } else {
    benchmark_path = "../samples/bench/BMPath.xml";
  }

  // 检查文件是否存在
  if (!std::filesystem::exists(benchmark_path)) {
    LOG(ERROR) << "Benchmark file not found: " << benchmark_path;
    LOG(ERROR) << "Usage: " << argv[0] << " [path_to_xcsp3_file]";
    return 1;
  }

  // 测试新解析器
  TestNewParser(benchmark_path);

  // TODO: Phase 7 & 8 - 后续实现求解器适配
  LOG(INFO) << "Next Steps:";
  LOG(INFO) << "  - Phase 7: NetworkAdapter (CPU solver)";
  LOG(INFO) << "  - Phase 8: CModelAdapter (GPU solver)";
  LOG(INFO) << "  - Phase 9: Run actual solving with MAC/CModel\n";

  google::ShutdownGoogleLogging();
  return 0;
}
