#include "model/xcsp_parser.h"

#include <glog/logging.h>

#include "model/libxml2_parser.h"

namespace cpim::model {

std::unique_ptr<XcspParser> XcspParser::Create(ParserType type) {
  switch (type) {
    case ParserType::kLibXml2:
      LOG(INFO) << "Creating LibXml2Parser";
      return std::make_unique<LibXml2Parser>();

    case ParserType::kPugiXml:
      LOG(ERROR) << "PugiXml parser not implemented yet";
      return nullptr;

    case ParserType::kTinyXml2:
      LOG(ERROR) << "TinyXml2 parser not implemented yet";
      return nullptr;

    default:
      LOG(ERROR) << "Unknown parser type";
      return nullptr;
  }
}

}  // namespace cpim::model
