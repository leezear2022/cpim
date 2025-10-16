/*
 * XMLBuilder.cpp
 *
 *  Created on: 2016年6月21日
 *      Author: leezear
 */
#include "xcsp3model/XBuilder.h"

#include <glog/logging.h>

#include <sstream>

namespace cpim::common {

XBuilder::XBuilder(const std::string& file_name, const XmlReaderType type) {
  const bool res = initial(file_name);
  if (res && type == XRT_BM_PATH) {
    benchmark_path_ = GetBMFile();
    del();
    initial(benchmark_path_);
  } else if (res && type == XRT_BM) {
    benchmark_path_ = file_name;
  } else {
    std::cout << "error" << std::endl;
  }
  const int last0 = benchmark_path_.find_last_of('/');
  const int last1 = benchmark_path_.find_last_of('\\');
  const int last2 = (last0 > last1 ? last0 : last1) + 1;
  file_name_ =
      benchmark_path_.substr(last2, benchmark_path_.size() - 4 - last2);
}

XBuilder::~XBuilder() {
  if (root_) {
    parser_.reset();  // 使用智能指针自动删除对象
    XMLPlatformUtils::Terminate();
  }
}

bool XBuilder::initial(const std::string& s) {
  if (s.empty()) {
    return false;
  } else {
    LOG(INFO) << "current bm file: " << s;
  }

  try {
    XMLPlatformUtils::Initialize();
  } catch (const XMLException& toCatch) {
    // 处理初始化失败
    return false;
  }

  // 使用 std::unique_ptr 管理 XercesDOMParser
  parser_ = std::make_unique<XercesDOMParser>();
  parser_->setValidationScheme(XercesDOMParser::Val_Always);
  parser_->setDoNamespaces(true);

  parser_->parse(s.c_str());
  document_ = parser_->getDocument();
  root_ = document_->getDocumentElement();

  if (!root_) {
    parser_.reset();
    return false;
  }
  return true;
}

std::string XBuilder::GetBMFile() const {
  DOMNodeList* bmfile_list =
      root_->getElementsByTagName(XMLString::transcode("BMFile"));
  DOMNode* bmfile = bmfile_list->item(0);
  char* bm_name = XMLString::transcode(bmfile->getFirstChild()->getNodeValue());
  std::string bm_file(bm_name);
  XMLString::release(&bm_name);
  return bm_file;
}

// void XBuilder::generateDomains(XModel *model) const {
// 	DOMNode *doms_nodes =
// root_->getElementsByTagName(XMLString::transcode("domains"))->item(0);
// const int num_doms = XMLString::parseInt(
// 		doms_nodes->getAttributes()->getNamedItem(XMLString::transcode("nbDomains"))->getTextContent());
// 	DOMNodeList *dom_nodes =
// root_->getElementsByTagName(XMLString::transcode("domain"));

// 	for (int i = 0; i < num_doms; ++i) {
// 		DOMNode * node = dom_nodes->item(i);
// 		const int size = XMLString::parseInt(
// 			node->getAttributes()->getNamedItem(XMLString::transcode("nbValues"))->getTextContent());
// 		char* values =
// XMLString::transcode(node->getFirstChild()->getNodeValue());
// model->add(i, size, values); 		XMLString::release(&values);
// 	}
// }

void XBuilder::generate_tuples(const std::string& ts_str_, int size, int arity,
                               IntTuples& tuples) {
  std::istringstream iss(ts_str_);
  std::string token;
  tuples.clear();
  tuples.resize(size, std::vector<int>(arity));

  int i = 0;
  std::string tuple_str;
  std::istringstream tuple_stream;  // 在循环外声明

  while (std::getline(iss, tuple_str, '|') && i < size) {
    tuple_stream.clear();         // 重置状态
    tuple_stream.str(tuple_str);  // 重新设置内容

    for (int j = 0; j < arity; ++j) {
      if (tuple_stream >> token) {
        tuples[i][j] = std::stoi(token);
      }
    }
    ++i;
  }
}

std::vector<int> XBuilder::get_scope(const std::string& scp_str) {
  // 使用 stringstream 和 istringstream 来解析 scope_str
  std::istringstream iss(scp_str);
  std::string token;
  // scope.reserve(arity);
  std::vector<int> scope;
  while (iss >> token) {
    if (token[0] == 'V') {
      scope.push_back(std::stoi(token.substr(1)));
    }
  }
  return scope;
}

void XBuilder::get_scope(const std::string& scp_str, std::vector<int>& scp) {
  // 使用 stringstream 和 istringstream 来解析 scope_str
  std::istringstream iss(scp_str);
  std::string token;
  // scope.reserve(arity);
  while (iss >> token) {
    if (token[0] == 'V') {
      scp.push_back(std::stoi(token.substr(1)));
    }
  }
}

void XBuilder::del() {
  parser_.reset();  // 使用智能指针自动删除对象
  XMLPlatformUtils::Terminate();
}

void XBuilder::GenerateHModel(const HModel& hm) const {
  DOMNode* doms_nodes =
      root_->getElementsByTagName(XMLString::transcode("domains"))->item(0);
  const int num_doms =
      XMLString::parseInt(doms_nodes->getAttributes()
                              ->getNamedItem(XMLString::transcode("nbDomains"))
                              ->getTextContent());
  DOMNodeList* dom_nodes =
      root_->getElementsByTagName(XMLString::transcode("domain"));

  // generate domians
  std::vector<std::vector<int>> doms(num_doms);

  for (u32 i = 0; i < num_doms; ++i) {
    DOMNode* node = dom_nodes->item(i);
    const int size =
        XMLString::parseInt(node->getAttributes()
                                ->getNamedItem(XMLString::transcode("nbValues"))
                                ->getTextContent());
    char* values = XMLString::transcode(node->getFirstChild()->getNodeValue());
    doms[i] = parseCharArray(values);
    XMLString::release(&values);
  }

  // generate variables
  DOMNode* vars_node =
      root_->getElementsByTagName(XMLString::transcode("variables"))->item(0);
  const int num_vars = XMLString::parseInt(
      vars_node->getAttributes()
          ->getNamedItem(XMLString::transcode("nbVariables"))
          ->getTextContent());
  DOMNodeList* var_nodes =
      root_->getElementsByTagName(XMLString::transcode("variable"));

  for (u32 i = 0; i < num_vars; ++i) {
    DOMNode* node = var_nodes->item(i);
    char* var_name_str =
        XMLString::transcode(node->getAttributes()
                                 ->getNamedItem(XMLString::transcode("name"))
                                 ->getTextContent());
    char* dom_id_str =
        XMLString::transcode(node->getAttributes()
                                 ->getNamedItem(XMLString::transcode("domain"))
                                 ->getTextContent());
    int var_index = extractNumberFromString(var_name_str);
    int domain_id = extractNumberFromString(dom_id_str);

    hm->AddVar(var_index, std::string(var_name_str), doms[domain_id]);

    // model->add(i, dom_id_str);
    XMLString::release(&var_name_str);
    XMLString::release(&dom_id_str);
  }

  // generate relations
  DOMNode* relations_node =
      root_->getElementsByTagName(XMLString::transcode("relations"))->item(0);
  const int relations_count = XMLString::parseInt(
      relations_node->getAttributes()
          ->getNamedItem(XMLString::transcode("nbRelations"))
          ->getTextContent());
  DOMNodeList* relation_nodes =
      root_->getElementsByTagName(XMLString::transcode("relation"));

  HRels Rels;
  for (int i = 0; i < relations_count; ++i) {
    DOMNode* node = relation_nodes->item(i);
    const int arity =
        XMLString::parseInt(node->getAttributes()
                                ->getNamedItem(XMLString::transcode("arity"))
                                ->getTextContent());
    char* semantics = XMLString::transcode(
        node->getAttributes()
            ->getNamedItem(XMLString::transcode("semantics"))
            ->getTextContent());
    const int size =
        XMLString::parseInt(node->getAttributes()
                                ->getNamedItem(XMLString::transcode("nbTuples"))
                                ->getTextContent());

    HRel Rel;
    if (size != 0) {
      char* ts_str =
          XMLString::transcode(node->getFirstChild()->getNodeValue());
      IntTuples tuples;
      generate_tuples(ts_str, size, arity, tuples);
      Rel = std::make_tuple(i, arity, size, std::string(semantics), tuples);
      // std::cout << tuples << std::endl;
      XMLString::release(&ts_str);
    } else {
      Rel = std::make_tuple(i, arity, size, semantics,
                            std::vector<std::vector<int>>());
    }
    XMLString::release(&semantics);
    Rels.push_back(Rel);
  }

  // generate constraints
  DOMNode* cons_node =
      root_->getElementsByTagName(XMLString::transcode("constraints"))->item(0);
  const int num_cons = XMLString::parseInt(
      cons_node->getAttributes()
          ->getNamedItem(XMLString::transcode("nbConstraints"))
          ->getTextContent());
  DOMNodeList* con_nodes =
      root_->getElementsByTagName(XMLString::transcode("constraint"));

  for (int i = 0; i < num_cons; ++i) {
    DOMNode* node = con_nodes->item(i);
    const int arity =
        XMLString::parseInt(node->getAttributes()
                                ->getNamedItem(XMLString::transcode("arity"))
                                ->getTextContent());
    char* scp_str =
        XMLString::transcode(node->getAttributes()
                                 ->getNamedItem(XMLString::transcode("scope"))
                                 ->getTextContent());
    char* rel_id_str = XMLString::transcode(
        node->getAttributes()
            ->getNamedItem(XMLString::transcode("reference"))
            ->getTextContent());
    int rel_id = extractNumberFromString(rel_id_str);
    std::vector<int> scope = get_scope(scp_str);
    // std::cout << std::get<3>(Rels[rel_id]) << std::endl;
    // std::cout << (std::get<3>(Rels[rel_id]) == "supports") << std::endl;
    hm->AddTab(std::get<3>(Rels[rel_id]) == "supports",
               std::get<4>(Rels[rel_id]), scope);
    XMLString::release(&scp_str);
    XMLString::release(&rel_id_str);
  }
}

std::vector<int> XBuilder::parseCharArray(const char* input) {
  std::vector<int> result;

  // Convert char* to std::string for easier manipulation
  std::string str(input);

  // Check if the input is a range like "0..3"
  size_t rangePos = str.find("..");
  if (rangePos != std::string::npos) {
    // Extract start and end of the range
    int start = std::stoi(str.substr(0, rangePos));
    int end = std::stoi(str.substr(rangePos + 2));

    // Add all numbers in the range to the vector
    for (int i = start; i <= end; ++i) {
      result.push_back(i);
    }
  } else {
    // Split by comma for individual values
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, ',')) {
      // Remove potential leading and trailing whitespaces
      item.erase(0, item.find_first_not_of(' '));
      item.erase(item.find_last_not_of(' ') + 1);

      // Convert item to int and add to the vector
      result.push_back(std::stoi(item));
    }
  }

  return result;
}

// Helper function to extract the number from a given string
int XBuilder::extractNumberFromString(const char* str) {
  std::string s(str);

  // Find the first digit in the string
  size_t pos = s.find_first_of("0123456789");

  // Extract the substring starting from the first digit
  std::string numberStr = s.substr(pos);

  // Convert the substring to an integer
  int number = std::stoi(numberStr);

  return number;
}

std::string XBuilder::path() const { return benchmark_path_; }
std::string XBuilder::file_name() const { return file_name_; }
}  // namespace cpim::common
