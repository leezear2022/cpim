#include "fpga_cpim/termination.hpp"

namespace fpga_cpim {

void TerminationDetector::OnEventEnqueue() {
  ++state_.inflight_events;
}

void TerminationDetector::OnEventDequeue() {
  if (state_.inflight_events > 0) {
    --state_.inflight_events;
  }
}

void TerminationDetector::OnTileStart() {
  ++state_.active_tiles;
}

void TerminationDetector::OnTileDone() {
  if (state_.active_tiles > 0) {
    --state_.active_tiles;
  }
}

void TerminationDetector::OnDeleteProduced() {
  ++state_.pending_deletes;
}

void TerminationDetector::OnDeleteApplied() {
  if (state_.pending_deletes > 0) {
    --state_.pending_deletes;
  }
}

bool TerminationDetector::FixedPoint(const EventRouter& router) const {
  return router.Empty() && state_.active_tiles == 0 &&
         state_.inflight_events == 0 && state_.pending_deletes == 0;
}

}  // namespace fpga_cpim
