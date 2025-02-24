/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    clp_context.cpp

Abstract:

    Bounded CLP (symbolic simulation using Z3) context.

Author:

    Nikolaj Bjorner (nbjorner) 2013-04-26

Revision History:

--*/

#include "clp_context.h"
#include "dl_context.h"
#include "unifier.h"
#include "var_subst.h"
#include "substitution.h"

namespace datalog {

    class clp::imp {
        struct stats {
            stats() { reset(); }
            void reset() { memset(this, 0, sizeof(*this)); }
            unsigned m_num_unfold;
            unsigned m_num_no_unfold;
            unsigned m_num_subsumed;
        };

        context&               m_ctx;
        ast_manager&           m;
        rule_manager&          rm;
        smt_params             m_fparams;
        smt::kernel            m_solver;
        var_subst              m_var_subst;
        expr_ref_vector        m_ground;
        app_ref_vector         m_goals;
        volatile bool          m_cancel;
        stats                  m_stats;
    public:
        imp(context& ctx):
            m_ctx(ctx), 
            m(ctx.get_manager()),
            rm(ctx.get_rule_manager()),
            m_solver(m, m_fparams),      // TBD: can be replaced by efficient BV solver.
            m_var_subst(m, false),
            m_ground(m),
            m_goals(m),
            m_cancel(false)
        {
            // m_fparams.m_relevancy_lvl = 0;
            m_fparams.m_mbqi = false;
            m_fparams.m_soft_timeout = 1000;
        }

        ~imp() {}        

        lbool query(expr* query) {
            m_ctx.ensure_opened();
            m_solver.reset();
            m_goals.reset();
            rm.mk_query(query, m_ctx.get_rules());
            m_ctx.apply_default_transformation();
            func_decl *head_decl = m_ctx.get_rules().get_output_predicate();

            expr_ref head(m_ctx.get_rules().get_predicate_rules(head_decl)[0]->get_head(), m);
            ground(head);
            m_goals.push_back(to_app(head));
            return search(20, 0);
        }
    
        void cancel() {
            m_cancel = true;
            m_solver.cancel();
        }
        
        void cleanup() {
            m_cancel = false;
            m_goals.reset();
            m_solver.reset_cancel();
        }

        void reset_statistics() {
            m_stats.reset();
        }

        void collect_statistics(statistics& st) const {
            //st.update("tab.num_unfold", m_stats.m_num_unfold);
            //st.update("tab.num_unfold_fail", m_stats.m_num_no_unfold);
            //st.update("tab.num_subsumed", m_stats.m_num_subsumed);
        }

        void display_certificate(std::ostream& out) const {
            expr_ref ans = get_answer();
            out << mk_pp(ans, m) << "\n";    

        }

        expr_ref get_answer() const {
            return expr_ref(m.mk_true(), m);
        }

    private:

        void reset_ground() {
            m_ground.reset();
        }
        
        void ground(expr_ref& e) {
            ptr_vector<sort> sorts;
            get_free_vars(e, sorts);
            if (m_ground.size() < sorts.size()) {
                m_ground.resize(sorts.size());
            }
            for (unsigned i = 0; i < sorts.size(); ++i) {
                if (sorts[i] && !m_ground[i].get()) {
                    m_ground[i] = m.mk_fresh_const("c",sorts[i]);
                }
            }
            m_var_subst(e, m_ground.size(), m_ground.c_ptr(), e);
        }

        static bool rule_sort_fn(const rule *r1, const rule *r2) {
            return r1->get_uninterpreted_tail_size() < r2->get_uninterpreted_tail_size();
        }

        lbool search(unsigned depth, unsigned index) {
            if (index == m_goals.size()) {
                return l_true;
            }
            if (depth == 0) {
                return l_undef;
            }
            IF_VERBOSE(1, verbose_stream() << "search " << depth << " " << index << "\n";);
            unsigned num_goals = m_goals.size();
            app* head = m_goals[index].get();

            rule_vector rules(m_ctx.get_rules().get_predicate_rules(head->get_decl()));
            std::stable_sort(rules.begin(), rules.end(), rule_sort_fn);

            lbool status = l_false;
            for (unsigned i = 0; i < rules.size(); ++i) {
                rule* r = rules[i];
                m_solver.push();
                reset_ground();
                expr_ref tmp(m);
                tmp = r->get_head();
                IF_VERBOSE(2, verbose_stream() << index << " " << mk_pp(tmp, m) << "\n";);
                ground(tmp);
                for (unsigned j = 0; j < head->get_num_args(); ++j) {
                    expr_ref eq(m);
                    eq = m.mk_eq(head->get_arg(j), to_app(tmp)->get_arg(j));
                    m_solver.assert_expr(eq);
                }
                for (unsigned j = r->get_uninterpreted_tail_size(); j < r->get_tail_size(); ++j) {
                    tmp = r->get_tail(j);
                    ground(tmp);
                    m_solver.assert_expr(tmp);
                }
                lbool is_sat = m_solver.check();
                switch (is_sat) {
                case l_false:
                    break;
                case l_true:
                    if (depth == 1 && (index+1 > m_goals.size() || r->get_uninterpreted_tail_size() > 0)) {
                        status = l_undef;
                        break;
                    }
                    for (unsigned j = 0; j < r->get_uninterpreted_tail_size(); ++j) {
                        tmp = r->get_tail(j);
                        ground(tmp);
                        m_goals.push_back(to_app(tmp));
                    }
                    switch(search(depth-1, index+1)) {
                    case l_undef:
                        status = l_undef;
                        // fallthrough
                    case l_false:
                        m_goals.resize(num_goals);   
                        break;
                    case l_true:
                        return l_true;
                    }
                    break;
                case l_undef:
                    status = l_undef;
                    throw default_exception("undef");
                }
                m_solver.pop(1);
            }
            return status;
        }
    
    
        proof_ref get_proof() const {
            return proof_ref(0, m);
        }
    };
    
    clp::clp(context& ctx):
        m_imp(alloc(imp, ctx)) {        
    }
    clp::~clp() {
        dealloc(m_imp);
    }    
    lbool clp::query(expr* query) {
        return m_imp->query(query);
    }
    void clp::cancel() {
        m_imp->cancel();
    }
    void clp::cleanup() {
        m_imp->cleanup();
    }
    void clp::reset_statistics() {
        m_imp->reset_statistics();
    }
    void clp::collect_statistics(statistics& st) const {
        m_imp->collect_statistics(st);
    }
    void clp::display_certificate(std::ostream& out) const {
        m_imp->display_certificate(out);
    }
    expr_ref clp::get_answer() {
        return m_imp->get_answer();
    }

};
