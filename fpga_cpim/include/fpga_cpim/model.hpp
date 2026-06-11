#ifndef FPGA_CPIM_MODEL_HPP_
#define FPGA_CPIM_MODEL_HPP_

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace fpga_cpim {

using VarId = uint32_t;
using Cid = uint32_t;
using Value = uint32_t;
using Word = uint32_t;
using WorldId = uint32_t;

struct BinaryConstraint {
  Cid cid = 0;
  VarId x = 0;
  VarId y = 0;
  uint32_t x_domain_size = 0;
  uint32_t y_domain_size = 0;

  // Direction 0: x -> y, rows are x values, words cover y values.
  // Direction 1: y -> x, rows are y values, words cover x values.
  uint64_t bit_sup_offset_dir0 = 0;
  uint64_t bit_sup_offset_dir1 = 0;
};

struct Model {
  uint32_t num_vars = 0;
  uint32_t num_constraints = 0;
  uint32_t max_domain_size = 0;
  uint32_t word_bits = 32;

  std::vector<uint32_t> domain_size;
  std::vector<BinaryConstraint> constraints;
  std::vector<uint32_t> bit_sup_words;
  std::vector<std::vector<Cid>> subscription;
  std::vector<std::pair<VarId, VarId>> scopes;
};

struct SyntheticConfig {
  std::string graph = "random";
  uint32_t vars = 32;
  uint32_t domain = 32;
  double density = 0.2;
  double tightness = 0.5;
  uint32_t degree = 0;
  uint32_t seed = 1;
  std::string relation = "random";
};

uint32_t WordCountForBits(uint32_t nbits);
uint32_t LastWordMask(uint32_t nbits);

Model MakeEmptyModel(const std::vector<uint32_t>& domain_sizes);

Cid AddBinaryConstraint(
    Model* model,
    VarId x,
    VarId y,
    const std::vector<std::pair<Value, Value>>& allowed_pairs);

Model MakeEqualityModel(uint32_t domain_size);
Model MakeLessThanModel(uint32_t domain_size);
Model MakeSyntheticModel(const SyntheticConfig& cfg);

bool ValidateModel(const Model& model, std::string* error);

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_MODEL_HPP_
