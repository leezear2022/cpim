//
// Created by lee on 24-7-4.
//
#include <utility>
#include <algorithm>
#include "xcsp3model/h_model.h"

namespace cpim ::common {
    // HVar HVarNode::Make(int id, const std::string &name, int min_val, int max_val) {
    //     auto node = make_shared<HVarNode>(id, name, min_val, max_val);
    //     return HVar(node);
    // }

    HVar HVarNode::Make(int id, const std::string &name, int min_val, int max_val) {
        // auto node = make_shared<HVarNode>();
        auto node = make_shared<HVarNode>(id, name, min_val, max_val);
        return HVar(node);
    }

    HVarNode::HVarNode(int id, std::string name, int min_val, int max_val): id(id), name(std::move(name)),
                                                                            std_max(max_val - min_val) {
        int j = 0;
        const int size = max_val - min_val + 1;
        vals.resize(size);
        anti_map.resize(size);
        for (int i = min_val; i <= max_val; ++i) {
            val_map[i] = j;
            vals[j] = j;
            anti_map[j] = i;
            ++j;
        }
    }

    HTab HTabNode::Make(int id, bool sem, std::vector<std::vector<int> > &ts, std::vector<HVar> &scp) {
        auto node = make_shared<HTabNode>(id, sem, ts, scp);
        return HTab(node);
    }

    HTabNode::HTabNode(int id, bool sem, std::vector<std::vector<int> > &ts,
                       std::vector<HVar> &scp) : id(id), semantics(sem), scope(scp) {
        unsigned long all_size = 1;
        for (auto i: scp)
            all_size *= i->vals.size();
        unsigned long sup_size;

        if (!sem)
            sup_size = all_size - ts.size();
        else
            sup_size = ts.size();
        std::vector<int> ori_t_(scope.size());
        std::vector<int> std_t_(scope.size());
        tmp_t_.resize(scope.size());
        tuples.resize(sup_size, std::vector<int>(scope.size()));

        if (sem) {
            for (size_t i = 0; i < sup_size; i++) {
                GetSTDTuple(ts[i], std_t_);
                tuples[i] = std_t_;
            }
        } else {
            int j = 0;
            for (int i = 0; (i < all_size) && (j <= sup_size); ++i) {
                GetTuple(i, ori_t_, std_t_);
                if (std::find(ts.begin(), ts.end(), ori_t_) == ts.end())
                    tuples[j++] = std_t_;
            }
        }

        semantics = true;
        isSTD = true;
    }

    void HTabNode::GetSTDTuple(std::vector<int> &src_tuple, std::vector<int> &std_tuple) {
        for (size_t i = 0; i < src_tuple.size(); ++i)
            std_tuple[i] = scope[i]->val_map[src_tuple[i]];
    }

    void HTabNode::GetORITuple(std::vector<int> &std_tuple, std::vector<int> &ori_tuple) {
        for (size_t i = 0; i < std_tuple.size(); ++i)
            ori_tuple[i] = scope[i]->anti_map[std_tuple[i]];
    }

    void HTabNode::GetTuple(int idx, std::vector<int> &src_t, std::vector<int> &std_t) {
        for (int i = (scope.size() - 1); i >= 0; --i) {
            HVar v = scope[i];
            std_t[i] = idx % v->vals.size();
            src_t[i] = v->anti_map[std_t[i]];
            idx /= v->vals.size();
        }
    }
}
