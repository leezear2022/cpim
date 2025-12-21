#include <iostream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/flags/parse.h"
#include "glog/logging.h"

// CPU 求解器
#include "Network.h"
#include "Solver.h"

// GPU 求解器
#include "GModel.cuh"
#include "GModelSolver.h"
#include "model/gmodel_adapter.h"
#include "model/model_normalizer.h"
#include "model/xcsp_parser.h"

ABSL_FLAG(std::string, input, "", "Path to XCSP2/3 XML instance");
ABSL_FLAG(int, time_limit, 60000, "Time limit in milliseconds (default: 60s)");
ABSL_FLAG(bool, verbose, false, "Enable verbose output");
ABSL_FLAG(bool, cpu_only, false, "Run CPU solver only");
ABSL_FLAG(bool, gpu_only, false, "Run GPU solver only");

using namespace cpim;

// 比较两个解是否相同
bool CompareSolutions(const std::vector<int>& cpu_sol,
                     const std::vector<int>& gpu_sol) {
  if (cpu_sol.size() != gpu_sol.size()) {
    std::cout << "❌ 解的大小不同：CPU=" << cpu_sol.size()
              << ", GPU=" << gpu_sol.size() << std::endl;
    return false;
  }

  for (size_t i = 0; i < cpu_sol.size(); ++i) {
    if (cpu_sol[i] != gpu_sol[i]) {
      std::cout << "❌ 变量 " << i << " 的赋值不同：CPU=" << cpu_sol[i]
                << ", GPU=" << gpu_sol[i] << std::endl;
      return false;
    }
  }

  return true;
}

// 打印解
void PrintSolution(const std::vector<int>& solution, int max_vars = 20) {
  std::cout << "解: [";
  const int print_count = std::min(static_cast<int>(solution.size()), max_vars);
  for (int i = 0; i < print_count; ++i) {
    std::cout << "x" << i << "=" << solution[i];
    if (i + 1 < print_count) std::cout << ", ";
  }
  if (static_cast<int>(solution.size()) > max_vars) {
    std::cout << ", ... (共 " << solution.size() << " 个变量)";
  }
  std::cout << "]" << std::endl;
}

// 运行 CPU 求解器
SearchStatistics RunCPUSolver(const model::IntermediateModel& im_model,
                             int time_limit, bool verbose,
                             std::vector<int>& solution) {
  std::cout << "\n========================================" << std::endl;
  std::cout << "=== CPU 求解器 (MAC + AC3bit) ===" << std::endl;
  std::cout << "========================================" << std::endl;

  // 创建 Network
  Network network(im_model);

  std::cout << "变量数: " << network.vars.size() << std::endl;
  std::cout << "约束数: " << network.tabs.size() << std::endl;

  // 创建 MAC 求解器（使用 AC3bit 算法）
  MAC mac(&network, AC_3bit, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);

  // 求解
  const SearchStatistics stats = mac.enforce(time_limit);

  // 提取解（从 MAC 对象的 solution 成员变量）
  if (!mac.solution.empty()) {
    solution = mac.solution;
  }

  return stats;
}

// 运行 GPU 求解器
GpuSearchStatistics RunGPUSolver(const model::IntermediateModel& im_model,
                                int time_limit, bool verbose,
                                std::vector<int>& solution) {
  std::cout << "\n========================================" << std::endl;
  std::cout << "=== GPU 求解器 (GModel + GAC) ===" << std::endl;
  std::cout << "========================================" << std::endl;

  std::cout << "变量数: " << im_model.num_variables() << std::endl;
  std::cout << "约束数: " << im_model.num_constraints() << std::endl;

  // 构建 GModel
  model::GModelOptions options;
  options.device_id = 0;
  options.enable_prefetch = false;  // Jetson 优化
  options.skip_non_binary = true;

  GModel gmodel = model::GModelAdapter::Build(im_model, options);

  // 创建 GModelSolver
  GModelSolver solver(&gmodel, verbose);

  // 求解
  const GpuSearchStatistics stats = solver.Solve(time_limit);

  // 提取解
  solution = solver.GetSolution();

  return stats;
}

int main(int argc, char** argv) {
  google::InitGoogleLogging(argv[0]);
  absl::ParseCommandLine(argc, argv);

  const std::string input = absl::GetFlag(FLAGS_input);
  const int time_limit = absl::GetFlag(FLAGS_time_limit);
  const bool verbose = absl::GetFlag(FLAGS_verbose);
  const bool cpu_only = absl::GetFlag(FLAGS_cpu_only);
  const bool gpu_only = absl::GetFlag(FLAGS_gpu_only);

  if (input.empty()) {
    std::cerr << "用法: " << argv[0] << " --input=/path/to/instance.xml"
              << std::endl;
    std::cerr << "选项:" << std::endl;
    std::cerr << "  --time_limit=N    时间限制（毫秒，默认 60000）" << std::endl;
    std::cerr << "  --verbose         打印详细信息" << std::endl;
    std::cerr << "  --cpu_only        只运行 CPU 求解器" << std::endl;
    std::cerr << "  --gpu_only        只运行 GPU 求解器" << std::endl;
    return 1;
  }

  std::cout << "========================================" << std::endl;
  std::cout << "=== CPU vs GPU 求解器对比测试 ===" << std::endl;
  std::cout << "========================================" << std::endl;
  std::cout << "问题实例: " << input << std::endl;
  std::cout << "时间限制: " << time_limit << " ms" << std::endl;
  std::cout << std::endl;

  try {
    // 解析 XCSP 文件（只解析一次）
    std::cout << "解析 XCSP 文件..." << std::endl;
    auto parser = model::XcspParser::Create(model::ParserType::kLibXml2);
    if (!parser) {
      std::cerr << "❌ 创建 XCSP 解析器失败" << std::endl;
      return 1;
    }

    auto model_or = parser->Parse(input);
    if (!model_or.ok()) {
      std::cerr << "❌ 解析 XML 失败: " << model_or.status() << std::endl;
      return 1;
    }

    // 归一化模型
    model::ModelNormalizer normalizer;
    auto normalized_or = normalizer.Normalize(*model_or);
    if (!normalized_or.ok()) {
      std::cerr << "❌ 归一化模型失败: " << normalized_or.status() << std::endl;
      return 1;
    }

    model::IntermediateModel normalized = std::move(*normalized_or);
    std::cout << "✓ 解析完成" << std::endl;

    std::vector<int> cpu_solution;
    std::vector<int> gpu_solution;
    SearchStatistics cpu_stats;
    GpuSearchStatistics gpu_stats;

    // 运行 CPU 求解器
    if (!gpu_only) {
      cpu_stats = RunCPUSolver(normalized, time_limit, verbose, cpu_solution);

      std::cout << "\n--- CPU 求解器统计 ---" << std::endl;
      std::cout << "求解时间: " << cpu_stats.solve_time << " ms" << std::endl;
      std::cout << "正向节点: " << cpu_stats.num_positive << std::endl;
      std::cout << "回溯节点: " << cpu_stats.num_negative << std::endl;
      std::cout << "找到解数: " << cpu_stats.num_sol << std::endl;
      std::cout << "超时: " << (cpu_stats.time_out ? "是" : "否") << std::endl;

      if (!cpu_solution.empty()) {
        PrintSolution(cpu_solution);
      } else {
        std::cout << "未找到解" << std::endl;
      }
    }

    // 运行 GPU 求解器
    if (!cpu_only) {
      gpu_stats = RunGPUSolver(normalized, time_limit, verbose, gpu_solution);

      std::cout << "\n--- GPU 求解器统计 ---" << std::endl;
      std::cout << "求解时间: " << gpu_stats.solve_time << " s" << std::endl;
      std::cout << "正向节点: " << gpu_stats.num_positive << std::endl;
      std::cout << "回溯节点: " << gpu_stats.num_negative << std::endl;
      std::cout << "GAC 迭代: " << gpu_stats.gac_iterations << std::endl;
      std::cout << "GAC 删除: " << gpu_stats.gac_deletions << std::endl;
      std::cout << "找到解数: " << gpu_stats.num_solutions << std::endl;
      std::cout << "超时: " << (gpu_stats.time_out ? "是" : "否") << std::endl;

      if (!gpu_solution.empty()) {
        PrintSolution(gpu_solution);
      } else {
        std::cout << "未找到解" << std::endl;
      }
    }

    // 对比结果
    if (!cpu_only && !gpu_only) {
      std::cout << "\n========================================" << std::endl;
      std::cout << "=== 对比结果 ===" << std::endl;
      std::cout << "========================================" << std::endl;

      if (cpu_solution.empty() && gpu_solution.empty()) {
        std::cout << "✓ 两个求解器都未找到解（一致）" << std::endl;
      } else if (cpu_solution.empty() || gpu_solution.empty()) {
        std::cout << "❌ 一个求解器找到解，另一个未找到（不一致）" << std::endl;
        std::cout << "  CPU 找到解: " << (cpu_solution.empty() ? "否" : "是") << std::endl;
        std::cout << "  GPU 找到解: " << (gpu_solution.empty() ? "否" : "是") << std::endl;
      } else {
        if (CompareSolutions(cpu_solution, gpu_solution)) {
          std::cout << "✓ 解完全一致！" << std::endl;
        } else {
          std::cout << "❌ 解不一致！" << std::endl;
          return 1;
        }
      }

      // 性能对比
      if (cpu_stats.solve_time > 0 && gpu_stats.solve_time > 0) {
        const double speedup = cpu_stats.solve_time / (gpu_stats.solve_time * 1000.0);
        std::cout << "\n性能对比:" << std::endl;
        std::cout << "  CPU 时间: " << cpu_stats.solve_time << " ms" << std::endl;
        std::cout << "  GPU 时间: " << (gpu_stats.solve_time * 1000.0) << " ms" << std::endl;
        std::cout << "  加速比: " << speedup << "x" << std::endl;
      }
    }

  } catch (const std::exception& e) {
    std::cerr << "\n❌ 错误: " << e.what() << std::endl;
    return 1;
  }

  std::cout << "\n========================================" << std::endl;
  std::cout << "=== 测试完成 ===" << std::endl;
  std::cout << "========================================" << std::endl;

  return 0;
}
