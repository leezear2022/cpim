#ifndef CPIM_MODEL_CMODEL_ADAPTER_H_
#define CPIM_MODEL_CMODEL_ADAPTER_H_

#include <cstdint>
#include <vector>

#include <cuda_runtime.h>

namespace cpim {
class HModel;
}

namespace cpim::model {

class IntermediateModel;

struct BitDomainLayout {
  int num_vars = 0;
  int max_dom_size = 0;
  int bit_dom_int_size = 0;
  std::vector<uint32_t> data;  // size = num_vars * bit_dom_int_size
};

struct BitSupportLayout {
  int num_constraints = 0;
  int bit_sup_int_size = 0;  // number of uint2 entries per constraint
  std::vector<uint2> data;
};

struct SubscriptionCSR {
  std::vector<int> offsets;   // size = num_vars + 1
  std::vector<uint3> entries; // (x_id, y_id, constraint_id)
};

class CModelAdapter {
 public:
  static CModelAdapter FromHModel(const HModel& model);
  static CModelAdapter FromIntermediate(const IntermediateModel& model);

  const BitDomainLayout& domains() const { return domains_; }
  const BitSupportLayout& supports() const { return supports_; }
  const SubscriptionCSR& subscriptions() const { return subscriptions_; }
  const std::vector<uint3>& constraints() const { return constraints_; }
  const std::vector<int>& domain_sizes() const { return domain_sizes_; }
  const std::vector<int>& degrees() const { return degrees_; }

  int num_vars() const { return domains_.num_vars; }
  int num_tabs() const { return static_cast<int>(constraints_.size()); }
  int max_dom_size() const { return domains_.max_dom_size; }
  int bit_dom_int_size() const { return domains_.bit_dom_int_size; }
  int bit_sup_int_size() const { return supports_.bit_sup_int_size; }

 private:
  BitDomainLayout domains_;
  BitSupportLayout supports_;
  SubscriptionCSR subscriptions_;
  std::vector<uint3> constraints_;
  std::vector<int> domain_sizes_;
  std::vector<int> degrees_;
};

}  // namespace cpim::model

#endif  // CPIM_MODEL_CMODEL_ADAPTER_H_
