#include "Solver.h"
namespace cpim {
AC::AC(Network* m) :m_(m) {

	tmp_tuple_.resize(m_->max_arity());
	Exclude(tmp_tuple_);
}

//AC::AC(Network* m, const LookAhead look_ahead, const LookBack look_back) :
//	m_(m),
//	la_(look_ahead),
//	lb_(look_back) {
//	q_.initial(m_->vars.size());
//	stamp_var_.resize(m_->vars.size(), 0);
//	stamp_tab_.resize(m_->tabs.size(), 0);
//	tmp_tuple_.resize(m_->max_arity());
//	Exclude(tmp_tuple_);
//}

//void AC::insert(IntVar* v) {
//	q_.push_back(v);
//	++t_;
//	stamp_var_[v->id()] = t_;
//}



}
