#include "Solver.h"
namespace cpim {
RPC3::RPC3(Network *m) :AC(m) {
	const int num_vars = m_->vars.size();
	const int max_dom_size = m->max_domain_size();
	rel_.resize(num_vars, vector<vector<vector<int>>>(num_vars, vector<vector<int>>(max_dom_size, vector<int>(max_dom_size, false))));
	common_neibor_.resize(num_vars, vector<vector<IntVar*>>(num_vars));
	//初始化约束查找矩阵
	neibor_matrix.resize(num_vars, vector<vector<Tabular*>>(num_vars, vector<Tabular*>()));
	for (const auto c : m->tabs) {
		neibor_matrix[c->scope[0]->id()][c->scope[1]->id()].push_back(c);
		neibor_matrix[c->scope[1]->id()][c->scope[0]->id()].push_back(c);
	}
	//set<QVar*> vars_map;
	vector<bool> vars_in(num_vars, false);
	for (auto x : m->vars) {
		for (auto y : m->vars) {
			if (x != y) {
				for (auto z : m->vars) {
					if (!neibor_matrix[x->id()][z->id()].empty() && !neibor_matrix[y->id()][z->id()].empty()) {
						//vars_map.insert(z);
						if (!vars_in[z->id()]) {
							vars_in[z->id()] = true;
							common_neibor_[x->id()][y->id()].push_back(z);
						}
					}
				}
			}
			vars_in.assign(vars_in.size(), false);
		}
	}

	neighborhood.resize(num_vars);
	for (const auto x : m->vars)
		for (const auto y : m->vars)
			if ((x != y) && !neibor_matrix[x->id()][y->id()].empty())
				neighborhood[x->id()].push_back(y);
	//initial bitSup
	//const int n = num_tabs * max_dom_size * max_arity;
	//bitSup_ = new u64*[n];
	//for (size_t i = 0; i < n; i++)
	//	bitSup_[i] = new u64[max_bitDom_size]();

	for (auto* c : m->tabs) {
		const int x = c->scope[0]->id();
		const int y = c->scope[1]->id();
		for (auto& t : c->tuples()) {
			rel_[x][y][t[0]][t[1]] = true;
			rel_[y][x][t[1]][t[0]] = true;
		}
	}

	//for (QTab* c : tabs) {
	//	const int x = c->scope[0]->id;
	//	const int y = c->scope[1]->id;
	//	cout << "------------------------" << endl;
	//	cout << "c:" << c->id << "; x = " << x << "; y=" << y << endl;
	//	for (size_t i = 0; i < max_dom_size; i++) {
	//		for (size_t j = 0; j < max_dom_size; j++) {
	//			cout << rel_[x][y][i][j] << " ";
	//		}
	//		cout << endl;
	//	}
	//	cout << endl;

	//	for (size_t i = 0; i < max_dom_size; i++) {
	//		for (size_t j = 0; j < max_dom_size; j++) {
	//			cout << rel_[y][x][j][i] << " ";
	//		}
	//		cout << endl;
	//	}
	//	cout << endl;
	//}

	/////
	//q_neibor_matrix.initial(num_vars);
	//N.resize(num_vars);
	//生成邻域子网矩阵
	//for (auto v : vars) {
	//	for (auto x : neighborhood[v->id()]) {
	//		for (auto y : neighborhood[v->id()]) {
	//			//只计算三角形，避免重复加入约束
	//			if (x->id > y->id && !neibor_matrix[x->id()][y->id()].empty()) {
	//				N[v->id()].push_back(neibor_matrix[x->id()][y->id()][0]);
	//			}
	//		}
	//	}
	//}

	//var_mark_.resize(num_vars, -1);

	con_que_.initial(num_vars);
	for (auto x : m->vars) {
		for (auto y : m->vars) {
			if (!neibor_matrix[x->id()][y->id()].empty()) {
				con_que_.push(variable_pair{ x,y });
			}
		}
	}
	//con_que_.clear_and_push(tabs);
	r_1_.resize(num_vars, vector<vector<int>>(max_dom_size, vector<int>(num_vars, Limits::INDEX_OVERFLOW)));
	r_2_.resize(num_vars, vector<vector<int>>(max_dom_size, vector<int>(num_vars, Limits::INDEX_OVERFLOW)));
}

ConsistencyState RPC3::enforce(vector<IntVar*>& x_evt, const int p) {

	//初始化con_que_
	if (x_evt.size() == 1) {
		con_que_.clear();
		//con_que_.clear_and_push(subscription[x_evt[0]->id()]);
		const auto x = x_evt[0];
		for (auto y : neighborhood[x->id()]) {
			if (!y->assigned())
				con_que_.push(variable_pair{ y,x });
			//con_que_.push(variable_pair{ x,y });
		}
	}
	int R;

	while (!con_que_.empty()) {
		const auto vp = con_que_.pop();
		//cout << "pop:" << vp.x->id << "," << vp.y->id << endl;
		bool deletion = false;
		for (auto a = vp.x->head(); a != Limits::INDEX_OVERFLOW; vp.x->next_value(a)) {
			//cout << "test: (" << vp.x->id << "," << a << ")" << endl;
			const bool valid[] = {
				vp.y->have(r_1_[vp.x->id()][a][vp.y->id()]),
				vp.y->have(r_2_[vp.x->id()][a][vp.y->id()]) };
			const auto c = neibor_matrix[vp.x->id()][vp.y->id()][0];
			//cout << "r1[" << vp.x->id << "," << a << "," << vp.y->id << "] = " << r_1_[vp.x->id()][a][vp.y->id()] << ":" << valid[0] << endl;
			//cout << "r2[" << vp.x->id << "," << a << "," << vp.y->id << "] = " << r_2_[vp.x->id()][a][vp.y->id()] << ":" << valid[1] << endl;
			//both r valid
			if (valid[0] && valid[1]) {
				continue;
			}
			else {
				if (valid[0] && !valid[1]) {
					R = r_1_[vp.x->id()][a][vp.y->id()];
				}
				else if (!valid[0] && valid[1]) {
					R = r_2_[vp.x->id()][a][vp.y->id()];
				}
				else {
					R = Limits::INDEX_OVERFLOW;
				}

				//cout << "find_two_support(" << vp.x->id << "," << a << "," << vp.y->id << "," << R << ")" << endl;
				if (!find_two_support(*vp.x, a, *vp.y, R, p)) {
					vp.x->RemoveValue(a);
					//cout << "remove P:" << p << ":" << vp.x->id << "," << a << endl;
					deletion = true;
				}
			}

			if (vp.x->faild()) {
				IncrementWeight(c);  // Phase 0.1
				cs.state = false;
				return cs;
			}

			if (deletion) {
				for (const auto z : neighborhood[vp.x->id()]) {
					if (!z->assigned()) {
						con_que_.push(variable_pair{ z,vp.x });
					}
					//for (const auto w : neighborhood[z->id()]) {
					//	if (!I.assigned(*w) && w != vp.x) {
					//		auto c = neibor_matrix[w->id()][vp.x->id()];
					//		if (!c.empty()) {
					//			con_que_.push(variable_pair{ w,z });
					//		}
					//	}
					//}
				}
			}
		}
	}
	cs.state = true;
	return cs;
}
bool RPC3::is_consistent(const IntVar& x, const int a, const IntVar& y, const int b) {
	//cout << "      consistent check:(" << x.id << "," << a << ")~(" << y.id << "," << b << ") =>" << rel_[x.id()][y.id()][a][b] << endl;
	return rel_[x.id()][y.id()][a][b];
}

//bool RPC3::is_consistent(const QTab& c, const QVar& x, const int a, const QVar& y, const int b) {
//	int t[2];
//	t[c.index(x)] = a;
//	t[c.index(y)] = b;
//	return rel_[c.id()][t[0]][t[1]];
//}

int RPC3::find_two_support(const IntVar& x, const int a, const IntVar& y, const int r, const int p) {
	//cout << "----FTS_1-----" << endl;
	bool one_support = (r != Limits::INDEX_OVERFLOW);
	//const auto c_xy = neibor_matrix[x.id()][y.id()][0];

	for (auto b = y.head(); b != Limits::INDEX_OVERFLOW; b = y.next(b)) {
		//cout << "  test: (" << x.id << "," << a << ")-(" << y.id << "," << last_b << ")" << endl;

		//L4
		if (is_consistent(x, a, y, b)) {
			//cout << "consistent" << endl;
			if (!one_support) {
				//L5 若一个支持都没有，放在r1
				//cout << "  1 support" << endl;
				one_support = true;
				r_1_[x.id()][a][y.id()] = b;
			}
			else {
				//有一个支持放在r2
				//cout << "  2 support" << endl;
				if (b != r) {
					r_2_[x.id()][a][y.id()] = b;
					return true;
				}
				else {
					r_1_[x.id()][a][y.id()] = b;
					r_2_[x.id()][a][y.id()] = Limits::INDEX_OVERFLOW;
				}
			}

		}
	}

	//cout << "----FTS_2_1S-----" << endl;
	//若单个支持 则要为(ai,aj)检查pc
	//singleton
	if (!one_support) {
		//无支持
		//cout << "  no support" << endl;
		return false;
	}
	else {
		//单个支持
		const int b = r_1_[x.id()][a][y.id()];
		//b=last_b

		//cout << "  1 support, seek pc" << endl;
		for (auto z : common_neibor_[x.id()][y.id()]) {
			//cout << "  pcw: " << z->id << endl;
			const bool valid[] = {
				z->have(r_1_[x.id()][a][z->id()]),
				z->have(r_2_[x.id()][a][z->id()]),
				z->have(r_1_[y.id()][b][z->id()]),
				z->have(r_2_[y.id()][b][z->id()]) };
			//R[x,a,z]有一个有效，并且该有效R与y,b相容
			//if (valid[0] || valid[1]) {
			//if (is_consistent(y, b, *z, r_1_[x.id()][a][z->id()])||is_consistent(y, b, *z, r_2_[x.id()][a][z->id()])) {
			//	//cout << "   consistent" << endl;
			//	continue;
			//}
			if (valid[0]) {
				//cout << "    valid[0]: (" << z->id << "," << r_1_[x.id()][a][z->id()] << endl;
				//并且该有效R与y,b相容
				if (is_consistent(y, b, *z, r_1_[x.id()][a][z->id()])) {
					//cout << "   consistent" << endl;
					continue;
				}
				else {
					//cout << "   inconsistent" << endl;
				}
			}

			if (valid[1]) {
				//cout << "    valid[1]: (" << z->id << "," << r_2_[x.id()][a][z->id()] << endl;
				//并且该有效R与y,b相容
				if (is_consistent(y, b, *z, r_2_[x.id()][a][z->id()])) {
					//cout << "   consistent" << endl;
					continue;
				}
				else {
					//cout << "   inconsistent" << endl;
				}


			}
			//如果两个都valid那么上一个
			//}
			///////////////////////////////////

			//if (valid[2] || valid[3]) {
			//cout << "    valid[2]: (" << z->id << "," << r_1_[y.id()][b][z->id()] << endl;
			if (valid[2]) {
				//并且该有效R与x,a相容
				if (is_consistent(x, a, *z, r_1_[y.id()][b][z->id()])) {
					//cout << "   consistent" << endl;
					continue;
				}
				else {
					//cout << "   inconsistent" << endl;
				}

			}
			if (valid[3]) {
				//并且该有效R与x,a相容
				//cout << "    valid[3]: (" << z->id << "," << r_2_[y.id()][b][z->id()] << endl;
				if (is_consistent(x, a, *z, r_2_[y.id()][b][z->id()])) {
					//cout << "    consistent" << endl;

					continue;
				}
				else {
					//cout << "    inconsistent" << endl;
				}

			}
			//如果两个都valid那么上一个
			//}

			bool pc_witness = false;
			//cout << "  non valid r, seek new pcw" << endl;
			for (auto c = z->head(); c != Limits::INDEX_OVERFLOW; z->next_value(c)) {
				//cout << "  test: (" << x.id << "," << a << ")-(" << z->id << "," << c << ")-(" << y.id << ", " << b << ")" << endl;
				if (is_consistent(x, a, *z, c) && is_consistent(y, b, *z, c)) {
					//cout << "    consistent" << endl;
					pc_witness = true;
					break;
				}
			}

			if (!pc_witness) {
				//cout << "    non consistent need delete" << endl;
				return false;
			}

		}
	}

	return true;
}
}