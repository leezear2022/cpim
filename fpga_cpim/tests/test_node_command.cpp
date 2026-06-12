#include "test_util.hpp"

#include "fpga_cpim/golden.hpp"
#include "fpga_cpim/node_command.hpp"

using namespace fpga_cpim;

namespace {

std::vector<std::pair<Value, Value>> EqPairs(uint32_t domain) {
  std::vector<std::pair<Value, Value>> pairs;
  for (Value value = 0; value < domain; ++value) {
    pairs.push_back({value, value});
  }
  return pairs;
}

std::vector<std::pair<Value, Value>> NeqPairs(uint32_t domain) {
  std::vector<std::pair<Value, Value>> pairs;
  for (Value x = 0; x < domain; ++x) {
    for (Value y = 0; y < domain; ++y) {
      if (x != y) {
        pairs.push_back({x, y});
      }
    }
  }
  return pairs;
}

Model MakeAcOkSacDwoModel() {
  Model model = MakeEmptyModel({2, 2, 2});
  AddBinaryConstraint(&model, 0, 1, EqPairs(2));
  AddBinaryConstraint(&model, 1, 2, EqPairs(2));
  AddBinaryConstraint(&model, 0, 2, NeqPairs(2));
  return model;
}

}  // namespace

int main() {
  {
    Model model = MakeAcOkSacDwoModel();
    NodeCommand command;
    command.mode = NodeCommand::kRunAC;
    NodeResult result = RunNodeCommand(model, command);
    GoldenResult golden = EnforceAC_Golden(
        model, fpga_cpim_test::FullDomains(model),
        fpga_cpim_test::AllConstraints(model));

    CHECK_TRUE(result.status == NodeResult::kAliveComplete);
    CHECK_TRUE(result.ac_complete);
    CHECK_TRUE(result.nsacq_complete);
    CHECK_EQ(result.final_domains.size(), golden.domains.size());
    for (size_t i = 0; i < result.final_domains.size(); ++i) {
      CHECK_TRUE(result.final_domains[i] == golden.domains[i]);
    }
  }

  {
    Model model = MakeLessThanModel(4);
    NodeCommand command;
    command.mode = NodeCommand::kRunAC;
    command.has_assignment = true;
    command.branch_var = 0;
    command.branch_value = 3;
    command.seed_vars = {0};
    NodeResult result = RunNodeCommand(model, command);
    CHECK_TRUE(result.status == NodeResult::kDWO);
    CHECK_TRUE(result.confirmed_deletions.empty());
  }

  {
    Model model = MakeAcOkSacDwoModel();
    NodeCommand command;
    command.mode = NodeCommand::kRunACThenNSACQ;
    command.nsacq_init = NodeCommand::kAllVars;
    NodeResult result = RunNodeCommand(model, command);
    CHECK_TRUE(result.status == NodeResult::kDWO);
    CHECK_TRUE(result.has_confirmed_deletions);
    CHECK_FALSE(result.confirmed_deletions.empty());

    const DeletionMask& deletion = result.confirmed_deletions.front();
    Value value = 0;
    while (value < deletion.mask.NBits() && !deletion.mask.Test(value)) {
      ++value;
    }
    GoldenResult confirm = RunSingletonProbe_Golden(
        model, fpga_cpim_test::FullDomains(model), deletion.var, value);
    CHECK_TRUE(confirm.status == PropStatus::kDWO);
  }

  {
    Model model = MakeAcOkSacDwoModel();
    NodeCommand command;
    command.mode = NodeCommand::kRunACThenNSACQ;
    command.nsacq_init = NodeCommand::kAllVars;
    command.budget.max_events = 100000;
    command.budget.max_probe_events = 0;
    NodeResult result = RunNodeCommand(model, command);
    CHECK_TRUE(result.status == NodeResult::kAliveIncomplete);
    CHECK_TRUE(result.ac_complete);
    CHECK_FALSE(result.nsacq_complete);
    CHECK_TRUE(result.confirmed_deletions.empty());
    CHECK_TRUE(result.unknown_probe_count > 0);
    CHECK_TRUE((result.incomplete_reason_mask & kProbeBudget) != 0);
  }

  {
    Model model = MakeAcOkSacDwoModel();
    NodeCommand command;
    command.mode = NodeCommand::kRunBranchProbes;
    NodeResult result = RunNodeCommand(model, command);
    CHECK_TRUE(result.status == NodeResult::kAliveIncomplete);
    CHECK_TRUE((result.incomplete_reason_mask & kDeadlockGuard) != 0);
  }

  return 0;
}
