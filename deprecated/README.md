# Deprecated Code

This directory contains deprecated code that has been replaced by modern implementations.

## Files

- **XBuilder.h / XBuilder.cpp**: Old XCSP3 parser using Xerces-C
  - **Replaced by**: `model/libxml2_parser.h` + `model/model_builder.h`
  - Uses modern C++17, libxml2, Abseil libraries
  - Better performance and cleaner API

- **main.cu / main_old.cu.bak**: Old main programs using XBuilder
  - **Replaced by**: `samples/main_new_parser.cpp`
  - New program: `build/cpim_test_parser`

- **object.h / shared.h**: Unused reference-counted object infrastructure
  - Appears to be copied from another project (CINN)
  - Not referenced by any code in the project
  - If needed in the future, use modern std::shared_ptr instead

## Migration Guide

### Old Code (Deprecated)
```cpp
#include "xcsp3model/XBuilder.h"

const XBuilder builder(X_PATH, XRT_BM_PATH);
const HModel hm = HModelNode::Make();
builder.GenerateHModel(hm);
```

### New Code (Recommended)
```cpp
#include "model/xcsp_parser.h"

auto parser = XcspParser::Create(ParserType::kLibXml2);
auto model = parser->Parse(benchmark_path);
```

## Why Deprecated?

1. **Xerces-C is heavy**: ~10MB library, slow initialization
2. **libxml2 is lighter**: ~1MB, faster parsing
3. **Modern C++ features**: RAII, smart pointers, absl containers
4. **Type safety**: Strong-typed IDs prevent bugs
5. **Better error handling**: absl::Status instead of bool

## How to Use New Parser

```bash
cd build
./cpim_test_parser ../samples/bench/queens-4_ext.xml
```

See `samples/main_new_parser.cpp` for complete example.
