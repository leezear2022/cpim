# HLS-friendly Core

这里放第一版可综合风格 C++ 原型。普通本地验证使用：

```bash
g++ -std=c++17 -I fpga_cpim/hls fpga_cpim/hls/testbench_hls.cpp fpga_cpim/hls/*.cpp -o hls_tb
./hls_tb
```

约束：

- core 文件不使用 STL 容器。
- 固定最大参数。
- 静态数组。
- 溢出或预算命中返回 `UNKNOWN`。
- 第一版只实例化一个 revise/owner 流水。
