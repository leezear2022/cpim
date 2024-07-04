//
// Created by lee on 24-7-4.
//

#ifndef HMODEL_H
#define HMODEL_H
#include <string>
#include <unordered_map>
#include <vector>

#include "object.h"

namespace cpim {
    namespace common {
        class HVar;
        class HTab;
        class HModel;

        class HVarNode : public Object {
        public:
            int id;
            std::string name;
            std::vector<int> vals;
            std::unordered_map<int, int> val_map;
            std::vector<int> anti_map;
            const int std_min = INT32_MIN;
            const int std_max = INT32_MAX;

            static HVar Make(int id, const std::string &name, int min_val, int max_val);

            HVarNode(int id, std::string name, int min_val, int max_val);

            // HVarNode(int id, std::string name, std::vector<int> &v);

            // ~HVarNode() {
            // }

            // void Show();

        private
        :
        };

        class HVar : public Shared<HVarNode> {
        public:
            HVar() = default;

            explicit HVar(HVarNode *p): Shared(p) {
            }
        };

        class HTabNode : public Object {
        public:
            int id;
            std::string name;
            bool semantics;
            std::vector<HVar> scope;
            std::vector<std::vector<int> > tuples;
            bool isSTD = false;

            static HTab Make(int id, bool sem, std::vector<std::vector<int> > &ts, std::vector<HVar> &scp);

            HTabNode(int id, bool sem, std::vector<std::vector<int> > &ts, std::vector<HVar> &scp);

            //
            // HTabNode(HTabNode *t, std::vector<HVar *> &scp);
            //
            // int GetAllSize() const;
            //
            void GetSTDTuple(std::vector<int> &src_tuple, std::vector<int> &std_tuple);

            void GetORITuple(std::vector<int> &std_tuple, std::vector<int> &ori_tuple);

            bool SAT(std::vector<int> &t);

            bool SAT_STD(std::vector<int> &t);

            //
            // void Show();
            //
            void GetTuple(int idx, std::vector<int> &t, std::vector<int> &t_idx);

        private:
            //临时变量
            std::vector<int> tmp_t_;

            friend std::ostream &operator<<(std::ostream &os, const std::vector<HVar *> &a);
        };

        class HTab : public Shared<HTabNode> {
        public:
            HTab() = default;

            explicit HTab(HTabNode *p): Shared(p) {
            }
        };


        class HModelNode : public Object {
        public:
            std::vector<HVar> vars;
            std::vector<HTabNode> tabs;
            std::unordered_map<std::string, HVar> var_n_;

            HModelNode();

            virtual ~HModelNode();

            // void AddVar(int id, std::string name, int min_val,
            //             int max_val);
            //
            // void AddVar(int id, std::string name, std::vector<int> &v);
            //
            // void AddTab(int id, bool sem, std::vector<std::vector<int> > &ts, std::vector<HVar *> &scp);
            //
            // void AddTab(int id, bool sem, std::vector<std::vector<int> > &ts,
            //             std::vector<std::string> &scp);
            //
            // void AddTabAsPrevious(HTabNode *t, std::vector<std::string> &scp);
            //
            // int max_domain_size() const { return mds_; }
            //
            // void Show();

        private:
            size_t mds_ = 0;
            size_t mas_ = 0;
        };


        class HModel : public Shared<HModelNode> {
        };
    }
}


#endif //HMODEL_H
