//
// Created by lee on 24-7-4.
//
#include <Solver.h>

#include <filesystem>
#include <iostream>

// #include "Network2.h"
#include <glog/logging.h>

#include "cuSAC.cuh"
#include "xcsp3model/HModel.h"
#include "xcsp3model/XBuilder.h"
using namespace cpim;
using namespace cpim::common;
// const std::string X_PATH =
//     "/home/lee/CLionProjects/cpim/samples/bench/BMPath.xml";
const std::string X_PATH = "../samples/bench/BMPath.xml";

constexpr long TimeLimit = 900000;
int main(int argc, char* argv[]) {
  const XBuilder builder(X_PATH, XRT_BM_PATH);
  const HModel hm = HModelNode::Make();
  builder.GenerateHModel(hm);
  // hm->show();
  auto* n = new Network(hm);
  // n->show(0);
  //
  // AC3bit ac_(n);
  // ac_.enforce(n->vars, 0);
  // n->show(0);

  // MAC mac(n, CA_RPC3, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);
  MAC mac(n, AC_3bit, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);

  const SearchStatistics statistics = mac.enforce(TimeLimit);
  cout << mac.sol_str << endl;
  cout << "is solution = " << mac.solution_check() << endl;
  cout << "time = " << statistics.solve_time << endl;
  cout << "positive = " << statistics.num_positive << endl;
  cout << "negative = " << statistics.num_negative << endl;

  delete n;
  // CModel cm(hm);
  // cm.enforceGAC();
  // cm.BuildBitModel(hm);
  // cm.DelGPUModel();
  // BuildBitModel(hm);
  // DelGPUModel();
  // M_Con.clear();

  // std::cout << "jiji2\n";
  // shared_ptr<Network> n = std::make_shared<Network>(hm);

  // auto* n = new Network(hm);
  // MAC mac(n, AC_3bit, Heuristic::VRH_DOM_WDEG_MIN, Heuristic::VLH_MIN);
  // AC3bit ac_(n);

  return 0;
  // ac_.enforce(n->vars, 0);
  ////hm->show();
  ////MAC mac(n, AC_3, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);
  // n->vars[0]->ReduceTo(0, 0);
  // n->vars[0]->assign(true, 0);
  // vector<IntVar*> vs;
  // vs.push_back(n->vars[0]);
  // ac_.enforce(vs, 0);
  // n->show(0);
  // MAC mac(n, CA_LMRPC_BIT, Heuristic::VRH_DOM_WDEG_MIN, Heuristic::VLH_MIN);
  // MAC mac(n, AC_3bit, Heuristic::VRH_DOM_WDEG_MIN, Heuristic::VLH_MIN);
  // MAC mac(n, CA_LMRPC_BIT, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);
  // MAC mac(n, AC_3bit, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);
  // MAC mac(n, CA_RPC3, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);
  // const SearchStatistics statistics = mac.enforce(TimeLimit);
  // cout << "is solution = " << mac.solution_check() << endl;
  // cout << "time = " << statistics.solve_time << endl;
  // cout << "positive = " << statistics.num_positive << endl;
  // cout << "negative = " << statistics.num_negative << endl;

  // n->show(0);
  // lMaxRPC ac(n);
  // ac.enforce(n->vars, 0);
  // n->show(0);

  // n->show(0);
  ////SAC1 sac(n, AC_3bit);
  // SAC3 sac(n, AC_3bit, Heuristic::VRH_DOM_MIN, Heuristic::VLH_MIN);
  // sac.enforce(n->vars, 0);
  // cout << sac.del() << endl;
  // n->show(0);
  return 0;
}
