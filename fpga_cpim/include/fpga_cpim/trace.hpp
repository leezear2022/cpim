#ifndef FPGA_CPIM_TRACE_HPP_
#define FPGA_CPIM_TRACE_HPP_

#include <cstdint>
#include <string>
#include <vector>

#include "fpga_cpim/bitset.hpp"
#include "fpga_cpim/model.hpp"

namespace fpga_cpim {

struct TraceEvent {
  uint64_t cycle = 0;
  std::string type;
  WorldId world = 0;
  Cid cid = 0;
  VarId var = 0;
  uint8_t dirs = 0;
  bool dwo = false;
  std::vector<uint32_t> mask_words;
};

class TraceRecorder {
 public:
  void Add(const TraceEvent& event);
  bool WriteJson(const std::string& path) const;
  const std::vector<TraceEvent>& Events() const { return events_; }

 private:
  std::vector<TraceEvent> events_;
};

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_TRACE_HPP_
