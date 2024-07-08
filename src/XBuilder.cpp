/*
 * XMLBuilder.cpp
 *
 *  Created on: 2016年6月21日
 *      Author: leezear
 */
#include "xcsp3model/XBuilder.h"

#include <sstream>

namespace cpim::common {

XBuilder::XBuilder(const std::string file_name, const XmlReaderType type) {
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
    delete parser_;
    XMLPlatformUtils::Terminate();
  }
}

bool XBuilder::initial(const std::string s) {
  if (s == "") {
    return false;
  }

  try {
    XMLPlatformUtils::Initialize();
  } catch (const XMLException& toCatch) {
    // Do your failure processing here
    return false;
  }
  // Do your actual work with Xerces-C++ here.
  parser_ = new XercesDOMParser();
  parser_->setValidationScheme(XercesDOMParser::Val_Always);
  parser_->setDoNamespaces(true);

  parser_->parse(s.c_str());
  document_ = parser_->getDocument();
  root_ = document_->getDocumentElement();

  if (!root_) {
    delete (parser_);
    parser_ = nullptr;
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

void XBuilder::del() const {
  if (root_) {
    delete parser_;
    XMLPlatformUtils::Terminate();
  }
}

void XBuilder::GenerateHModel(HModel hm) {
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
    // char* dom_id_str =
    //     XMLString::transcode(node->getAttributes()
    //                              ->getNamedItem(XMLString::transcode("domain"))
    //                              ->getTextContent());
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
