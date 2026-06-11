#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/engine.hpp"
#include "fpga_cpim/golden.hpp"
#include "fpga_cpim/model.hpp"
#include "fpga_cpim/partition.hpp"
#include "fpga_cpim/sim_stats.hpp"

namespace fpga_cpim {
namespace {

struct CliOptions {
  SyntheticConfig synthetic;
  std::string instance = "synthetic";
  std::string mode = "nsacq";
  uint32_t worlds = 4;
  uint64_t max_events = 100000;
  uint64_t max_revise = 100000;
  uint64_t max_epochs = 1000;
  uint64_t max_support_words = 1000000;
  uint32_t nsac_radius = 1;
  PartitionConfig partition;
  SupportOracleConfig support_oracle;
  std::string json_path;
};

std::string GetArgValue(int argc, char** argv, int* i) {
  std::string arg = argv[*i];
  const size_t eq = arg.find('=');
  if (eq != std::string::npos) {
    return arg.substr(eq + 1);
  }
  if (*i + 1 >= argc) {
    return "";
  }
  ++(*i);
  return argv[*i];
}

PartitionPolicy ParsePartitionPolicyName(const std::string& name) {
  if (name == "degree") {
    return PartitionPolicy::kDegree;
  }
  if (name == "contiguous") {
    return PartitionPolicy::kContiguous;
  }
  throw std::invalid_argument("unknown partition policy: " + name);
}

CliOptions ParseArgs(int argc, char** argv) {
  CliOptions opt;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--instance" || arg.rfind("--instance=", 0) == 0) {
      opt.instance = GetArgValue(argc, argv, &i);
    } else if (arg == "--vars" || arg.rfind("--vars=", 0) == 0) {
      opt.synthetic.vars = static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--domain" || arg.rfind("--domain=", 0) == 0) {
      opt.synthetic.domain = static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--density" || arg.rfind("--density=", 0) == 0) {
      opt.synthetic.density = std::stod(GetArgValue(argc, argv, &i));
    } else if (arg == "--tightness" || arg.rfind("--tightness=", 0) == 0) {
      opt.synthetic.tightness = std::stod(GetArgValue(argc, argv, &i));
    } else if (arg == "--degree" || arg.rfind("--degree=", 0) == 0) {
      opt.synthetic.degree = static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--seed" || arg.rfind("--seed=", 0) == 0) {
      opt.synthetic.seed = static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--graph" || arg.rfind("--graph=", 0) == 0) {
      opt.synthetic.graph = GetArgValue(argc, argv, &i);
    } else if (arg == "--relation" || arg.rfind("--relation=", 0) == 0) {
      opt.synthetic.relation = GetArgValue(argc, argv, &i);
    } else if (arg == "--mode" || arg.rfind("--mode=", 0) == 0) {
      opt.mode = GetArgValue(argc, argv, &i);
    } else if (arg == "--worlds" || arg.rfind("--worlds=", 0) == 0) {
      opt.worlds = static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--max-events" || arg.rfind("--max-events=", 0) == 0) {
      opt.max_events = std::stoull(GetArgValue(argc, argv, &i));
    } else if (arg == "--max-revise" || arg.rfind("--max-revise=", 0) == 0) {
      opt.max_revise = std::stoull(GetArgValue(argc, argv, &i));
    } else if (arg == "--max-epochs" || arg.rfind("--max-epochs=", 0) == 0) {
      opt.max_epochs = std::stoull(GetArgValue(argc, argv, &i));
    } else if (arg == "--max-support-words" ||
               arg.rfind("--max-support-words=", 0) == 0) {
      opt.max_support_words = std::stoull(GetArgValue(argc, argv, &i));
    } else if (arg == "--support-banks" ||
               arg.rfind("--support-banks=", 0) == 0) {
      opt.support_oracle.num_banks =
          static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--support-base-latency" ||
               arg.rfind("--support-base-latency=", 0) == 0) {
      opt.support_oracle.base_latency_cycles =
          static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--support-conflict-penalty" ||
               arg.rfind("--support-conflict-penalty=", 0) == 0) {
      opt.support_oracle.conflict_penalty_cycles =
          static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--nsac-radius" || arg.rfind("--nsac-radius=", 0) == 0) {
      opt.nsac_radius = static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--partitions" || arg.rfind("--partitions=", 0) == 0) {
      opt.partition.num_partitions =
          static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--partition-policy" ||
               arg.rfind("--partition-policy=", 0) == 0) {
      opt.partition.policy = ParsePartitionPolicyName(GetArgValue(argc, argv, &i));
    } else if (arg == "--max-vars-per-partition" ||
               arg.rfind("--max-vars-per-partition=", 0) == 0) {
      opt.partition.max_vars_per_partition =
          static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--max-constraints-per-partition" ||
               arg.rfind("--max-constraints-per-partition=", 0) == 0) {
      opt.partition.max_constraints_per_partition =
          static_cast<uint32_t>(std::stoul(GetArgValue(argc, argv, &i)));
    } else if (arg == "--json" || arg.rfind("--json=", 0) == 0) {
      opt.json_path = GetArgValue(argc, argv, &i);
    } else if (arg == "--help") {
      std::cout
          << "fpga_cpim_sim --instance synthetic --vars 32 --domain 32 "
          << "--density 0.2 --mode ac|sacq|qsac|nsacq --worlds 4 "
          << "--partitions 4 --partition-policy degree "
          << "--support-banks 4 --json out.json\n";
      std::exit(0);
    }
  }
  if (opt.support_oracle.num_banks == 0) {
    opt.support_oracle.num_banks = 1;
  }
  if (opt.instance.rfind("synthetic:", 0) == 0) {
    opt.synthetic.graph = opt.instance.substr(std::string("synthetic:").size());
    opt.instance = "synthetic";
  }
  return opt;
}

std::vector<DomainMask> FullDomains(const Model& model) {
  std::vector<DomainMask> domains;
  domains.reserve(model.num_vars);
  for (uint32_t size : model.domain_size) {
    DomainMask mask(size);
    mask.SetAllValid(size);
    domains.push_back(mask);
  }
  return domains;
}

std::vector<Cid> AllConstraints(const Model& model) {
  std::vector<Cid> cids;
  cids.reserve(model.num_constraints);
  for (Cid cid = 0; cid < model.num_constraints; ++cid) {
    cids.push_back(cid);
  }
  return cids;
}

std::vector<std::pair<VarId, Value>> AllProbes(
    const Model& model, const std::vector<DomainMask>& domains) {
  std::vector<std::pair<VarId, Value>> probes;
  for (VarId var = 0; var < model.num_vars; ++var) {
    for (Value value = 0; value < model.domain_size[var]; ++value) {
      if (domains[var].Test(value)) {
        probes.push_back({var, value});
      }
    }
  }
  return probes;
}

std::vector<uint64_t> SubscriptionDegrees(const Model& model) {
  std::vector<uint64_t> degrees;
  degrees.reserve(model.subscription.size());
  for (const auto& edges : model.subscription) {
    degrees.push_back(edges.size());
  }
  return degrees;
}

std::string JsonEscape(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '"' || c == '\\') {
      out.push_back('\\');
    }
    out.push_back(c);
  }
  return out;
}

double PercentileDouble(std::vector<double> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const double clamped = std::max(0.0, std::min(100.0, p));
  const double rank = (clamped / 100.0) * (values.size() - 1);
  const size_t lo = static_cast<size_t>(rank);
  const size_t hi = std::min(lo + 1, values.size() - 1);
  const double t = rank - lo;
  return values[lo] * (1.0 - t) + values[hi] * t;
}

void WriteUintArray(std::ostream* os, const std::vector<uint32_t>& values) {
  *os << "[";
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) {
      *os << ",";
    }
    *os << values[i];
  }
  *os << "]";
}

void WriteJson(const CliOptions& opt, const Model& model,
               const std::vector<WorldResult>& results,
               const std::vector<uint64_t>& epoch_samples,
               const StorageEstimate& storage,
               const PartitionStats& partition, std::ostream* os) {
  uint64_t ok = 0;
  uint64_t dwo = 0;
  uint64_t unknown = 0;
  uint64_t events = 0;
  uint64_t revise = 0;
  uint64_t support_words = 0;
  uint64_t support_latency = 0;
  uint64_t support_bank_conflicts = 0;
  uint64_t support_max_bank_accesses = 0;
  uint64_t deleted = 0;
  uint64_t router_enqueued = 0;
  uint64_t router_deduped = 0;
  uint64_t router_overflow_drops = 0;
  std::vector<double> queue_p50_samples;
  std::vector<double> queue_p95_samples;
  std::vector<uint64_t> queue_max_samples;
  for (const WorldResult& result : results) {
    ok += result.status == WorldStatus::kOK;
    dwo += result.status == WorldStatus::kDWO;
    unknown += result.status == WorldStatus::kUNKNOWN;
    events += result.events;
    revise += result.revise_calls;
    support_words += result.support_words_touched;
    support_latency += result.support_latency_cycles;
    support_bank_conflicts += result.support_bank_conflicts;
    support_max_bank_accesses =
        std::max(support_max_bank_accesses, result.support_max_bank_accesses);
    deleted += result.status == WorldStatus::kDWO ? 1 : 0;
    router_enqueued += result.router_events_enqueued;
    router_deduped += result.router_events_deduped;
    router_overflow_drops += result.router_events_dropped_overflow;
    queue_p50_samples.push_back(result.queue_occupancy_p50);
    queue_p95_samples.push_back(result.queue_occupancy_p95);
    queue_max_samples.push_back(result.queue_occupancy_max);
  }
  std::vector<uint64_t> degrees = SubscriptionDegrees(model);
  const double unknown_rate =
      results.empty() ? 0.0 : static_cast<double>(unknown) / results.size();
  *os << "{\n";
  *os << "  \"config\": {\"mode\":\"" << JsonEscape(opt.mode)
      << "\",\"worlds\":" << opt.worlds << ",\"max_events\":"
      << opt.max_events << ",\"max_revise\":" << opt.max_revise
      << ",\"max_epochs\":" << opt.max_epochs
      << ",\"partitions\":" << opt.partition.num_partitions
      << ",\"partition_policy\":\"" << PartitionPolicyName(opt.partition.policy)
      << "\""
      << ",\"support_banks\":" << opt.support_oracle.num_banks
      << ",\"support_base_latency\":"
      << opt.support_oracle.base_latency_cycles
      << ",\"support_conflict_penalty\":"
      << opt.support_oracle.conflict_penalty_cycles << "},\n";
  *os << "  \"model\": {\"num_vars\":" << model.num_vars
      << ",\"num_constraints\":" << model.num_constraints
      << ",\"max_domain_size\":" << model.max_domain_size
      << ",\"bit_sup_bytes\":" << model.bit_sup_words.size() * sizeof(uint32_t)
      << ",\"subscription_degree_p50\":" << Percentile(degrees, 50)
      << ",\"subscription_degree_p95\":" << Percentile(degrees, 95)
      << ",\"subscription_degree_max\":"
      << (degrees.empty() ? 0 : *std::max_element(degrees.begin(), degrees.end()))
      << "},\n";
  *os << "  \"results\": {\"probes_total\":" << results.size()
      << ",\"ok\":" << ok << ",\"dwo\":" << dwo
      << ",\"unknown\":" << unknown << ",\"unknown_rate\":"
      << unknown_rate << ",\"confirmed_deletions\":" << deleted << "},\n";
  *os << "  \"telemetry\": {\"events\":" << events
      << ",\"revise_calls\":" << revise
      << ",\"support_words_touched\":" << support_words
      << ",\"support_latency_cycles\":" << support_latency
      << ",\"support_bank_conflicts\":" << support_bank_conflicts
      << ",\"support_max_bank_accesses\":" << support_max_bank_accesses
      << ",\"epochs_p50\":" << Percentile(epoch_samples, 50)
      << ",\"epochs_p95\":" << Percentile(epoch_samples, 95)
      << ",\"queue_occupancy_p50\":" << PercentileDouble(queue_p50_samples, 50)
      << ",\"queue_occupancy_p95\":" << PercentileDouble(queue_p95_samples, 95)
      << ",\"queue_occupancy_max\":"
      << (queue_max_samples.empty() ? 0
                                    : *std::max_element(queue_max_samples.begin(),
                                                        queue_max_samples.end()))
      << ",\"fanout_p95\":" << Percentile(degrees, 95)
      << ",\"partition_cross_event_ratio\":" << partition.cross_event_ratio
      << ",\"deleted_values\":" << deleted
      << ",\"router_events_enqueued\":" << router_enqueued
      << ",\"router_events_deduped\":" << router_deduped
      << ",\"router_events_dropped_overflow\":" << router_overflow_drops
      << "},\n";
  *os << "  \"partition\": {\"num_partitions\":" << partition.num_partitions
      << ",\"policy\":\"" << PartitionPolicyName(partition.policy) << "\""
      << ",\"local_events\":" << partition.local_events
      << ",\"cross_events\":" << partition.cross_events
      << ",\"cross_event_ratio\":" << partition.cross_event_ratio
      << ",\"max_partition_degree\":" << partition.max_partition_degree
      << ",\"max_var_degree\":" << partition.max_var_degree
      << ",\"high_degree_hub_count\":" << partition.high_degree_hub_count
      << ",\"vars_per_partition\":";
  WriteUintArray(os, partition.vars_per_partition);
  *os << ",\"constraints_per_partition\":";
  WriteUintArray(os, partition.constraints_per_partition);
  *os << "},\n";
  *os << "  \"storage\": {\"bit_sup_bytes\":" << storage.bit_sup_bytes
      << ",\"domain_state_bytes_per_world\":"
      << storage.domain_state_bytes_per_world
      << ",\"domain_state_bytes_total\":" << storage.domain_state_bytes_total
      << ",\"frontier_bytes_per_world\":" << storage.frontier_bytes_per_world
      << ",\"router_buffer_bytes\":" << storage.router_buffer_bytes
      << ",\"bram18_estimate\":" << storage.bram18_estimate
      << ",\"uram288_estimate\":" << storage.uram288_estimate
      << ",\"likely_onchip_fit\":"
      << (storage.likely_onchip_fit ? "true" : "false") << "}\n";
  *os << "}\n";
}

}  // namespace
}  // namespace fpga_cpim

int main(int argc, char** argv) {
  using namespace fpga_cpim;
  const CliOptions opt = ParseArgs(argc, argv);
  if (opt.instance != "synthetic") {
    std::cerr << "第一版只支持 --instance synthetic\n";
    return 2;
  }

  Model model = MakeSyntheticModel(opt.synthetic);
  std::string error;
  if (!ValidateModel(model, &error)) {
    std::cerr << "模型非法: " << error << "\n";
    return 2;
  }
  std::vector<DomainMask> base_domains = FullDomains(model);
  std::vector<WorldResult> results;
  std::vector<uint64_t> epoch_samples;

  if (opt.mode == "ac") {
    EngineConfig cfg;
    cfg.num_worlds = opt.worlds;
    cfg.max_events_per_probe = opt.max_events;
    cfg.max_revise_calls_per_probe = opt.max_revise;
    cfg.max_epochs_per_probe = opt.max_epochs;
    cfg.max_support_words_per_probe = opt.max_support_words;
    cfg.support_oracle = opt.support_oracle;
    PropagationEngine engine(model, cfg);
    WorldResult result = engine.RunAC(base_domains, AllConstraints(model));
    results.push_back(result);
    epoch_samples.push_back(result.epochs);
  } else {
    EngineConfig cfg;
    cfg.num_worlds = opt.worlds;
    cfg.max_events_per_probe = opt.max_events;
    cfg.max_revise_calls_per_probe = opt.max_revise;
    cfg.max_epochs_per_probe = opt.max_epochs;
    cfg.max_support_words_per_probe = opt.max_support_words;
    cfg.nsac_enabled = opt.mode == "nsacq";
    cfg.nsac_radius = opt.mode == "nsacq" ? opt.nsac_radius : 0;
    cfg.support_oracle = opt.support_oracle;
    PropagationEngine engine(model, cfg);
    results = engine.RunProbeBatch(base_domains, AllProbes(model, base_domains));
    for (const WorldResult& result : results) {
      epoch_samples.push_back(result.epochs);
    }
  }

  StorageEstimate storage =
      EstimateStorage(model, opt.worlds, 4096);
  PartitionStats partition = BuildPartition(model, opt.partition);
  if (!opt.json_path.empty()) {
    std::ofstream out(opt.json_path);
    if (!out) {
      std::cerr << "无法写入 JSON: " << opt.json_path << "\n";
      return 1;
    }
    WriteJson(opt, model, results, epoch_samples, storage, partition, &out);
  }
  WriteJson(opt, model, results, epoch_samples, storage, partition, &std::cout);
  return 0;
}
