#include "fpga_cpim/trace.hpp"

#include <fstream>

#include "fpga_cpim/bitset.hpp"

namespace fpga_cpim {

void TraceRecorder::Add(const TraceEvent& event) {
  events_.push_back(event);
}

bool TraceRecorder::WriteJson(const std::string& path) const {
  std::ofstream out(path);
  if (!out) {
    return false;
  }
  out << "[\n";
  for (size_t i = 0; i < events_.size(); ++i) {
    const TraceEvent& e = events_[i];
    out << "  {\"cycle\":" << e.cycle << ",\"type\":\"" << e.type
        << "\",\"world\":" << e.world << ",\"cid\":" << e.cid
        << ",\"var\":" << e.var << ",\"dirs\":" << static_cast<int>(e.dirs)
        << ",\"dwo\":" << (e.dwo ? "true" : "false") << ",\"mask\":[";
    for (size_t w = 0; w < e.mask_words.size(); ++w) {
      if (w != 0) {
        out << ",";
      }
      out << "\"" << HexWord(e.mask_words[w]) << "\"";
    }
    out << "]}";
    if (i + 1 != events_.size()) {
      out << ",";
    }
    out << "\n";
  }
  out << "]\n";
  return true;
}

}  // namespace fpga_cpim
