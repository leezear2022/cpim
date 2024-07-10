//
// Created by lee on 24-7-9.
//

#ifndef CPIMBASE_H
#define CPIMBASE_H
#include <iostream>
namespace cpim {
// 定义类型
using u64 = unsigned long;
using u32 = unsigned int;
using u16 = unsigned short;
using u8 = unsigned char;

using i64 = long;
using i32 = int;
using i16 = short;
using i8 = char;

inline std::ostream &operator<<(std::ostream &os, const std::vector<int> &vec) {
  os << "[";
  for (size_t i = 0; i < vec.size(); ++i) {
    os << vec[i];
    if (i != vec.size() - 1) {
      os << ", ";
    }
  }
  os << "]";
  return os;
}
// 重载 << 运算符以输出 std::vector<std::vector<int>>
inline std::ostream &operator<<(std::ostream &os,
                                const std::vector<std::vector<int>> &vec) {
  for (const auto &v : vec) {
    os << "( ";
    for (const auto &elem : v) {
      os << elem << " ";
    }
    os << ")";
  }
  return os;
}
}  // namespace cpim

#endif  // CPIMBASE_H
