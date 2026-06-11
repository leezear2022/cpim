#ifndef FPGA_CPIM_TERMINATION_HPP_
#define FPGA_CPIM_TERMINATION_HPP_

#include <cstdint>

#include "fpga_cpim/event_router.hpp"

namespace fpga_cpim {

struct TerminationState {
  uint64_t active_tiles = 0;
  uint64_t inflight_events = 0;
  uint64_t pending_deletes = 0;
  uint64_t epoch = 0;
};

class TerminationDetector {
 public:
  void OnEventEnqueue();
  void OnEventDequeue();
  void OnTileStart();
  void OnTileDone();
  void OnDeleteProduced();
  void OnDeleteApplied();

  bool FixedPoint(const EventRouter& router) const;
  const TerminationState& State() const { return state_; }

 private:
  TerminationState state_;
};

}  // namespace fpga_cpim

#endif  // FPGA_CPIM_TERMINATION_HPP_
