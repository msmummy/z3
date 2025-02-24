/*++
Copyright (c) 2012 Microsoft Corporation

Module Name:

    qe_lite.cpp

Abstract:

    Light weight partial quantifier-elimination procedure

Author:

    Nikolaj Bjorner (nbjorner) 2012-10-17

Revision History:


--*/
#include "qe_lite.h"
#include "expr_abstract.h"
#include "used_vars.h"
#include "occurs.h"
#include "for_each_expr.h"
#include "rewriter_def.h"
#include "ast_pp.h"
#include "ast_ll_pp.h"
#include "ast_smt2_pp.h"
#include "tactical.h"
#include "bool_rewriter.h"
#include "var_subst.h"
#include "uint_set.h"
#include "dl_util.h"
#include "th_rewriter.h"
#include "dl_util.h"
#include "for_each_expr.h"
#include "expr_safe_replace.h"
#include "cooperate.h"
#include "datatype_decl_plugin.h"

class is_variable_proc {
public:
    virtual bool operator()(expr* e) const = 0;
};

class is_variable_test : public is_variable_proc {
    enum is_var_kind { BY_VAR_SET, BY_VAR_SET_COMPLEMENT, BY_NUM_DECLS };
    uint_set m_var_set;
    unsigned m_num_decls;
    is_var_kind m_var_kind;
public:
    is_variable_test(uint_set const& vars, bool index_of_bound) :
        m_var_set(vars), 
        m_num_decls(0), 
        m_var_kind(index_of_bound?BY_VAR_SET:BY_VAR_SET_COMPLEMENT) {}

    is_variable_test(unsigned num_decls) :
        m_num_decls(num_decls),
        m_var_kind(BY_NUM_DECLS) {}

    virtual bool operator()(expr* e) const {
        if (!is_var(e)) {
            return false;
        }
        unsigned idx = to_var(e)->get_idx();
        switch(m_var_kind) {
        case BY_VAR_SET:
            return m_var_set.contains(idx);
        case BY_VAR_SET_COMPLEMENT:
            return !m_var_set.contains(idx);
        case BY_NUM_DECLS:
            return idx < m_num_decls;
        }
        UNREACHABLE();
        return false;
    }
};


namespace eq {
    class der {
        ast_manager &   m;
        arith_util      a;
        datatype_util   dt;
        is_variable_proc* m_is_variable;
        var_subst       m_subst;
        expr_ref_vector m_new_exprs;
        
        ptr_vector<expr> m_map;
        int_vector       m_pos2var;
        ptr_vector<var>  m_inx2var;
        unsigned_vector  m_order;
        expr_ref_vector  m_subst_map;
        expr_ref_buffer  m_new_args;
        th_rewriter      m_rewriter;
        volatile bool    m_cancel;
        
        void der_sort_vars(ptr_vector<var> & vars, ptr_vector<expr> & definitions, unsigned_vector & order) {
            order.reset();
            
            // eliminate self loops, and definitions containing quantifiers.
            bool found = false;
            for (unsigned i = 0; i < definitions.size(); i++) {
                var * v  = vars[i];
                expr * t = definitions[i];
                if (t == 0 || has_quantifiers(t) || occurs(v, t))
                    definitions[i] = 0;
                else
                    found = true; // found at least one candidate
            }
            
            if (!found)
                return;
            
            typedef std::pair<expr *, unsigned> frame;
            svector<frame> todo;
            
            expr_fast_mark1 visiting;
            expr_fast_mark2 done;
            
            unsigned vidx, num;
            
            for (unsigned i = 0; i < definitions.size(); i++) {
                if (definitions[i] == 0)
                    continue;
                var * v = vars[i];
                SASSERT(v->get_idx() == i);
                SASSERT(todo.empty());
                todo.push_back(frame(v, 0));
                while (!todo.empty()) {
                start:
                    frame & fr = todo.back();
                    expr * t   = fr.first;
                    if (t->get_ref_count() > 1 && done.is_marked(t)) {
                        todo.pop_back();
                        continue;
                    }
                    switch (t->get_kind()) {
                    case AST_VAR:
                        vidx = to_var(t)->get_idx();
                        if (fr.second == 0) {
                            CTRACE("der_bug", vidx >= definitions.size(), tout << "vidx: " << vidx << "\n";);
                            // Remark: The size of definitions may be smaller than the number of variables occuring in the quantified formula.
                            if (definitions.get(vidx, 0) != 0) {
                                if (visiting.is_marked(t)) {
                                    // cycle detected: remove t
                                    visiting.reset_mark(t);
                                    definitions[vidx] = 0;
                                }
                                else {
                                    visiting.mark(t);
                                    fr.second = 1;
                                    todo.push_back(frame(definitions[vidx], 0));
                                    goto start;
                                }
                            }
                        }
                        else {
                            SASSERT(fr.second == 1);
                            if (definitions.get(vidx, 0) != 0) {
                                visiting.reset_mark(t);
                                order.push_back(vidx);
                            }
                            else {
                                // var was removed from the list of candidate vars to elim cycle
                                // do nothing
                            }
                        }
                        if (t->get_ref_count() > 1)
                            done.mark(t);
                        todo.pop_back();
                        break;
                    case AST_QUANTIFIER:
                        UNREACHABLE();
                        todo.pop_back();
                        break;
                    case AST_APP:
                        num = to_app(t)->get_num_args();
                        while (fr.second < num) {
                            expr * arg = to_app(t)->get_arg(fr.second);
                            fr.second++;
                            if (arg->get_ref_count() > 1 && done.is_marked(arg))
                                continue;
                            todo.push_back(frame(arg, 0));
                            goto start;
                        }
                        if (t->get_ref_count() > 1)
                            done.mark(t);
                        todo.pop_back();
                        break;
                    default:
                        UNREACHABLE();
                        todo.pop_back();
                        break;
                    }
                }
            }
        }
        
        bool is_variable(expr * e) const {
            return (*m_is_variable)(e);
        }
        
        bool is_neg_var(ast_manager & m, expr * e, var*& v) {
            expr* e1;
            if (m.is_not(e, e1) && is_variable(e1)) {
                v = to_var(e1);
                return true;
            }
            else {
                return false;
            }
        }
        
        
        /**
           \brief Return true if e can be viewed as a variable disequality. 
           Store the variable id in v and the definition in t.
           For example:
           
           if e is (not (= (VAR 1) T)), then v assigned to 1, and t to T.
           if e is (iff (VAR 2) T), then v is assigned to 2, and t to (not T).
           (not T) is used because this formula is equivalent to (not (iff (VAR 2) (not T))),
           and can be viewed as a disequality.
        */
        bool is_var_diseq(expr * e, ptr_vector<var>& vs, expr_ref_vector& ts) {
            expr* e1;
            if (m.is_not(e, e1)) {
                return is_var_eq(e, vs, ts);
            }
            else if (is_var_eq(e, vs, ts) && vs.size() == 1 && m.is_bool(vs[0])) { 
                expr_ref tmp(m);
                bool_rewriter(m).mk_not(ts[0].get(), tmp);
                ts[0] = tmp;
                return true;
            }
            else {
                return false;
            }
        }

        bool solve_arith_core(app * lhs, expr * rhs, expr * eq, ptr_vector<var>& vs, expr_ref_vector& ts) {
            SASSERT(a.is_add(lhs));
            bool is_int  = a.is_int(lhs);
            expr * a1, *v;
            expr_ref def(m);
            rational a_val;
            unsigned num = lhs->get_num_args();
            unsigned i;
            for (i = 0; i < num; i++) {
                expr * arg = lhs->get_arg(i);
                if (is_variable(arg)) {
                    a_val = rational(1); 
                    v     = arg;
                    break;
                }
                else if (a.is_mul(arg, a1, v) && 
                         is_variable(v) && 
                         a.is_numeral(a1, a_val) &&
                         !a_val.is_zero() &&
                         (!is_int || a_val.is_minus_one())) {
                    break;
                }
            }
            if (i == num)
                return false;
            vs.push_back(to_var(v));
            expr_ref inv_a(m);
            if (!a_val.is_one()) {
                inv_a = a.mk_numeral(rational(1)/a_val, is_int);
                rhs   = a.mk_mul(inv_a, rhs);
            }
            
            ptr_buffer<expr> other_args;
            for (unsigned j = 0; j < num; j++) {
                if (i != j) {
                    if (inv_a)
                        other_args.push_back(a.mk_mul(inv_a, lhs->get_arg(j)));
                    else
                        other_args.push_back(lhs->get_arg(j));
                }
            }
            switch (other_args.size()) {
            case 0:
                def = rhs;
                break;
            case 1:
                def = a.mk_sub(rhs, other_args[0]);
                break;
            default:
                def = a.mk_sub(rhs, a.mk_add(other_args.size(), other_args.c_ptr()));
                break;
            }
            ts.push_back(def);
            return true;
        }

        
        bool arith_solve(expr * lhs, expr * rhs, expr * eq, ptr_vector<var>& vs, expr_ref_vector& ts) {
            return 
                (a.is_add(lhs) && solve_arith_core(to_app(lhs), rhs, eq, vs, ts)) ||
                (a.is_add(rhs) && solve_arith_core(to_app(rhs), lhs, eq, vs, ts));
        }

        bool trivial_solve(expr* lhs, expr* rhs, expr* eq, ptr_vector<var>& vs, expr_ref_vector& ts) {
            if (!is_variable(lhs)) {
                std::swap(lhs, rhs);
            }
            if (!is_variable(lhs)) {
                return false;
            }
            vs.push_back(to_var(lhs));
            ts.push_back(rhs);
            TRACE("qe_lite", tout << mk_pp(eq, m) << "\n";);
            return true;
        }
        

        bool same_vars(ptr_vector<var> const& vs1, ptr_vector<var> const& vs2) const {
            if (vs1.size() != vs2.size()) {
                return false;
            }
            for (unsigned i = 0; i < vs1.size(); ++i) {
                if (vs1[i] != vs2[i]) {
                    return false;
                }
            }
            return true;
        }

        /**
           \brief Return true if e can be viewed as a variable equality.
        */
        
        bool is_var_eq(expr * e, ptr_vector<var>& vs, expr_ref_vector & ts) {
            expr* lhs, *rhs;
            var* v;
            
            // (= VAR t), (iff VAR t), (iff (not VAR) t), (iff t (not VAR)) cases    
            if (m.is_eq(e, lhs, rhs) || m.is_iff(e, lhs, rhs)) {
                // (iff (not VAR) t) (iff t (not VAR)) cases
                if (!is_variable(lhs) && !is_variable(rhs) && m.is_bool(lhs)) {
                    if (!is_neg_var(m, lhs, v)) {
                        std::swap(lhs, rhs);
                    }
                    if (!is_neg_var(m, lhs, v)) {
                        return false;
                    }
                    vs.push_back(v);
                    ts.push_back(m.mk_not(rhs));
                    TRACE("qe_lite", tout << mk_pp(e, m) << "\n";);
                    return true;
                }
                if (trivial_solve(lhs, rhs, e, vs, ts)) {
                    return true;
                }
                if (arith_solve(lhs, rhs, e, vs, ts)) {
                    return true;
                }
                return false;
            }
            
            // (ite cond (= VAR t) (= VAR t2)) case
            expr* cond, *e2, *e3;
            if (m.is_ite(e, cond, e2, e3)) {
                if (is_var_eq(e2, vs, ts)) {
                    expr_ref_vector ts2(m);
                    ptr_vector<var> vs2;
                    if (is_var_eq(e3, vs2, ts2) && same_vars(vs, vs2)) {
                        for (unsigned i = 0; i < vs.size(); ++i) {
                            ts[i] = m.mk_ite(cond, ts[i].get(), ts2[i].get());
                        }
                        return true;
                    }
                }
                return false;
            }
            
            // VAR = true case
            if (is_variable(e)) {
                ts.push_back(m.mk_true());
                vs.push_back(to_var(e));
                TRACE("qe_lite", tout << mk_pp(e, m) << "\n";);
                return true;
            }
            
            // VAR = false case
            if (is_neg_var(m, e, v)) {
                ts.push_back(m.mk_false());
                vs.push_back(v);
                TRACE("qe_lite", tout << mk_pp(e, m) << "\n";);
                return true;
            }
            
            return false;
        }
        
        
        bool is_var_def(bool check_eq, expr* e, ptr_vector<var>& vs, expr_ref_vector& ts) {
            if (check_eq) {
                return is_var_eq(e, vs, ts);
            }
            else {
                return is_var_diseq(e, vs, ts);
            }    
        }
        
        void get_elimination_order() {
            m_order.reset();
            
            TRACE("top_sort",
                  tout << "DEFINITIONS: " << std::endl;
                  for(unsigned i = 0; i < m_map.size(); i++)
                      if(m_map[i]) tout << "VAR " << i << " = " << mk_pp(m_map[i], m) << std::endl;
                  );
            
            der_sort_vars(m_inx2var, m_map, m_order);
            
            TRACE("qe_lite", 
                  tout << "Elimination m_order:" << std::endl;
                  for(unsigned i=0; i<m_order.size(); i++)
                      {
                          if (i != 0) tout << ",";
                          tout << m_order[i];
                      }
                  tout << std::endl;            
                  );
        }    
        
        void create_substitution(unsigned sz) {
            m_subst_map.reset();
            m_subst_map.resize(sz, 0);        
            for (unsigned i = 0; i < m_order.size(); i++) {
                expr_ref cur(m_map[m_order[i]], m);            
                // do all the previous substitutions before inserting
                expr_ref r(m);
                m_subst(cur, m_subst_map.size(), m_subst_map.c_ptr(), r);            
                unsigned inx = sz - m_order[i]- 1;
                SASSERT(m_subst_map[inx]==0);
                m_subst_map[inx] = r;
            }
        }
        
        void flatten_args(quantifier* q, unsigned& num_args, expr*const*& args) {
            expr * e = q->get_expr();
            if ((q->is_forall() && m.is_or(e)) ||
                (q->is_exists() && m.is_and(e))) {
                num_args = to_app(e)->get_num_args();
                args     = to_app(e)->get_args();
            }
        }
        
        void apply_substitution(quantifier * q, expr_ref & r) {
            
            expr * e = q->get_expr();
            unsigned num_args = 1;
            expr* const* args = &e;
            flatten_args(q, num_args, args);
            bool_rewriter rw(m);
            
            // get a new expression
            m_new_args.reset();
            for(unsigned i = 0; i < num_args; i++) {
                int x = m_pos2var[i];
                if (x == -1 || m_map[x] == 0) {
                    m_new_args.push_back(args[i]);
                }
            }
            
            expr_ref t(m);
            if (q->is_forall()) {
                rw.mk_or(m_new_args.size(), m_new_args.c_ptr(), t);
            }
            else {
                rw.mk_and(m_new_args.size(), m_new_args.c_ptr(), t);
            }
            expr_ref new_e(m);    
            m_subst(t, m_subst_map.size(), m_subst_map.c_ptr(), new_e);
            
            // don't forget to update the quantifier patterns
            expr_ref_buffer  new_patterns(m);
            expr_ref_buffer  new_no_patterns(m);
            for (unsigned j = 0; j < q->get_num_patterns(); j++) {
                expr_ref new_pat(m);
                m_subst(q->get_pattern(j), m_subst_map.size(), m_subst_map.c_ptr(), new_pat);
                new_patterns.push_back(new_pat);
            }
            
            for (unsigned j = 0; j < q->get_num_no_patterns(); j++) {
                expr_ref new_nopat(m);
                m_subst(q->get_no_pattern(j), m_subst_map.size(), m_subst_map.c_ptr(), new_nopat);
                new_no_patterns.push_back(new_nopat);
            }
            
            r = m.update_quantifier(q, new_patterns.size(), new_patterns.c_ptr(), 
                                    new_no_patterns.size(), new_no_patterns.c_ptr(), new_e);
        }
        
        void reduce_quantifier1(quantifier * q, expr_ref & r, proof_ref & pr) {
            expr * e = q->get_expr();
            is_variable_test is_v(q->get_num_decls());
            set_is_variable_proc(is_v);
            unsigned num_args = 1;
            expr* const* args = &e;
            flatten_args(q, num_args, args);
            
            unsigned def_count = 0;
            unsigned largest_vinx = 0;
            
            find_definitions(num_args, args, q->is_exists(), def_count, largest_vinx);
            
            if (def_count > 0) {
                get_elimination_order();
                SASSERT(m_order.size() <= def_count); // some might be missing because of cycles
                
                if (!m_order.empty()) {            
                    create_substitution(largest_vinx + 1);
                    apply_substitution(q, r);
                }
                else {
                    r = q;
                }
            }
            else {
                TRACE("der_bug", tout << "Did not find any diseq\n" << mk_pp(q, m) << "\n";);
                r = q;
            }
            
            if (m.proofs_enabled()) {
                pr = r == q ? 0 : m.mk_der(q, r);
            }    
        }    
        
        void elim_unused_vars(expr_ref& r, proof_ref &pr) {
            if (is_quantifier(r)) {
                quantifier * q = to_quantifier(r);
                ::elim_unused_vars(m, q, r);
                if (m.proofs_enabled()) {
                    proof * p1 = m.mk_elim_unused_vars(q, r);
                    pr = m.mk_transitivity(pr, p1);
                }
            }
        }
        
        void find_definitions(unsigned num_args, expr* const* args, bool is_exists, unsigned& def_count, unsigned& largest_vinx) {
            def_count = 0;
            largest_vinx = 0;
            m_map.reset();
            m_pos2var.reset();
            m_inx2var.reset();    
            m_pos2var.reserve(num_args, -1);
            
            // Find all definitions
            for (unsigned i = 0; i < num_args; i++) {
                checkpoint();
                ptr_vector<var> vs;
                expr_ref_vector ts(m);
                if (is_var_def(is_exists, args[i], vs, ts)) {
                    for (unsigned j = 0; j < vs.size(); ++j) {
                        var* v = vs[j];
                        expr* t = ts[j].get();
                        unsigned idx = v->get_idx();
                        if (m_map.get(idx, 0) == 0) {
                            m_map.reserve(idx + 1, 0);
                            m_inx2var.reserve(idx + 1, 0);                
                            m_map[idx] = t;
                            m_inx2var[idx] = v;
                            m_pos2var[i] = idx;
                            def_count++;
                            largest_vinx = std::max(idx, largest_vinx); 
                            m_new_exprs.push_back(t);
                        }
                    }
                }
            }
        }

        void flatten_definitions(expr_ref_vector& conjs) {
            TRACE("qe_lite",
                  expr_ref tmp(m);
                  tmp = m.mk_and(conjs.size(), conjs.c_ptr());
                  tout << mk_pp(tmp, m) << "\n";);
            for (unsigned i = 0; i < conjs.size(); ++i) {
                expr* c = conjs[i].get();
                expr* l, *r;
                if (m.is_false(c)) {
                    conjs[0] = c;
                    conjs.resize(1);
                    break;
                }
                if (is_ground(c)) {
                    continue;
                }
                if (!m.is_eq(c, l, r)) {
                    continue;
                }
                if (!is_app(l) || !is_app(r)) {
                    continue;
                }
                if (dt.is_constructor(to_app(l)->get_decl())) {
                    flatten_constructor(to_app(l), to_app(r), conjs);
                    conjs[i] = conjs.back();
                    conjs.pop_back();
                    --i;
                    continue;
                }
                if (dt.is_constructor(to_app(r)->get_decl())) {
                    flatten_constructor(to_app(r), to_app(l), conjs);
                    conjs[i] = conjs.back();
                    conjs.pop_back();
                    --i;
                    continue;
                }
            }
            TRACE("qe_lite",
                  expr_ref tmp(m);
                  tmp = m.mk_and(conjs.size(), conjs.c_ptr());
                  tout << "after flatten\n" << mk_pp(tmp, m) << "\n";);
        }
        
        void flatten_constructor(app* c, app* r, expr_ref_vector& conjs) {
            SASSERT(dt.is_constructor(c));
            
            func_decl* d = c->get_decl();

            if (dt.is_constructor(r->get_decl())) {
                app* b = to_app(r);
                if (d == b->get_decl()) {
                    for (unsigned j = 0; j < c->get_num_args(); ++j) {
                        conjs.push_back(m.mk_eq(c->get_arg(j), b->get_arg(j)));
                    }
                }
                else {
                    conjs.push_back(m.mk_false());
                }                
            }
            else {
                func_decl* rec = dt.get_constructor_recognizer(d);
                conjs.push_back(m.mk_app(rec, r));
                ptr_vector<func_decl> const& acc = *dt.get_constructor_accessors(d);
                for (unsigned i = 0; i < acc.size(); ++i) {
                    conjs.push_back(m.mk_eq(c->get_arg(i), m.mk_app(acc[i], r)));
                }
            }
        }

        bool is_unconstrained(var* x, expr* t, unsigned i, expr_ref_vector const& conjs) {
            bool occ = occurs(x, t);
            for (unsigned j = 0; !occ && j < conjs.size(); ++j) {
                occ = (i != j) && occurs(x, conjs[j]);
            }
            return !occ;
        }

        bool remove_unconstrained(expr_ref_vector& conjs) {
            bool reduced = false, change = true;
            expr* r, *l, *ne;           
            while (change) {
                change = false;
                for (unsigned i = 0; i < conjs.size(); ++i) {
                    if (m.is_not(conjs[i].get(), ne) && m.is_eq(ne, l, r)) {
                        TRACE("qe_lite", tout << mk_pp(conjs[i].get(), m) << " " << is_variable(l) << " " << is_variable(r) << "\n";);
                        if (is_variable(l) && ::is_var(l) && is_unconstrained(::to_var(l), r, i, conjs)) {
                            conjs[i] = m.mk_true();
                            reduced = true;
                            change = true;
                        }
                        else if (is_variable(r) && ::is_var(r) && is_unconstrained(::to_var(r), l, i, conjs)) {
                            conjs[i] = m.mk_true();
                            reduced = true;
                            change = true;
                        }
                    }
                }
            }
            return reduced;
        }
        
        bool reduce_var_set(expr_ref_vector& conjs) {
            unsigned def_count = 0;
            unsigned largest_vinx = 0;
            bool reduced = false;

            flatten_definitions(conjs);
            
            find_definitions(conjs.size(), conjs.c_ptr(), true, def_count, largest_vinx);
            
            if (def_count > 0) {
                get_elimination_order();
                SASSERT(m_order.size() <= def_count); // some might be missing because of cycles
                
                if (!m_order.empty()) {            
                    expr_ref r(m), new_r(m);
                    r = m.mk_and(conjs.size(), conjs.c_ptr());
                    create_substitution(largest_vinx + 1);
                    m_subst(r, m_subst_map.size(), m_subst_map.c_ptr(), new_r);
                    m_rewriter(new_r);
                    conjs.reset();
                    datalog::flatten_and(new_r, conjs);
                    reduced = true;
                }
            }

            if (remove_unconstrained(conjs)) {
                reduced = true;
            }

            return reduced;
        }

        void checkpoint() {
            cooperate("der");
            if (m_cancel)
                throw tactic_exception(TACTIC_CANCELED_MSG);
        }

    public:
        der(ast_manager & m): 
            m(m), 
            a(m),
            dt(m),
            m_is_variable(0), 
            m_subst(m), 
            m_new_exprs(m), 
            m_subst_map(m), 
            m_new_args(m), 
            m_rewriter(m), 
            m_cancel(false) {}
        
        void set_is_variable_proc(is_variable_proc& proc) { m_is_variable = &proc;}
        
        void operator()(quantifier * q, expr_ref & r, proof_ref & pr) {
            TRACE("qe_lite", tout << mk_pp(q, m) << "\n";);    
            pr = 0;
            r  = q;
            reduce_quantifier(q, r, pr);    
            if (r != q) {
                elim_unused_vars(r, pr);
            }
        }
        
        void reduce_quantifier(quantifier * q, expr_ref & r, proof_ref & pr) {   
            r = q;
            // Keep applying reduce_quantifier1 until r doesn't change anymore
            do {
                checkpoint();
                proof_ref curr_pr(m);
                q  = to_quantifier(r);
                reduce_quantifier1(q, r, curr_pr);
                if (m.proofs_enabled()) {
                    pr = m.mk_transitivity(pr, curr_pr);
                }
            } while (q != r && is_quantifier(r));
            
            m_new_exprs.reset();
        }
        
        void operator()(expr_ref_vector& r) {
            while (reduce_var_set(r)) ;
            m_new_exprs.reset();
        }
        
        ast_manager& get_manager() const { return m; }

        void set_cancel(bool f) {
            m_rewriter.set_cancel(f);
            m_cancel = f;
        }

    };
}; // namespace eq

// ------------------------------------------------------------
// basic destructive equality (and disequality) resolution for arrays.

namespace ar {
    class der {
        ast_manager&             m;
        array_util               a;
        is_variable_proc*        m_is_variable;
        ptr_vector<expr>         m_todo;
        expr_mark                m_visited;
        volatile bool            m_cancel;
        
        bool is_variable(expr * e) const {
            return (*m_is_variable)(e);
        }

        void mark_all(expr* e) {
            for_each_expr(*this, m_visited, e);
        }

        void mark_all(expr_ref_vector const& fmls, unsigned j) {
            for (unsigned i = 0; i < fmls.size(); ++i) {
                if (i != j) {
                    mark_all(fmls[i]);
                }
            }
        }

        /**
           Ex A. A[x] = t & Phi where x \not\in A, t. 
           =>
           Ex A. Phi[store(A,x,t)]
         */

        bool solve_select(expr_ref_vector& conjs, unsigned i, expr* e1, expr* e2) {
            if (a.is_select(e1)) {
                app* a1 = to_app(e1);
                expr* A = a1->get_arg(0);
                if (!is_variable(A)) {
                    return false;
                }
                m_visited.reset();
                for (unsigned j = 1; j < a1->get_num_args(); ++j) {
                    mark_all(a1->get_arg(j));
                }
                mark_all(e2);
                if (m_visited.is_marked(A)) {
                    return false;
                }
                ptr_vector<expr> args;
                args.push_back(A);
                args.append(a1->get_num_args()-1, a1->get_args()+1);
                args.push_back(e2);
                expr* B = a.mk_store(args.size(), args.c_ptr());
                expr_safe_replace rep(m);
                rep.insert(A, B);
                expr_ref tmp(m);
                for (unsigned j = 0; j < conjs.size(); ++j) {
                    if (i == j) {
                        conjs[j] = m.mk_true();
                    }
                    else {
                        rep(conjs[j].get(), tmp);
                        conjs[j] = tmp;
                    }
                }
                return true;
            }
            return false;
        }

        bool solve_select(expr_ref_vector& conjs, unsigned i, expr* e) {
            expr* e1, *e2;
            return 
                m.is_eq(e, e1, e2) &&
                (solve_select(conjs, i, e1, e2) ||
                 solve_select(conjs, i, e2, e1));
        }

        /**
           Ex x. A[x] != B[x] & Phi where x \not\in A, B, Phi
           =>
           A != B & Phi
         */
        bool solve_neq_select(expr_ref_vector& conjs, unsigned i, expr* e) {
            expr* e1, *a1, *a2;
            if (m.is_not(e, e1) && m.is_eq(e1, a1, a2)) {
                if (a.is_select(a1) && 
                    a.is_select(a2) && 
                    to_app(a1)->get_num_args() == to_app(a2)->get_num_args()) {
                    expr* e1 = to_app(a1)->get_arg(0);
                    expr* e2 = to_app(a2)->get_arg(0);
                    m_visited.reset();
                    mark_all(conjs, i);
                    mark_all(e1);
                    mark_all(e2);
                    for (unsigned j = 1; j < to_app(a1)->get_num_args(); ++j) {
                        expr* x = to_app(a1)->get_arg(j);
                        expr* y = to_app(a2)->get_arg(j);
                        if (!is_variable(x)) {
                            return false;
                        }
                        if (x != y) {
                            return false;
                        }
                        if (m_visited.is_marked(x)) {
                            return false;
                        }
                    }
                    conjs[i] = m.mk_not(m.mk_eq(e1, e2));
                    return true;
                }
            }
            return false;
        }

        void checkpoint() {
            cooperate("der");
            if (m_cancel)
                throw tactic_exception(TACTIC_CANCELED_MSG);
        }

    public:

        der(ast_manager& m): m(m), a(m), m_is_variable(0), m_cancel(false) {}

        void operator()(expr_ref_vector& fmls) {
            for (unsigned i = 0; i < fmls.size(); ++i) {
                checkpoint();
                solve_select(fmls, i, fmls[i].get());
                solve_neq_select(fmls, i, fmls[i].get());
            }
        }

        void operator()(expr* e) {}

        void set_is_variable_proc(is_variable_proc& proc) { m_is_variable = &proc;}

        void set_cancel(bool f) {
            m_cancel = f;
        }
        
    };
}; // namespace ar


// ------------------------------------------------------------
// fm_tactic adapted to eliminate designated de-Brujin indices.

namespace fm {
    typedef ptr_vector<app> clauses;
    typedef unsigned        var;
    typedef int             bvar;
    typedef int             literal;
    typedef svector<var>    var_vector;

    // Encode the constraint
    // lits \/ ( as[0]*xs[0] + ... + as[num_vars-1]*xs[num_vars-1] <= c
    // if strict is true, then <= is <.
    struct constraint {
        static unsigned get_obj_size(unsigned num_lits, unsigned num_vars) {
            return sizeof(constraint) + num_lits*sizeof(literal) + num_vars*(sizeof(var) + sizeof(rational));
        }
        unsigned           m_id;
        unsigned           m_num_lits:29;
        unsigned           m_strict:1;
        unsigned           m_dead:1;
        unsigned           m_mark:1;
        unsigned           m_num_vars;
        literal *          m_lits;
        var *              m_xs;
        rational  *        m_as;
        rational           m_c;
        expr_dependency *  m_dep;
        ~constraint() {
            rational * it  = m_as;
            rational * end = it + m_num_vars;
            for (; it != end; ++it)
                it->~rational();
        }
        
        unsigned hash() const { return hash_u(m_id); }
    };
    
    typedef ptr_vector<constraint> constraints;
    
    class constraint_set {
        unsigned_vector m_id2pos; 
        constraints     m_set;
    public:
        typedef constraints::const_iterator iterator;
        
        bool contains(constraint const & c) const { 
            if (c.m_id >= m_id2pos.size()) 
                return false; 
            return m_id2pos[c.m_id] != UINT_MAX; 
        }
        
        bool empty() const { return m_set.empty(); }
        unsigned size() const { return m_set.size(); }
        
        void insert(constraint & c) {
            unsigned id  = c.m_id;
            m_id2pos.reserve(id+1, UINT_MAX);
            if (m_id2pos[id] != UINT_MAX)
                return; // already in the set
            unsigned pos = m_set.size();
            m_id2pos[id] = pos;
            m_set.push_back(&c);
        }
        
        void erase(constraint & c) {
            unsigned id = c.m_id;
            if (id >= m_id2pos.size())
                return;
            unsigned pos = m_id2pos[id];
            if (pos == UINT_MAX)
                return;
            m_id2pos[id] = UINT_MAX;
            unsigned last_pos = m_set.size() - 1;
            if (pos != last_pos) {
                constraint * last_c = m_set[last_pos];
                m_set[pos] = last_c; 
                m_id2pos[last_c->m_id] = pos;
            }
            m_set.pop_back();
        }
        
        constraint & erase() {
            SASSERT(!empty());
            constraint & c = *m_set.back(); 
            m_id2pos[c.m_id] = UINT_MAX;
            m_set.pop_back();
            return c;
        }
        
        void reset() { m_id2pos.reset(); m_set.reset(); }
        void finalize() { m_id2pos.finalize(); m_set.finalize(); }
        
        iterator begin() const { return m_set.begin(); }
        iterator end() const { return m_set.end(); }
    };
    
    class fm {
        ast_manager &            m;
        is_variable_proc*        m_is_variable;
        small_object_allocator   m_allocator;
        arith_util               m_util;
        constraints              m_constraints;
        expr_ref_vector          m_bvar2expr;
        char_vector              m_bvar2sign;
        obj_map<expr, bvar>      m_expr2bvar;
        char_vector              m_is_int;
        char_vector              m_forbidden;
        expr_ref_vector          m_var2expr;
        obj_map<expr, var>       m_expr2var;
        unsigned_vector          m_var2pos;
        vector<constraints>      m_lowers;
        vector<constraints>      m_uppers;
        uint_set                 m_forbidden_set; // variables that cannot be eliminated because occur in non OCC ineq part
        expr_ref_vector          m_new_fmls;
        volatile bool            m_cancel;
        id_gen                   m_id_gen;
        bool                     m_fm_real_only;
        unsigned                 m_fm_limit;
        unsigned                 m_fm_cutoff1;
        unsigned                 m_fm_cutoff2;
        unsigned                 m_fm_extra;
        bool                     m_fm_occ;
        unsigned                 m_counter;
        bool                     m_inconsistent;
        expr_dependency_ref      m_inconsistent_core;
        constraint_set           m_sub_todo;
        
        // ---------------------------
        //
        // OCC clause recognizer
        //
        // ---------------------------
        
        bool is_literal(expr * t) const {
            expr * atom;
            return is_uninterp_const(t) || (m.is_not(t, atom) && is_uninterp_const(atom));
        }
        
        bool is_constraint(expr * t) const {
            return !is_literal(t);
        }
        
        bool is_var(expr * t, expr * & x) const {
            
            if ((*m_is_variable)(t)) {
                x = t;
                return true;
            }
            else if (m_util.is_to_real(t) && (*m_is_variable)(to_app(t)->get_arg(0))) {
                x = to_app(t)->get_arg(0);
                return true;
            }
            return false;
        }
        
        bool is_var(expr * t) const {
            expr * x;
            return is_var(t, x);
        }
        
        bool is_linear_mon_core(expr * t, expr * & x) const {
            expr * c;
            if (m_util.is_mul(t, c, x) && m_util.is_numeral(c) && is_var(x, x))
                return true;
            return is_var(t, x);
        }
        
        bool is_linear_mon(expr * t) const {
            expr * x;
            return is_linear_mon_core(t, x);
        }
        
        bool is_linear_pol(expr * t) const {
            unsigned       num_mons;
            expr * const * mons;
            if (m_util.is_add(t)) {
                num_mons = to_app(t)->get_num_args();
                mons     = to_app(t)->get_args();
            }
            else {
                num_mons = 1;
                mons     = &t;
            }
            
            expr_fast_mark2 visited;
            bool all_forbidden = true;
            for (unsigned i = 0; i < num_mons; i++) {
                expr * x;
                if (!is_linear_mon_core(mons[i], x))
                    return false;
                if (visited.is_marked(x))
                    return false; // duplicates are not supported... must simplify first
                visited.mark(x);
                SASSERT(::is_var(x));
                if (!m_forbidden_set.contains(::to_var(x)->get_idx()) && (!m_fm_real_only || !m_util.is_int(x)))
                    all_forbidden = false;
            }
            return !all_forbidden;
        }
        
        bool is_linear_ineq(expr * t) const {
            bool result = false;
            m.is_not(t, t);
            expr * lhs, * rhs;
            if (m_util.is_le(t, lhs, rhs) || m_util.is_ge(t, lhs, rhs)) {
                result = m_util.is_numeral(rhs) && is_linear_pol(lhs);
            }
            TRACE("qe_lite", tout << mk_pp(t, m) << " " << (result?"true":"false") << "\n";);

            return result;
        }
        
        bool is_occ(expr * t) {
            if (m_fm_occ && m.is_or(t)) {
                unsigned num = to_app(t)->get_num_args();
                bool found = false;
                for (unsigned i = 0; i < num; i++) {
                    expr * l = to_app(t)->get_arg(i);
                    if (is_literal(l)) {
                        continue;
                    }
                    else if (is_linear_ineq(l)) {
                        if (found)
                            return false;
                        found = true;
                    }
                    else {
                        return false;
                    }
                }
                return found;
            }
            return is_linear_ineq(t);
        }
        
        // ---------------------------
        //
        // Memory mng
        //
        // ---------------------------
        void del_constraint(constraint * c) {
            m.dec_ref(c->m_dep);
            m_sub_todo.erase(*c);
            m_id_gen.recycle(c->m_id);
            c->~constraint();
            unsigned sz = constraint::get_obj_size(c->m_num_lits, c->m_num_vars);
            m_allocator.deallocate(sz, c);
        }

        void del_constraints(unsigned sz, constraint * const * cs) {
            for (unsigned i = 0; i < sz; i++)
                del_constraint(cs[i]);
        }
        
        void reset_constraints() {
            del_constraints(m_constraints.size(), m_constraints.c_ptr());
            m_constraints.reset();
        }
        
        constraint * mk_constraint(unsigned num_lits, literal * lits, unsigned num_vars, var * xs, rational * as, rational & c, bool strict,
                                   expr_dependency * dep) {
            unsigned sz         = constraint::get_obj_size(num_lits, num_vars);
            char * mem          = static_cast<char*>(m_allocator.allocate(sz));
            char * mem_as       = mem + sizeof(constraint);
            char * mem_lits     = mem_as + sizeof(rational)*num_vars;
            char * mem_xs       = mem_lits + sizeof(literal)*num_lits;
            constraint * cnstr  = new (mem) constraint();
            cnstr->m_id         = m_id_gen.mk();
            cnstr->m_num_lits   = num_lits;
            cnstr->m_dead       = false;
            cnstr->m_mark       = false;
            cnstr->m_strict     = strict;
            cnstr->m_num_vars   = num_vars;
            cnstr->m_lits       = reinterpret_cast<literal*>(mem_lits);
            for (unsigned i = 0; i < num_lits; i++)
                cnstr->m_lits[i] = lits[i];
            cnstr->m_xs         = reinterpret_cast<var*>(mem_xs);
            cnstr->m_as         = reinterpret_cast<rational*>(mem_as);
            for (unsigned i = 0; i < num_vars; i++) {
                TRACE("qe_lite", tout << "xs[" << i << "]: " << xs[i] << "\n";);
                cnstr->m_xs[i] = xs[i];
                new (cnstr->m_as + i) rational(as[i]);
            }
            cnstr->m_c = c;
            DEBUG_CODE({
                for (unsigned i = 0; i < num_vars; i++) {
                    SASSERT(cnstr->m_xs[i] == xs[i]);
                    SASSERT(cnstr->m_as[i] == as[i]);
                }
            });
            cnstr->m_dep = dep;
            m.inc_ref(dep);
            return cnstr;
        }
        
        // ---------------------------
        //
        // Util
        //
        // ---------------------------
        
        unsigned num_vars() const { return m_is_int.size(); }
        
        // multiply as and c, by the lcm of their denominators
        void mk_int(unsigned num, rational * as, rational & c) {
            rational l = denominator(c);
            for (unsigned i = 0; i < num; i++)
                l = lcm(l, denominator(as[i]));
            if (l.is_one())
                return;
            c *= l;
            SASSERT(c.is_int());
            for (unsigned i = 0; i < num; i++) {
                as[i] *= l;
                SASSERT(as[i].is_int());
            }
        }
        
        void normalize_coeffs(constraint & c) {
            if (c.m_num_vars == 0)
                return;
            // compute gcd of all coefficients
            rational g = c.m_c;
            if (g.is_neg())
                g.neg();
            for (unsigned i = 0; i < c.m_num_vars; i++) {
                if (g.is_one())
                    break;
                if (c.m_as[i].is_pos())
                    g = gcd(c.m_as[i], g);
                else
                    g = gcd(-c.m_as[i], g);
            }
            if (g.is_one())
                return;
            c.m_c /= g;
            for (unsigned i = 0; i < c.m_num_vars; i++)
                c.m_as[i] /= g;
        }
        
        void display(std::ostream & out, constraint const & c) const {
            for (unsigned i = 0; i < c.m_num_lits; i++) {
                literal l = c.m_lits[i];
                if (sign(l))
                    out << "~";
                bvar p    = lit2bvar(l);
                out << mk_ismt2_pp(m_bvar2expr[p], m);
                out << " ";
            }
            out << "(";
            if (c.m_num_vars == 0)
                out << "0";
            for (unsigned i = 0; i < c.m_num_vars; i++) {
                if (i > 0)
                    out << " + ";
                if (!c.m_as[i].is_one())
                    out << c.m_as[i] << "*";
                out << mk_ismt2_pp(m_var2expr.get(c.m_xs[i]), m);
            }
            if (c.m_strict)
                out << " < ";
            else
                out << " <= ";
            out << c.m_c;
            out << ")";
        }
        
        /**
           \brief Return true if c1 subsumes c2
       
           c1 subsumes c2 If
           1) All literals of c1 are literals of c2
           2) polynomial of c1 == polynomial of c2
           3) c1.m_c <= c2.m_c
        */
        bool subsumes(constraint const & c1, constraint const & c2) {
            if (&c1 == &c2)
                return false;
            // quick checks first
            if (c1.m_num_lits > c2.m_num_lits)
                return false;
            if (c1.m_num_vars != c2.m_num_vars)
                return false;
            if (c1.m_c > c2.m_c)
                return false;
            if (!c1.m_strict && c2.m_strict && c1.m_c == c2.m_c)
                return false;
            
            m_counter += c1.m_num_lits + c2.m_num_lits;
            
            for (unsigned i = 0; i < c1.m_num_vars; i++) {
                m_var2pos[c1.m_xs[i]] = i;
            }
            
            bool failed = false;
            for (unsigned i = 0; i < c2.m_num_vars; i++) {
                unsigned pos1 = m_var2pos[c2.m_xs[i]];
                if (pos1 == UINT_MAX || c1.m_as[pos1] != c2.m_as[i]) {
                    failed = true;
                    break;
                }
            }
            
            for (unsigned i = 0; i < c1.m_num_vars; i++) {
                m_var2pos[c1.m_xs[i]] = UINT_MAX;
            }
            
            if (failed)
                return false;
            
            for (unsigned i = 0; i < c2.m_num_lits; i++) {
                literal l = c2.m_lits[i];
                bvar b    = lit2bvar(l);
                SASSERT(m_bvar2sign[b] == 0);
                m_bvar2sign[b] = sign(l) ? -1 : 1;
            }
            
            for (unsigned i = 0; i < c1.m_num_lits; i++) {
                literal l = c1.m_lits[i];
                bvar b    = lit2bvar(l);
                char s    = sign(l) ? -1 : 1;
                if (m_bvar2sign[b] != s) {
                    failed = true;
                    break;
                }
            }
            
            for (unsigned i = 0; i < c2.m_num_lits; i++) {
                literal l = c2.m_lits[i];
                bvar b    = lit2bvar(l);
                m_bvar2sign[b] = 0;
            }
            
            if (failed)
                return false;
            
            return true;
        }
        
        void backward_subsumption(constraint const & c) {
            if (c.m_num_vars == 0)
                return;
            var      best       = UINT_MAX;
            unsigned best_sz    = UINT_MAX;
            bool     best_lower = false;
            for (unsigned i = 0; i < c.m_num_vars; i++) {
                var xi     = c.m_xs[i];
                if (is_forbidden(xi))
                    continue; // variable is not in the index
                bool neg_a = c.m_as[i].is_neg();
                constraints & cs = neg_a ? m_lowers[xi] : m_uppers[xi];
                if (cs.size() < best_sz) {
                    best       = xi;
                    best_sz    = cs.size();
                    best_lower = neg_a;
                }
            }
            if (best_sz == 0)
                return;
            if (best == UINT_MAX)
                return; // none of the c variables are in the index.
            constraints & cs = best_lower ? m_lowers[best] : m_uppers[best];
            m_counter += cs.size();
            constraints::iterator it  = cs.begin();
            constraints::iterator it2 = it;
            constraints::iterator end = cs.end();
            for (; it != end; ++it) {
                constraint * c2 = *it;
                if (c2->m_dead)
                    continue;
                if (subsumes(c, *c2)) {
                    TRACE("qe_lite", display(tout, c); tout << "\nsubsumed:\n"; display(tout, *c2); tout << "\n";);
                    c2->m_dead = true;
                    continue;
                }
                *it2 = *it;
                ++it2;
            }
            cs.set_end(it2);
        }
        
        void subsume() {
            while (!m_sub_todo.empty()) {
                constraint & c = m_sub_todo.erase();
                if (c.m_dead)
                    continue;
                backward_subsumption(c);
            }
        }

    public:
        
        // ---------------------------
        //
        // Initialization
        //
        // ---------------------------
        
        fm(ast_manager & _m):
            m(_m),
            m_is_variable(0),
            m_allocator("fm-elim"),
            m_util(m),
            m_bvar2expr(m),
            m_var2expr(m),
            m_new_fmls(m),
            m_inconsistent_core(m) {
            m_cancel = false;
            updt_params();
            m_counter = 0;
            m_inconsistent = false;
        }
        
        ~fm() {
            reset_constraints();
        }
        
        void updt_params() {
            m_fm_real_only   = false;
            m_fm_limit       = 5000000;
            m_fm_cutoff1     = 8;
            m_fm_cutoff2     = 256;
            m_fm_extra       = 0;
            m_fm_occ         = true;
        }
        
        void set_cancel(bool f) {
            m_cancel = f;
        }
    private:
        
        struct forbidden_proc {
            fm & m_owner;
            forbidden_proc(fm & o):m_owner(o) {}
            void operator()(::var * n) {
                if (m_owner.is_var(n) && m_owner.m.get_sort(n)->get_family_id() == m_owner.m_util.get_family_id()) {
                    m_owner.m_forbidden_set.insert(n->get_idx());
                }
            }
            void operator()(app * n) { }
            void operator()(quantifier * n) {}
        };
        
        void init_forbidden_set(expr_ref_vector const & g) {
            m_forbidden_set.reset();
            expr_fast_mark1 visited;
            forbidden_proc  proc(*this);
            unsigned sz = g.size();
            for (unsigned i = 0; i < sz; i++) {
                expr * f = g[i];
                if (is_occ(f))
                    continue;
                TRACE("qe_lite", tout << "not OCC:\n" << mk_ismt2_pp(f, m) << "\n";);
                quick_for_each_expr(proc, visited, f);
            }
        }
        
        void init(expr_ref_vector const & g) {
            m_sub_todo.reset();
            m_id_gen.reset();
            reset_constraints();
            m_bvar2expr.reset();
            m_bvar2sign.reset();
            m_bvar2expr.push_back(0); // bvar 0 is not used
            m_bvar2sign.push_back(0);
            m_expr2var.reset();
            m_is_int.reset();
            m_var2pos.reset();
            m_forbidden.reset();
            m_var2expr.reset();
            m_expr2var.reset();
            m_lowers.reset();
            m_uppers.reset();
            m_new_fmls.reset();
            m_counter = 0;
            m_inconsistent = false;
            m_inconsistent_core = 0;
            init_forbidden_set(g);
        }
        
        // ---------------------------
        //
        // Internal data-structures
        //
        // ---------------------------
        
        static bool sign(literal l) { return l < 0; }
        static bvar lit2bvar(literal l) { return l < 0 ? -l : l; }
        
        bool is_int(var x) const { 
            return m_is_int[x] != 0;
        }
        
        bool is_forbidden(var x) const {
            return m_forbidden[x] != 0;
        }
        
        bool all_int(constraint const & c) const {
            for (unsigned i = 0; i < c.m_num_vars; i++) {
                if (!is_int(c.m_xs[i]))
                    return false;
            }
            return true;
        }
        
        app * to_expr(constraint const & c) {
            expr * ineq;
            if (c.m_num_vars == 0) {
                // 0 <  k (for k > 0)  --> true
                // 0 <= 0 -- > true
                if (c.m_c.is_pos() || (!c.m_strict && c.m_c.is_zero()))
                    return m.mk_true();
                ineq = 0;
            }
            else {
                bool int_cnstr = all_int(c);
                ptr_buffer<expr> ms;
                for (unsigned i = 0; i < c.m_num_vars; i++) {
                    expr * x = m_var2expr.get(c.m_xs[i]);
                    if (!int_cnstr && is_int(c.m_xs[i]))
                        x = m_util.mk_to_real(x);
                    if (c.m_as[i].is_one())
                        ms.push_back(x);
                    else
                        ms.push_back(m_util.mk_mul(m_util.mk_numeral(c.m_as[i], int_cnstr), x));
                }
                expr * lhs;
                if (c.m_num_vars == 1)
                    lhs = ms[0];
                else
                    lhs = m_util.mk_add(ms.size(), ms.c_ptr());
                expr * rhs = m_util.mk_numeral(c.m_c, int_cnstr);
                if (c.m_strict) {
                    ineq = m.mk_not(m_util.mk_ge(lhs, rhs));
                }
                else {
                    ineq = m_util.mk_le(lhs, rhs);
                }
            }
            
            if (c.m_num_lits == 0) {
                if (ineq)
                    return to_app(ineq);
                else
                    return m.mk_false();
            }
            
            ptr_buffer<expr> lits;
            for (unsigned i = 0; i < c.m_num_lits; i++) {
                literal l = c.m_lits[i];
                if (sign(l))
                    lits.push_back(m.mk_not(m_bvar2expr.get(lit2bvar(l))));
                else 
                    lits.push_back(m_bvar2expr.get(lit2bvar(l)));
            }
            if (ineq)
                lits.push_back(ineq);
            if (lits.size() == 1)
                return to_app(lits[0]);
            else
                return m.mk_or(lits.size(), lits.c_ptr());
        }
        
        var mk_var(expr * t) {
            SASSERT(::is_var(t));
            SASSERT(m_util.is_int(t) || m_util.is_real(t));
            var x = m_var2expr.size();
            m_var2expr.push_back(t);
            bool is_int = m_util.is_int(t);
            m_is_int.push_back(is_int);
            m_var2pos.push_back(UINT_MAX);
            m_expr2var.insert(t, x);
            m_lowers.push_back(constraints());
            m_uppers.push_back(constraints());
            bool forbidden = m_forbidden_set.contains(::to_var(t)->get_idx()) || (m_fm_real_only && is_int);
            m_forbidden.push_back(forbidden);
            SASSERT(m_var2expr.size()  == m_is_int.size());
            SASSERT(m_lowers.size()    == m_is_int.size());
            SASSERT(m_uppers.size()    == m_is_int.size());
            SASSERT(m_forbidden.size() == m_is_int.size()); 
            SASSERT(m_var2pos.size()   == m_is_int.size());
            TRACE("qe_lite", tout << mk_pp(t,m) << " |-> " << x << " forbidden: " << forbidden << "\n";);
            return x;
        }
        
        bvar mk_bvar(expr * t) {
            SASSERT(is_uninterp_const(t));
            SASSERT(m.is_bool(t));
            bvar p = m_bvar2expr.size();
            m_bvar2expr.push_back(t);
            m_bvar2sign.push_back(0);
            SASSERT(m_bvar2expr.size() == m_bvar2sign.size());
            m_expr2bvar.insert(t, p);
            SASSERT(p > 0);
            return p;
        }
        
        var to_var(expr * t) {
            var x;
            if (!m_expr2var.find(t, x))
                x = mk_var(t);
            SASSERT(m_expr2var.contains(t));
            SASSERT(m_var2expr.get(x) == t);
            TRACE("qe_lite", tout << mk_ismt2_pp(t, m) << " --> " << x << "\n";);
            return x;
        }
        
        bvar to_bvar(expr * t) {
            bvar p;
            if (m_expr2bvar.find(t, p))
                return p;
            return mk_bvar(t);
        }
        
        literal to_literal(expr * t) {
            if (m.is_not(t, t))
                return -to_bvar(t); 
            else
                return to_bvar(t);
        }
        
        
        void add_constraint(expr * f, expr_dependency * dep) {
            TRACE("qe_lite", tout << mk_pp(f, m) << "\n";);
            SASSERT(!m.is_or(f) || m_fm_occ);
            sbuffer<literal> lits;
            sbuffer<var>     xs;
            buffer<rational> as;
            rational         c;
            bool             strict;
            unsigned         num;
            expr * const *   args;
            if (m.is_or(f)) {
                num  = to_app(f)->get_num_args();
                args = to_app(f)->get_args();
            }
            else {
                num  = 1;
                args = &f;
            }

#if Z3DEBUG
            bool found_ineq = false;
#endif
            for (unsigned i = 0; i < num; i++) {
                expr * l = args[i];
                if (is_literal(l)) {
                    lits.push_back(to_literal(l));
                }
                else {
                    // found inequality
                    SASSERT(!found_ineq);
                    DEBUG_CODE(found_ineq = true;);
                    bool neg    = m.is_not(l, l);
                    SASSERT(m_util.is_le(l) || m_util.is_ge(l));
                    strict      = neg;
                    if (m_util.is_ge(l))
                        neg = !neg;
                    expr * lhs = to_app(l)->get_arg(0);
                    expr * rhs = to_app(l)->get_arg(1);
                    VERIFY (m_util.is_numeral(rhs, c));
                    if (neg)
                        c.neg();
                    unsigned num_mons;
                    expr * const * mons;
                    if (m_util.is_add(lhs)) {
                        num_mons = to_app(lhs)->get_num_args();
                        mons     = to_app(lhs)->get_args();
                    }
                    else {
                        num_mons = 1;
                        mons     = &lhs;
                    }
                    
                    bool all_int = true;
                    for (unsigned j = 0; j < num_mons; j++) {
                        expr * monomial = mons[j];
                        expr * a;
                        rational a_val;
                        expr * x;
                        if (m_util.is_mul(monomial, a, x)) {
                            VERIFY(m_util.is_numeral(a, a_val));
                        }
                        else {
                            x     = monomial;
                            a_val = rational(1);
                        }
                        if (neg)
                            a_val.neg();
                        VERIFY(is_var(x, x));
                        xs.push_back(to_var(x));
                        as.push_back(a_val);
                        if (!is_int(xs.back()))
                            all_int = false;
                    }
                    mk_int(as.size(), as.c_ptr(), c);
                    if (all_int && strict) {
                        strict = false;
                        c--;
                    }
                }
            }
            
            TRACE("qe_lite", tout << "before mk_constraint: "; for (unsigned i = 0; i < xs.size(); i++) tout << " " << xs[i]; tout << "\n";);
            
            constraint * new_c = mk_constraint(lits.size(),
                                               lits.c_ptr(),
                                               xs.size(),
                                               xs.c_ptr(),
                                               as.c_ptr(),
                                               c,
                                               strict,
                                               dep);
            
            TRACE("qe_lite", tout << "add_constraint: "; display(tout, *new_c); tout << "\n";);
            VERIFY(register_constraint(new_c));
        }
        
        bool is_false(constraint const & c) const {
            return c.m_num_lits == 0 && c.m_num_vars == 0 && (c.m_c.is_neg() || (c.m_strict && c.m_c.is_zero()));
        }
        
        bool register_constraint(constraint * c) {
            normalize_coeffs(*c);
            if (is_false(*c)) {
                del_constraint(c);
                m_inconsistent = true;
                TRACE("qe_lite", tout << "is false "; display(tout, *c); tout << "\n";);
                return false;
            }
            
            bool r = false;
            
            for (unsigned i = 0; i < c->m_num_vars; i++) {
                var x = c->m_xs[i];
                if (!is_forbidden(x)) {
                    r = true;
                    if (c->m_as[i].is_neg()) 
                        m_lowers[x].push_back(c);
                    else
                        m_uppers[x].push_back(c);
                }
            }
            
            if (r) {
                m_sub_todo.insert(*c);
                m_constraints.push_back(c);
                return true;
            }
            else {
                TRACE("qe_lite", tout << "all variables are forbidden "; display(tout, *c); tout << "\n";);
                m_new_fmls.push_back(to_expr(*c));
                del_constraint(c);
                return false;
            }
        }
        
        void init_use_list(expr_ref_vector const & g) {
            unsigned sz = g.size();
            for (unsigned i = 0; !m_inconsistent && i < sz; i++) {
                expr * f = g[i];
                if (is_occ(f))
                    add_constraint(f, 0);
                else
                    m_new_fmls.push_back(f);
            }
        }

        unsigned get_cost(var x) const {
            unsigned long long r = static_cast<unsigned long long>(m_lowers[x].size()) * static_cast<unsigned long long>(m_uppers[x].size());
            if (r > UINT_MAX)
                return UINT_MAX;
            return static_cast<unsigned>(r);
        }
        
        typedef std::pair<var, unsigned> x_cost;
    
        struct x_cost_lt {
            char_vector const m_is_int;
            x_cost_lt(char_vector & is_int):m_is_int(is_int) {}
            bool operator()(x_cost const & p1, x_cost const & p2) const { 
                // Integer variables with cost 0 can be eliminated even if they depend on real variables.
                // Cost 0 == no lower or no upper bound.
                if (p1.second == 0) {
                    if (p2.second > 0) return true;
                    return p1.first < p2.first;
                }
                if (p2.second == 0) return false;
                bool int1 = m_is_int[p1.first] != 0;
                bool int2 = m_is_int[p2.first] != 0;
                return (!int1 && int2) || (int1 == int2 && p1.second < p2.second); 
            }
        };

        void sort_candidates(var_vector & xs) {
            svector<x_cost> x_cost_vector;
            unsigned num = num_vars();
            for (var x = 0; x < num; x++) {
                if (!is_forbidden(x)) {
                    x_cost_vector.push_back(x_cost(x, get_cost(x)));
                }
            }
            // x_cost_lt is not a total order on variables
            std::stable_sort(x_cost_vector.begin(), x_cost_vector.end(), x_cost_lt(m_is_int));
            TRACE("qe_lite", 
                  svector<x_cost>::iterator it2  = x_cost_vector.begin();
                  svector<x_cost>::iterator end2 = x_cost_vector.end();
                  for (; it2 != end2; ++it2) {
                      tout << "(" << mk_ismt2_pp(m_var2expr.get(it2->first), m) << " " << it2->second << ") ";
                  }
                  tout << "\n";);
            svector<x_cost>::iterator it2  = x_cost_vector.begin();
            svector<x_cost>::iterator end2 = x_cost_vector.end();
            for (; it2 != end2; ++it2) {
                xs.push_back(it2->first);
            }
        }
        
        void cleanup_constraints(constraints & cs) {
            unsigned j = 0;
            unsigned sz = cs.size();
            for (unsigned i = 0; i < sz; i++) {
                constraint * c = cs[i];
                if (c->m_dead)
                    continue;
                cs[j] = c;
                j++;
            }
            cs.shrink(j);
        }
    
        // Set all_int = true if all variables in c are int.
        // Set unit_coeff = true if the coefficient of x in c is 1 or -1.
        // If all_int = false, then unit_coeff may not be set.
        void analyze(constraint const & c, var x, bool & all_int, bool & unit_coeff) const {
            all_int    = true;
            unit_coeff = true;
            for (unsigned i = 0; i < c.m_num_vars; i++) {
                if (!is_int(c.m_xs[i])) {
                    all_int = false;
                    return;
                }
                if (c.m_xs[i] == x) {
                    unit_coeff = (c.m_as[i].is_one() || c.m_as[i].is_minus_one());
                }
            }
        }

        void analyze(constraints const & cs, var x, bool & all_int, bool & unit_coeff) const {
            all_int    = true;
            unit_coeff = true;
            constraints::const_iterator it  = cs.begin();
            constraints::const_iterator end = cs.end();
            for (; it != end; ++it) {
                bool curr_unit_coeff;
                analyze(*(*it), x, all_int, curr_unit_coeff);
                if (!all_int)
                    return;
                if (!curr_unit_coeff)
                    unit_coeff = false;
            }
        }
        
        // An integer variable x may be eliminated, if 
        //   1- All variables in the contraints it occur are integer.
        //   2- The coefficient of x in all lower bounds (or all upper bounds) is unit.
        bool can_eliminate(var x) const {
            if (!is_int(x))
                return true;
            bool all_int;
            bool l_unit, u_unit;
            analyze(m_lowers[x], x, all_int, l_unit);
            if (!all_int)
                return false;
            analyze(m_uppers[x], x, all_int, u_unit);
            return all_int && (l_unit || u_unit);
        }
        
        void copy_constraints(constraints const & s, clauses & t) {
            constraints::const_iterator it  = s.begin();
            constraints::const_iterator end = s.end();
            for (; it != end; ++it) {
                app * c = to_expr(*(*it));
                t.push_back(c);
            }
        }
        
        clauses tmp_clauses;
        void save_constraints(var x) {  }
        
        void mark_constraints_dead(constraints const & cs) {
            constraints::const_iterator it  = cs.begin();
            constraints::const_iterator end = cs.end();
            for (; it != end; ++it)
                (*it)->m_dead = true;
        }
        
        void mark_constraints_dead(var x) {
            save_constraints(x);
            mark_constraints_dead(m_lowers[x]);
            mark_constraints_dead(m_uppers[x]);
        }
        
        void get_coeff(constraint const & c, var x, rational & a) {
            for (unsigned i = 0; i < c.m_num_vars; i++) {
                if (c.m_xs[i] == x) {
                    a = c.m_as[i];
                    return;
                }
            }
            UNREACHABLE();
        }
        
        var_vector       new_xs;
        vector<rational> new_as;
        svector<literal> new_lits;
        
        constraint * resolve(constraint const & l, constraint const & u, var x) {
            m_counter += l.m_num_vars + u.m_num_vars + l.m_num_lits + u.m_num_lits;
            rational a, b;
            get_coeff(l, x, a);
            get_coeff(u, x, b);
            SASSERT(a.is_neg());
            SASSERT(b.is_pos());
            a.neg();
            
            SASSERT(!is_int(x) || a.is_one() || b.is_one());
            
            new_xs.reset();
            new_as.reset();
            rational         new_c = l.m_c*b + u.m_c*a;
            bool             new_strict = l.m_strict || u.m_strict;
            
            for (unsigned i = 0; i < l.m_num_vars; i++) {
                var xi = l.m_xs[i];
                if (xi == x)
                    continue;
                unsigned pos = new_xs.size();
                new_xs.push_back(xi);
                SASSERT(m_var2pos[xi] == UINT_MAX);
                m_var2pos[xi] = pos;
                new_as.push_back(l.m_as[i] * b);
                SASSERT(new_xs[m_var2pos[xi]] == xi);
                SASSERT(new_xs.size() == new_as.size());
            }
            
            for (unsigned i = 0; i < u.m_num_vars; i++) {
                var xi = u.m_xs[i];
                if (xi == x)
                    continue;
                unsigned pos = m_var2pos[xi];
                if (pos == UINT_MAX) {
                    new_xs.push_back(xi);
                    new_as.push_back(u.m_as[i] * a);
                }
                else {
                    new_as[pos] += u.m_as[i] * a;
                }
            }
            
            // remove zeros and check whether all variables are int
            bool all_int = true;
            unsigned sz = new_xs.size();
            unsigned j  = 0;
            for (unsigned i = 0; i < sz; i++) {
                if (new_as[i].is_zero())
                    continue;
                if (!is_int(new_xs[i]))
                    all_int = false;
                if (i != j) {
                    new_xs[j] = new_xs[i];
                    new_as[j] = new_as[i];
                }
                j++;
            }
            new_xs.shrink(j);
            new_as.shrink(j);
            
            if (all_int && new_strict) {
                new_strict = false;
                new_c --;
            }
            
            // reset m_var2pos
            for (unsigned i = 0; i < l.m_num_vars; i++) {
                m_var2pos[l.m_xs[i]] = UINT_MAX;
            }
            
            if (new_xs.empty() && (new_c.is_pos() || (!new_strict && new_c.is_zero()))) {
                // literal is true
                TRACE("qe_lite", tout << "resolution " << x << " consequent literal is always true: \n";
                      display(tout, l);
                      tout << "\n";
                      display(tout, u); tout << "\n";);
                return 0; // no constraint needs to be created.
            }
            
            new_lits.reset();
            for (unsigned i = 0; i < l.m_num_lits; i++) {
                literal lit = l.m_lits[i];
                bvar    p   = lit2bvar(lit);
                m_bvar2sign[p] = sign(lit) ? -1 : 1;
                new_lits.push_back(lit);
            }
            
            bool tautology = false;
            for (unsigned i = 0; i < u.m_num_lits && !tautology; i++) {
                literal lit = u.m_lits[i];
                bvar    p   = lit2bvar(lit);
                switch (m_bvar2sign[p]) {
                case 0:
                    new_lits.push_back(lit);
                break;
                case -1:
                    if (!sign(lit))
                        tautology = true;
                    break;
                case 1:
                    if (sign(lit))
                        tautology = true;
                    break;
                default:
                    UNREACHABLE();
                }
            }
            
            // reset m_bvar2sign
            for (unsigned i = 0; i < l.m_num_lits; i++) {
                literal lit = l.m_lits[i];
                bvar    p   = lit2bvar(lit);
                m_bvar2sign[p] = 0;
            }
            
            if (tautology) {
                TRACE("qe_lite", tout << "resolution " << x << " tautology: \n";
                      display(tout, l);
                      tout << "\n";
                      display(tout, u); tout << "\n";);
                return 0;
            }

            expr_dependency * new_dep = m.mk_join(l.m_dep, u.m_dep);
            
            if (new_lits.empty() && new_xs.empty() && (new_c.is_neg() || (new_strict && new_c.is_zero()))) {
                TRACE("qe_lite", tout << "resolution " << x << " inconsistent: \n";
                      display(tout, l);
                      tout << "\n";
                      display(tout, u); tout << "\n";);
                m_inconsistent      = true;
                m_inconsistent_core = new_dep;
                return 0;
            }
            
            constraint * new_cnstr = mk_constraint(new_lits.size(),
                                                   new_lits.c_ptr(),
                                                   new_xs.size(),
                                                   new_xs.c_ptr(),
                                                   new_as.c_ptr(),
                                                   new_c,
                                                   new_strict,
                                                   new_dep);

            TRACE("qe_lite", tout << "resolution " << x << "\n";
                  display(tout, l);
                  tout << "\n";
                  display(tout, u);
                  tout << "\n---->\n";
                  display(tout, *new_cnstr); 
                  tout << "\n";
                  tout << "new_dep: " << new_dep << "\n";);
            
            return new_cnstr;
        }
        
        ptr_vector<constraint> new_constraints;
        
        bool try_eliminate(var x) {
            constraints & l = m_lowers[x];
            constraints & u = m_uppers[x];
            cleanup_constraints(l);
            cleanup_constraints(u);
            
            if (l.empty() || u.empty()) {
                // easy case
                mark_constraints_dead(x);
                TRACE("qe_lite", tout << "variable was eliminated (trivial case)\n";);
                return true;
            }
            
            unsigned num_lowers = l.size();
            unsigned num_uppers = u.size();
            
            if (num_lowers > m_fm_cutoff1 && num_uppers > m_fm_cutoff1)
                return false;
            
            if (num_lowers * num_uppers > m_fm_cutoff2)
                return false;
            
            if (!can_eliminate(x))
                return false;
            
            m_counter += num_lowers * num_uppers;
            
            TRACE("qe_lite", tout << "eliminating " << mk_ismt2_pp(m_var2expr.get(x), m) << "\nlowers:\n";
                  display_constraints(tout, l); tout << "uppers:\n"; display_constraints(tout, u););
            
            unsigned num_old_cnstrs = num_uppers + num_lowers;
            unsigned limit          = num_old_cnstrs + m_fm_extra;
            unsigned num_new_cnstrs = 0;
            new_constraints.reset();
            for (unsigned i = 0; i < num_lowers; i++) {
                for (unsigned j = 0; j < num_uppers; j++) {
                    if (m_inconsistent || num_new_cnstrs > limit) {
                        TRACE("qe_lite", tout << "too many new constraints: " << num_new_cnstrs << "\n";);
                        del_constraints(new_constraints.size(), new_constraints.c_ptr());
                        return false;
                    }
                    constraint const & l_c = *(l[i]);
                    constraint const & u_c = *(u[j]);
                    constraint * new_c = resolve(l_c, u_c, x);
                    if (new_c != 0) {
                        num_new_cnstrs++;
                        new_constraints.push_back(new_c);
                    }
                }
            }
            
            mark_constraints_dead(x);
            
            unsigned sz = new_constraints.size();
            
            m_counter += sz;
            
            for (unsigned i = 0; i < sz; i++) {
                constraint * c = new_constraints[i];
                backward_subsumption(*c);
                register_constraint(c);
            }
            TRACE("qe_lite", tout << "variables was eliminated old: " << num_old_cnstrs << " new_constraints: " << sz << "\n";);
            return true;
        }
        
        void copy_remaining(vector<constraints> & v2cs) {
            vector<constraints>::iterator it  = v2cs.begin();
            vector<constraints>::iterator end = v2cs.end();
            for (; it != end; ++it) {
                constraints & cs = *it;
                constraints::iterator it2  = cs.begin();
                constraints::iterator end2 = cs.end();
                for (; it2 != end2; ++it2) {
                    constraint * c = *it2;
                    if (!c->m_dead) {
                        c->m_dead = true;
                        expr * new_f = to_expr(*c);
                        TRACE("qe_lite", tout << "asserting...\n" << mk_ismt2_pp(new_f, m) << "\nnew_dep: " << c->m_dep << "\n";);
                        m_new_fmls.push_back(new_f);
                    }
                }
            }
            v2cs.finalize();
        }
        
        // Copy remaining clauses to m_new_fmls
        void copy_remaining() {
            copy_remaining(m_uppers);
            copy_remaining(m_lowers);
        }
        
        void checkpoint() {
            cooperate("fm");
            if (m_cancel)
                throw tactic_exception(TACTIC_CANCELED_MSG);
        }
    public:

        void set_is_variable_proc(is_variable_proc& proc) { m_is_variable = &proc;}

        void operator()(expr_ref_vector& fmls) {
            init(fmls);
            init_use_list(fmls);
            if (m_inconsistent) {
                m_new_fmls.reset();
                m_new_fmls.push_back(m.mk_false());
            }
            else {
                TRACE("qe_lite", display(tout););
                
                subsume();
                var_vector candidates;
                sort_candidates(candidates);                
                unsigned eliminated = 0;                
                
                unsigned num = candidates.size();
                for (unsigned i = 0; i < num; i++) {
                    checkpoint();
                    if (m_counter > m_fm_limit)
                        break;
                    m_counter++;
                    if (try_eliminate(candidates[i]))
                        eliminated++;
                    if (m_inconsistent) {
                        m_new_fmls.reset();
                        m_new_fmls.push_back(m.mk_false());
                        break;
                    }
                }
                if (!m_inconsistent) {
                    copy_remaining();
                }
            }
            reset_constraints();
            fmls.reset();
            fmls.append(m_new_fmls);
        }        
        
        void display_constraints(std::ostream & out, constraints const & cs) const {
            constraints::const_iterator it  = cs.begin();
            constraints::const_iterator end = cs.end();
            for (; it != end; ++it) {
                out << "  ";
                display(out, *(*it));
                out << "\n";
            }
        }
        
        void display(std::ostream & out) const {
            unsigned num = num_vars();
            for (var x = 0; x < num; x++) {
                if (is_forbidden(x))
                    continue;
                out << mk_ismt2_pp(m_var2expr.get(x), m) << "\n";
                display_constraints(out, m_lowers[x]);
                display_constraints(out, m_uppers[x]);
            }
        }
    };

} // namespace fm

class qe_lite::impl {
public:
    struct elim_cfg : public default_rewriter_cfg {
        impl& m_imp;
        ast_manager& m;
    public:
        elim_cfg(impl& i): m_imp(i), m(i.m) {}
        
        bool reduce_quantifier(quantifier * q, 
                               expr * new_body, 
                               expr * const * new_patterns, 
                               expr * const * new_no_patterns,
                               expr_ref & result,
                               proof_ref & result_pr) {
            result = new_body;
            if (is_forall(q)) {
                result = m.mk_not(result);
            }
            uint_set indices;
            for (unsigned i = 0; i < q->get_num_decls(); ++i) {
                indices.insert(i);
            }
            m_imp(indices, true, result);          
            if (is_forall(q)) {
                result = m.mk_not(result);
            }
            result = m.update_quantifier(
                q, 
                q->get_num_patterns(), new_patterns, 
                q->get_num_no_patterns(), new_no_patterns, result);
            m_imp.m_rewriter(result);
            return true;
        }
    };

    class elim_star : public rewriter_tpl<elim_cfg> {
        elim_cfg m_cfg;
    public:
        elim_star(impl& i): 
            rewriter_tpl<elim_cfg>(i.m, false, m_cfg),
            m_cfg(i)
        {}
    };

private:
    ast_manager& m;
    eq::der      m_der;
    fm::fm       m_fm;
    ar::der      m_array_der;
    elim_star    m_elim_star;
    th_rewriter  m_rewriter;

    bool has_unique_non_ground(expr_ref_vector const& fmls, unsigned& index) {
        index = fmls.size();
        if (index <= 1) {
            return false;
        }
        for (unsigned i = 0; i < fmls.size(); ++i) {
            if (!is_ground(fmls[i])) {
                if (index != fmls.size()) {
                    return false;
                }
                index = i;
            }
        }
        return index < fmls.size();
    }

public:
    impl(ast_manager& m): 
        m(m), 
        m_der(m), 
        m_fm(m), 
        m_array_der(m), 
        m_elim_star(*this), 
        m_rewriter(m) {}
    
    void operator()(app_ref_vector& vars, expr_ref& fml) {
        if (vars.empty()) {
            return;
        }
        expr_ref tmp(fml);
        quantifier_ref q(m);
        proof_ref pr(m);     
        symbol qe_lite("QE");
        expr_abstract(m, 0, vars.size(), (expr*const*)vars.c_ptr(), fml, tmp);
        ptr_vector<sort> sorts;
        svector<symbol> names;
        for (unsigned i = 0; i < vars.size(); ++i) {
            sorts.push_back(m.get_sort(vars[i].get()));
            names.push_back(vars[i]->get_decl()->get_name());
        }
        q = m.mk_exists(vars.size(), sorts.c_ptr(), names.c_ptr(), tmp, 1, qe_lite);
        m_der.reduce_quantifier(q, tmp, pr);
        // assumes m_der just updates the quantifier and does not change things more.
        if (is_exists(tmp) && to_quantifier(tmp)->get_qid() == qe_lite) {
            used_vars used;
            tmp = to_quantifier(tmp)->get_expr();
            used.process(tmp);
            var_subst vs(m, true);
            vs(tmp, vars.size(), (expr*const*)vars.c_ptr(), fml);
            // collect set of variables that were used.
            unsigned j = 0;
            for (unsigned i = 0; i < vars.size(); ++i) {
                if (used.contains(vars.size()-i-1)) {
                    vars.set(j, vars.get(i));
                    ++j;
                }
            }
            vars.resize(j);            
        }        
        else {
            fml = tmp;
        }
    }    

    void operator()(expr_ref& fml, proof_ref& pr) {
        expr_ref tmp(m);
        m_elim_star(fml, tmp, pr);
        fml = tmp;
    }

    void operator()(uint_set const& index_set, bool index_of_bound, expr_ref& fml) {
        expr_ref_vector disjs(m);
        datalog::flatten_or(fml, disjs);
        for (unsigned i = 0; i < disjs.size(); ++i) {
            expr_ref_vector conjs(m);
            conjs.push_back(disjs[i].get());
            (*this)(index_set, index_of_bound, conjs);
            bool_rewriter(m).mk_and(conjs.size(), conjs.c_ptr(), fml);
            disjs[i] = fml;
        }
        bool_rewriter(m).mk_or(disjs.size(), disjs.c_ptr(), fml);
    }


    void operator()(uint_set const& index_set, bool index_of_bound, expr_ref_vector& fmls) {
        datalog::flatten_and(fmls);
        unsigned index;
        if (has_unique_non_ground(fmls, index)) {
            expr_ref fml(m);
            fml = fmls[index].get();
            (*this)(index_set, index_of_bound, fml);
            fmls[index] = fml;
            return;
        }
        TRACE("qe_lite", for (unsigned i = 0; i < fmls.size(); ++i) {
                tout << mk_pp(fmls[i].get(), m) << "\n";
            });
        IF_VERBOSE(3, for (unsigned i = 0; i < fmls.size(); ++i) {
                verbose_stream() << mk_pp(fmls[i].get(), m) << "\n";
            });
        is_variable_test is_var(index_set, index_of_bound);
        m_der.set_is_variable_proc(is_var);
        m_fm.set_is_variable_proc(is_var);
        m_array_der.set_is_variable_proc(is_var);
        m_der(fmls);
        m_fm(fmls);
        m_array_der(fmls);
        TRACE("qe_lite", for (unsigned i = 0; i < fmls.size(); ++i) tout << mk_pp(fmls[i].get(), m) << "\n";);
    }

    void set_cancel(bool f) {
        m_der.set_cancel(f);
        m_array_der.set_cancel(f);
        m_fm.set_cancel(f);
        m_elim_star.set_cancel(f);
        m_rewriter.set_cancel(f);
    }

};

qe_lite::qe_lite(ast_manager& m) {
    m_impl = alloc(impl, m);
}

qe_lite::~qe_lite() {
    dealloc(m_impl);
}

void qe_lite::operator()(app_ref_vector& vars, expr_ref& fml) {
    (*m_impl)(vars, fml);
}

void qe_lite::set_cancel(bool f) {
    m_impl->set_cancel(f);
}

void qe_lite::operator()(expr_ref& fml, proof_ref& pr) {
    (*m_impl)(fml, pr);
}

void qe_lite::operator()(uint_set const& index_set, bool index_of_bound, expr_ref& fml) {
    (*m_impl)(index_set, index_of_bound, fml);
}

void qe_lite::operator()(uint_set const& index_set, bool index_of_bound, expr_ref_vector& fmls) {
    (*m_impl)(index_set, index_of_bound, fmls);
}

class qe_lite_tactic : public tactic {
    
    struct imp {
        ast_manager&             m;
        qe_lite                  m_qe;
        volatile bool            m_cancel;

        imp(ast_manager& m, params_ref const& p): 
            m(m),
            m_qe(m),
            m_cancel(false)
        {}

        void set_cancel(bool f) {
            m_cancel = f;
            m_qe.set_cancel(f);
        }

        void checkpoint() {
            if (m_cancel)
                throw tactic_exception(TACTIC_CANCELED_MSG);
            cooperate("qe-lite");
        }
        
        void operator()(goal_ref const & g, 
                        goal_ref_buffer & result, 
                        model_converter_ref & mc, 
                        proof_converter_ref & pc,
                        expr_dependency_ref & core) {
            SASSERT(g->is_well_sorted());
            mc = 0; pc = 0; core = 0;
            tactic_report report("qe-lite", *g);
            proof_ref new_pr(m);
            expr_ref new_f(m);
            bool produce_proofs = g->proofs_enabled();

            unsigned sz = g->size();
            for (unsigned i = 0; i < sz; i++) {
                checkpoint();
                if (g->inconsistent())
                    break;
                expr * f = g->form(i);
                if (!has_quantifiers(f))
                    continue;
                new_f = f;
                m_qe(new_f, new_pr);
                if (produce_proofs) {
                    expr* fact = m.get_fact(new_pr);
                    if (to_app(fact)->get_arg(0) != to_app(fact)->get_arg(1)) {
                        new_pr = m.mk_modus_ponens(g->pr(i), new_pr);                        
                    }
                    else {
                        new_pr = g->pr(i);
                    }
                }
                g->update(i, new_f, new_pr, g->dep(i));                
            }
            g->inc_depth();
            result.push_back(g.get());
            TRACE("qe", g->display(tout););
            SASSERT(g->is_well_sorted());
        }

    };
    
    params_ref m_params;
    imp *      m_imp;

public:
    qe_lite_tactic(ast_manager & m, params_ref const & p):
        m_params(p) {
        m_imp = alloc(imp, m, p);
    }
        
    virtual ~qe_lite_tactic() {
        dealloc(m_imp);
    }

    virtual tactic * translate(ast_manager & m) {
        return alloc(qe_lite_tactic, m, m_params);
    }

    virtual void updt_params(params_ref const & p) {
        m_params = p;
        // m_imp->updt_params(p);
    }

   
    virtual void collect_param_descrs(param_descrs & r) {
        // m_imp->collect_param_descrs(r);
    }
    
    virtual void operator()(goal_ref const & in, 
                            goal_ref_buffer & result, 
                            model_converter_ref & mc, 
                            proof_converter_ref & pc,
                            expr_dependency_ref & core) {
        (*m_imp)(in, result, mc, pc, core);
    }

    
    virtual void collect_statistics(statistics & st) const {
        // m_imp->collect_statistics(st);
    }

    virtual void reset_statistics() {
        // m_imp->reset_statistics();
    }

    
    virtual void cleanup() {
        ast_manager & m = m_imp->m;
        imp * d = m_imp;
        #pragma omp critical (tactic_cancel)
        {
            m_imp = 0;
        }
        dealloc(d);
        d = alloc(imp, m, m_params);
        #pragma omp critical (tactic_cancel)
        {
            m_imp = d;
        }
    }
    
};

tactic * mk_qe_lite_tactic(ast_manager & m, params_ref const & p) {
    return alloc(qe_lite_tactic, m, p);
}

template class rewriter_tpl<qe_lite::impl::elim_cfg>;
