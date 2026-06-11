#include "cpim_hls_types.hpp"

namespace fpga_cpim_hls {

bool hls_enqueue(DirtyEventHls queue[MAX_QUEUE], uint16_t* tail,
                 DirtyEventHls event) {
  if (*tail >= MAX_QUEUE) {
    return false;
  }
  queue[*tail] = event;
  ++(*tail);
  return true;
}

}  // namespace fpga_cpim_hls
