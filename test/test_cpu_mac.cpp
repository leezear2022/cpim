#include <iostream>
#include "Solver.h"
#include "xcsp3model/HModel.h"
#include "xcsp3model/XBuilder.h"

using namespace cpim;
using namespace cpim::common;

// 自定义 MAC 类，添加详细日志
class VerboseMAC : public MAC {
 public:
  VerboseMAC(Network* n, const ACAlgorithm ac_algzm,
             const Heuristic::Var varh, const Heuristic::Val valh)
      : MAC(n, ac_algzm, varh, valh) {}

  SearchStatistics enforce_verbose(const int time_limits) {
    Timer t;
    x_evt_.clear();

    std::cout << "[CPU MAC] Initial GAC..." << std::endl;
    auto init_cs = ac_->enforce(n_->vars, 0);
    consistent_ = init_cs.state;
    std::cout << "[CPU MAC] Initial GAC: deletions=" << init_cs.num_delete
              << ", consistent=" << (consistent_ ? "true" : "false") << std::endl;

    if (!consistent_) {
      statistics_.solve_time = t.elapsed();
      return statistics_;
    }

    std::cout << "[CPU MAC] Starting search from level 0..." << std::endl;
    int search_level = 0;

    while (!finished_) {
      if (t.elapsed() > time_limits) {
        statistics_.solve_time = t.elapsed();
        statistics_.time_out = true;
        return statistics_;
      }

      IntVal v_a = select_v_value(I.size());

      std::cout << "[Try] Level " << search_level << ": var[" << v_a.v()->id()
                << "] = " << v_a.a() << std::endl;

      n_->NewLevel(I.size());
      I.push(v_a);
      ++statistics_.num_positive;
      v_a.v()->ReduceTo(v_a.a(), I.size());
      x_evt_.push_back(v_a.v());

      auto cs = ac_->enforce(x_evt_, I.size());
      consistent_ = cs.state;

      std::cout << "  [GAC] deletions=" << cs.num_delete
                << ", inconsistent=" << (!consistent_ ? "true" : "false");

      x_evt_.clear();

      if (consistent_ && I.full()) {
        std::cout << " → solution found!" << std::endl;
        finished_ = true;
        statistics_.solve_time = t.elapsed();
        get_solution();
        return statistics_;
      } else if (consistent_) {
        std::cout << " → continue search" << std::endl;
        ++search_level;
      } else {
        std::cout << " → prune" << std::endl;
      }

      while (!consistent_ && !I.empty()) {
        v_a = I.pop();
        --search_level;
        std::cout << "[Backtrack] Level " << search_level << ": var["
                  << v_a.v()->id() << "] = " << v_a.a() << std::endl;

        n_->BackTo(I.size());
        v_a.v()->RemoveValue(v_a.a(), I.size());
        ++statistics_.num_negative;
        x_evt_.push_back(v_a.v());
        consistent_ = v_a.v()->size(I.size()) && ac_->enforce(x_evt_, I.size()).state;
        x_evt_.clear();
        I.update_model_assigned();
      }

      if (!consistent_) {
        finished_ = true;
        std::cout << "[CPU MAC] No solution found!" << std::endl;
      }
    }

    statistics_.solve_time = t.elapsed();
    return statistics_;
  }
};

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <xcsp_file>" << std::endl;
    return 1;
  }

  const std::string X_PATH = argv[1];

  std::cout << "[CPU MAC] Parsing: " << X_PATH << std::endl;
  const XBuilder builder(X_PATH, XRT_BM_PATH);
  const HModel hm = HModelNode::Make();
  builder.GenerateHModel(hm);

  auto* n = new Network(hm);

  std::cout << "[CPU MAC] Variables: " << n->vars.size() << std::endl;
  std::cout << "[CPU MAC] Constraints: " << n->tabs.size() << std::endl;

  VerboseMAC mac(n, AC_3bit, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);

  constexpr long TimeLimit = 900000;
  const SearchStatistics statistics = mac.enforce_verbose(TimeLimit);

  if (statistics.num_positive > 0) {
    std::cout << "\n=== Solution Found! ===" << std::endl;
    std::cout << mac.sol_str << std::endl;
  }

  std::cout << "\n=== Statistics ===" << std::endl;
  std::cout << "Time: " << statistics.solve_time << " ms" << std::endl;
  std::cout << "Positive (assignments): " << statistics.num_positive << std::endl;
  std::cout << "Negative (backtracks): " << statistics.num_negative << std::endl;

  delete n;
  return 0;
}
