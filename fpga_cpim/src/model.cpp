#include "fpga_cpim/model.hpp"

#include <algorithm>
#include <cassert>
#include <random>
#include <set>
#include <sstream>

namespace fpga_cpim {

uint32_t WordCountForBits(uint32_t nbits) {
  return (nbits + 31) / 32;
}

uint32_t LastWordMask(uint32_t nbits) {
  const uint32_t rem = nbits % 32;
  if (nbits == 0) {
    return 0;
  }
  if (rem == 0) {
    return 0xffffffffu;
  }
  return (uint32_t{1} << rem) - 1;
}

Model MakeEmptyModel(const std::vector<uint32_t>& domain_sizes) {
  Model model;
  model.num_vars = static_cast<uint32_t>(domain_sizes.size());
  model.domain_size = domain_sizes;
  model.subscription.assign(model.num_vars, {});
  model.max_domain_size = 0;
  for (uint32_t size : domain_sizes) {
    model.max_domain_size = std::max(model.max_domain_size, size);
  }
  return model;
}

Cid AddBinaryConstraint(
    Model* model,
    VarId x,
    VarId y,
    const std::vector<std::pair<Value, Value>>& allowed_pairs) {
  assert(model != nullptr);
  assert(x < model->num_vars);
  assert(y < model->num_vars);
  const uint32_t x_size = model->domain_size[x];
  const uint32_t y_size = model->domain_size[y];
  const uint32_t x_words = WordCountForBits(x_size);
  const uint32_t y_words = WordCountForBits(y_size);

  BinaryConstraint c;
  c.cid = static_cast<Cid>(model->constraints.size());
  c.x = x;
  c.y = y;
  c.x_domain_size = x_size;
  c.y_domain_size = y_size;
  c.bit_sup_offset_dir0 = model->bit_sup_words.size();
  model->bit_sup_words.resize(model->bit_sup_words.size() +
                              static_cast<size_t>(x_size) * y_words, 0);
  c.bit_sup_offset_dir1 = model->bit_sup_words.size();
  model->bit_sup_words.resize(model->bit_sup_words.size() +
                              static_cast<size_t>(y_size) * x_words, 0);

  for (const auto& [xv, yv] : allowed_pairs) {
    if (xv >= x_size || yv >= y_size) {
      continue;
    }
    const uint64_t dir0_index = c.bit_sup_offset_dir0 +
                                static_cast<uint64_t>(xv) * y_words + yv / 32;
    const uint64_t dir1_index = c.bit_sup_offset_dir1 +
                                static_cast<uint64_t>(yv) * x_words + xv / 32;
    model->bit_sup_words[dir0_index] |= uint32_t{1} << (yv % 32);
    model->bit_sup_words[dir1_index] |= uint32_t{1} << (xv % 32);
  }

  model->constraints.push_back(c);
  model->scopes.push_back({x, y});
  model->subscription[x].push_back(c.cid);
  model->subscription[y].push_back(c.cid);
  model->num_constraints = static_cast<uint32_t>(model->constraints.size());
  return c.cid;
}

Model MakeEqualityModel(uint32_t domain_size) {
  Model model = MakeEmptyModel({domain_size, domain_size});
  std::vector<std::pair<Value, Value>> allowed;
  for (Value v = 0; v < domain_size; ++v) {
    allowed.push_back({v, v});
  }
  AddBinaryConstraint(&model, 0, 1, allowed);
  return model;
}

Model MakeLessThanModel(uint32_t domain_size) {
  Model model = MakeEmptyModel({domain_size, domain_size});
  std::vector<std::pair<Value, Value>> allowed;
  for (Value x = 0; x < domain_size; ++x) {
    for (Value y = 0; y < domain_size; ++y) {
      if (x < y) {
        allowed.push_back({x, y});
      }
    }
  }
  AddBinaryConstraint(&model, 0, 1, allowed);
  return model;
}

namespace {

std::vector<std::pair<VarId, VarId>> BuildPairs(const SyntheticConfig& cfg) {
  std::set<std::pair<VarId, VarId>> pairs;
  if (cfg.graph == "chain") {
    for (VarId i = 0; i + 1 < cfg.vars; ++i) {
      pairs.insert({i, i + 1});
    }
  } else if (cfg.graph == "hub") {
    for (VarId i = 1; i < cfg.vars; ++i) {
      pairs.insert({0, i});
    }
  } else if (cfg.graph == "grid") {
    const uint32_t width = static_cast<uint32_t>(std::max(1.0, std::sqrt(cfg.vars)));
    for (VarId i = 0; i < cfg.vars; ++i) {
      if (i + 1 < cfg.vars && (i + 1) % width != 0) {
        pairs.insert({i, i + 1});
      }
      if (i + width < cfg.vars) {
        pairs.insert({i, i + width});
      }
    }
  } else if (cfg.degree > 0) {
    for (VarId i = 0; i < cfg.vars; ++i) {
      for (uint32_t d = 1; d <= cfg.degree; ++d) {
        VarId j = (i + d) % cfg.vars;
        if (i != j) {
          pairs.insert({std::min(i, j), std::max(i, j)});
        }
      }
    }
  } else {
    std::mt19937 rng(cfg.seed);
    std::uniform_real_distribution<double> coin(0.0, 1.0);
    for (VarId i = 0; i < cfg.vars; ++i) {
      for (VarId j = i + 1; j < cfg.vars; ++j) {
        if (coin(rng) < cfg.density) {
          pairs.insert({i, j});
        }
      }
    }
  }
  if (pairs.empty() && cfg.vars >= 2) {
    pairs.insert({0, 1});
  }
  return {pairs.begin(), pairs.end()};
}

std::vector<std::pair<Value, Value>> BuildRelation(
    uint32_t domain, const SyntheticConfig& cfg, uint32_t cid_seed) {
  std::vector<std::pair<Value, Value>> allowed;
  if (cfg.relation == "equality") {
    for (Value v = 0; v < domain; ++v) {
      allowed.push_back({v, v});
    }
    return allowed;
  }
  if (cfg.relation == "not-equal") {
    for (Value x = 0; x < domain; ++x) {
      for (Value y = 0; y < domain; ++y) {
        if (x != y) {
          allowed.push_back({x, y});
        }
      }
    }
    return allowed;
  }
  if (cfg.relation == "less-than") {
    for (Value x = 0; x < domain; ++x) {
      for (Value y = 0; y < domain; ++y) {
        if (x < y) {
          allowed.push_back({x, y});
        }
      }
    }
    return allowed;
  }

  std::mt19937 rng(cfg.seed * 1315423911u + cid_seed * 2654435761u);
  std::uniform_real_distribution<double> coin(0.0, 1.0);
  const double allow_probability = std::max(0.0, std::min(1.0, 1.0 - cfg.tightness));
  for (Value x = 0; x < domain; ++x) {
    for (Value y = 0; y < domain; ++y) {
      if (coin(rng) < allow_probability) {
        allowed.push_back({x, y});
      }
    }
  }
  if (allowed.empty() && domain > 0) {
    allowed.push_back({0, 0});
  }
  return allowed;
}

}  // namespace

Model MakeSyntheticModel(const SyntheticConfig& cfg) {
  std::vector<uint32_t> domains(cfg.vars, cfg.domain);
  Model model = MakeEmptyModel(domains);
  auto pairs = BuildPairs(cfg);
  uint32_t cid_seed = 0;
  for (const auto& [x, y] : pairs) {
    AddBinaryConstraint(&model, x, y, BuildRelation(cfg.domain, cfg, cid_seed++));
  }
  return model;
}

bool ValidateModel(const Model& model, std::string* error) {
  auto fail = [&](const std::string& msg) {
    if (error != nullptr) {
      *error = msg;
    }
    return false;
  };
  if (model.word_bits != 32) {
    return fail("only 32-bit words are supported");
  }
  if (model.domain_size.size() != model.num_vars ||
      model.subscription.size() != model.num_vars) {
    return fail("variable arrays do not match num_vars");
  }
  if (model.constraints.size() != model.num_constraints ||
      model.scopes.size() != model.num_constraints) {
    return fail("constraint arrays do not match num_constraints");
  }
  for (Cid cid = 0; cid < model.constraints.size(); ++cid) {
    const auto& c = model.constraints[cid];
    if (c.cid != cid || c.x >= model.num_vars || c.y >= model.num_vars) {
      return fail("invalid constraint id or scope");
    }
    const uint64_t dir0_size =
        static_cast<uint64_t>(c.x_domain_size) * WordCountForBits(c.y_domain_size);
    const uint64_t dir1_size =
        static_cast<uint64_t>(c.y_domain_size) * WordCountForBits(c.x_domain_size);
    if (c.bit_sup_offset_dir0 + dir0_size > model.bit_sup_words.size() ||
        c.bit_sup_offset_dir1 + dir1_size > model.bit_sup_words.size()) {
      return fail("bitSup offset out of bounds");
    }
  }
  return true;
}

}  // namespace fpga_cpim
