//#include "Solver.h"
//namespace cp {
//RNSQ::RNSQ(Network* m) :AC3bit(m) {
//	const int vars_bit_length = ceil(float(m_->vars.size()) / BITSIZE);
//	for (auto i : m_->vars) {
//		neibor_[i].resize(vars_bit_length, 0);
//		for (auto v : m_->neighborhood[i]) {
//			auto a = GetBitIdx(v->id());
//			neibor_[i][get<0>(a)].set(get<1>(a));
//		}
//	}
//	q_var_.initial(m_->vars.size());
//	q_nei_.initial(m_->vars.size());
//}
//
//ConsistencyState RNSQ::conditionFC(IntVar* x, const int level) {
//	level_ = level;
//	//在subscription上检查相容性
//	for (auto c : m_->subscription[x]) {
//		for (auto y : c->scope) {
//			if (!y->assigned() && y != x) {
//				if (revise(arc(c, y))) {
//					if (y->faild()) {
//						cs.tab = c;
//						cs.var = y;
//						++(c->weight);
//						cs.state = false;
//						return cs;
//					}
//				}
//			}
//		}
//	}
//
//	//检查临域上其它变量的相容性
//	for (auto c : m_->tabs) {
//		if (in_neibor_exp(c, x)) {
//			for (auto v : c->scope) {
//				if (!v->assigned()) {
//					if (revise(arc(c, v))) {
//						if (v->faild()) {
//							cs.tab = c;
//							cs.var = v;
//							++(c->weight);
//							cs.state = false;
//							return cs;
//						}
//					}
//				}
//			}
//		}
//	}
//	//for (auto v : m_->neighborhood[x]) {
//	//	for (auto c : m_->subscription[v])
//	//		for (auto y : c->scope)
//	//			if (!y->assigned() && y != v &&y != x)
//	//				if (revise(arc(c, y)))
//	//					if (y->faild()) {
//	//						cs.tab = c;
//	//						cs.var = y;
//	//						cs.state = false;
//	//						return cs;
//	//					}
//	//}
//	cs.state = true;
//	return cs;
//}
//
//ConsistencyState RNSQ::neiborAC(vector<IntVar*>& x_evt, IntVar* w, const int level) {
//	level_ = level;
//	q_nei_.clear();
//
//	for (auto v : x_evt)
//		insert_(q_nei_, v);
//	insert_(q_nei_, w);
//
//	while (!q_nei_.empty()) {
//		IntVar* x = q_nei_.pop();
//		for (Tabular* c : m_->subscription[x]) {
//			//检查该约束
//			if (in_neibor(c, w)) {
//				if (stamp_var_[x->id()] > stamp_tab_[c->id()]) {
//					for (auto y : c->scope) {
//						if (!y->assigned()) {
//							bool aa = false;
//							for (auto z : c->scope)
//								if ((z != x) && stamp_var_[z->id()] > stamp_tab_[c->id()])
//									aa = true;
//
//							if ((y != x) || aa)
//								if (revise(arc(c, y))) {
//									if (y->faild()) {
//										cs.tab = c;
//										cs.var = y;
//										++(c->weight);
//										//cout << c->id()<<": weight = "<<c->weight << endl;
//										cs.state = false;
//										return cs;
//									}
//									insert_(q_nei_, y);
//								}
//						}
//					}
//					++t_;
//					stamp_tab_[c->id()] = t_;
//				}
//			}
//		}
//	}
//	cs.state = true;
//	return cs;
//}
//
//void RNSQ::insert_(var_que& q, IntVar* v) {
//	q.push(v);
//	++t_;
//	stamp_var_[v->id()] = t_;
//}
//
//bool RNSQ::in_neibor_exp(Tabular* t, IntVar* x) {		//临域检查
//	for (auto v : t->scope)
//		if (!is_neibor(x, v))
//			return false;
//	return true;
//}
//bool RNSQ::is_neibor(IntVar* x, IntVar* v) {
//	auto a = GetBitIdx(v->id());
//	return neibor_[x][get<0>(a)].test(get<1>(a));
//}
//bool RNSQ::in_neibor(Tabular * t, IntVar * x) {
//	for (auto v : t->scope)
//		if (!is_neibor(x, v) && v != x)
//			return false;
//	return true;
//}
//
//bool RNSQ::has_sigleton_domain_neibor(IntVar* x) const {
//	for (auto v : m_->neighborhood[x])
//		if (v->size() == 1 && !v->assigned())
//			return true;
//	return false;
//}
//
//ConsistencyState RNSQ::enforce(vector<IntVar*>& x_evt, const int level) {
//	level_ = level;
//	q_var_.clear();
//	cs.level = level;
//	cs.num_delete = 0;
//	bool deletion = false;
//
//	//if (level == 0 || x_evt[0]->assigned()) {
//	if (level == 0)
//		for (auto v : x_evt)
//			q_var_.push(v);
//	else
//		for (auto v : m_->neighborhood[x_evt[0]])
//			//if (!v->assigned()) {
//			q_var_.push(v);
//	//}
//
//
//	while (!q_var_.empty()) {
//		IntVar* x = q_var_.pop();
//		deletion = false;
//
//		for (auto a : x->values()) {
//			if (x->have(a)) {
//				x->ReduceTo(a, level + 1);
//				x->assign(true);
//				bool result = conditionFC(x, level + 1).state;
//				level_ = level;
//
//				if (!result) {
//					//cout << IntVal(x, a) << "!result" << endl;
//					m_->RestoreUpto(level);
//					x->assign(false);
//					x->RemoveValue(a, level);
//					deletion = true;
//				}
//				else {
//					m_->RestoreUpto(level);
//					x->assign(false);
//				}
//				//	if (has_sigleton_domain_neibor(x)) {
//				//		result = neiborAC(m_->neighborhood[x], x, level + 1).state;
//				//		if (!result) {
//				//			m_->RestoreUpto(level);
//				//			x->assign(false);
//				//			x->RemoveValue(a, level);
//				//			deletion = true;
//				//		}
//				//	}
//				//}
//
//				if (x->size() == 0) {
//					cs.state = false;
//					return cs;
//				}
//			}
//		}
//
//		if (deletion) {
//			//q_var_.clear();
//			for (auto v : m_->neighborhood[x])
//				if (!v->assigned()) {
//					q_var_.push(v);
//				}
//		}
//	}
//	//}
//	cs.state = true;
//	return cs;
//}
//
//}
