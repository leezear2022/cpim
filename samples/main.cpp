//
// Created by lee on 24-7-4.
//
#include <iostream>

#include "xcsp3model/HModel.h"
using namespace cpim::common;

int main(int argc, char* argv[])
{
    {
        auto var0 = HVarNode::Make(0, 0, "var0", 0, 10);
        auto var1 = HVarNode::Make(1, 1, "var1", 0, 10);
        var0->Show();
        std::cout << var0->id << std::endl;
        std::cout << var1->id << std::endl;
        // std::vector<<>>
        std::vector<std::vector<int>> myVector = {{0, 0}, {1, 1}};
        std::vector<HVar> scp = {var0, var1};
        auto tab = HTabNode::Make(0, true, myVector, scp);
        std::cout << tab->id << std::endl;
    }
    std::cout << "jiji2\n";
    return 1;
}
