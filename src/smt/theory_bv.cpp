/*++
Copyright (c) 2006 Microsoft Corporation

Module Name:

    theory_bv.cpp

Abstract:

    <abstract>

Author:

    Leonardo de Moura (leonardo) 2008-06-06.

Revision History:

--*/
#include"smt_context.h"
#include"theory_bv.h"
#include"ast_ll_pp.h"
#include"ast_pp.h"
#include"smt_model_generator.h"
#include"stats.h"


namespace smt {

    void theory_bv::init(context * ctx) {
        theory::init(ctx);
        m_simplifier    = &(ctx->get_simplifier());
    }

    theory_var theory_bv::mk_var(enode * n) {
        theory_var r  = theory::mk_var(n);
        m_find.mk_var();
        m_bits.push_back(literal_vector());
        m_wpos.push_back(0);
        m_zero_one_bits.push_back(zero_one_bits());
        get_context().attach_th_var(n, this, r);
        return r;
    }

    app * theory_bv::mk_bit2bool(app * bv, unsigned idx) {
        parameter p(idx);
        expr * args[1] = {bv};
        return get_manager().mk_app(get_id(), OP_BIT2BOOL, 1, &p, 1, args);
    }
    
    void theory_bv::mk_bits(theory_var v) {
        enode * n             = get_enode(v);
        app * owner           = n->get_owner();
        unsigned bv_size      = get_bv_size(n);
        context & ctx         = get_context();
        literal_vector & bits = m_bits[v];
        for (unsigned i = 0; i < bv_size; i++) {
            app * bit  = mk_bit2bool(owner, i);
            ctx.internalize(bit, true);
            bool_var b = ctx.get_bool_var(bit);
            bits.push_back(literal(b));
        }
    }

    class mk_atom_trail : public trail<theory_bv> {
        bool_var m_var;
    public:
        mk_atom_trail(bool_var v):m_var(v) {}
        virtual void undo(theory_bv & th) {
            theory_bv::atom * a = th.get_bv2a(m_var);
            a->~atom();
            th.erase_bv2a(m_var);
        }
    };

    void theory_bv::mk_bit2bool(app * n) {
        context & ctx    = get_context();
        SASSERT(!ctx.b_internalized(n));
        if (!ctx.e_internalized(n->get_arg(0))) {
            // This may happen if bit2bool(x) is in a conflict
            // clause that is being reinitialized, and x was not reinitialized
            // yet.
            // So, we internalize x (i.e., n->get_arg(0))
            expr * first_arg = n->get_arg(0);
            ctx.internalize(first_arg, false);
            SASSERT(ctx.e_internalized(first_arg));
            // In most cases, when x is internalized, its bits are created.
            // They are created because x is a bit-vector operation or apply_sort_cnstr is invoked.
            // However, there is an exception. The method apply_sort_cnstr is not invoked for ite-terms.
            // So, I execute get_var on the enode attached to first_arg. 
            // This will force a theory variable to be created if it does not already exist.
            // This will also force the creation of all bits for x.
            enode * first_arg_enode = ctx.get_enode(first_arg);
            get_var(first_arg_enode);
            SASSERT(ctx.b_internalized(n));
        }
        else {
            enode * arg      = ctx.get_enode(n->get_arg(0));
            // The argument was already internalized, but it may not have a theory variable associated with it.
            // For example, for ite-terms the method apply_sort_cnstr is not invoked.
            // See comment in the then-branch.
            theory_var v_arg = arg->get_th_var(get_id());
            if (v_arg == null_theory_var) {
                // The method get_var will create a theory variable for arg. 
                // As a side-effect the bits for arg will also be created.
                get_var(arg);
                SASSERT(ctx.b_internalized(n));
            }
            else {
                SASSERT(v_arg != null_theory_var);
                bool_var bv      = ctx.mk_bool_var(n);
                ctx.set_var_theory(bv, get_id());
                bit_atom * a     = new (get_region()) bit_atom();
                insert_bv2a(bv, a);
                m_trail_stack.push(mk_atom_trail(bv));
                unsigned idx     = n->get_decl()->get_parameter(0).get_int();
                SASSERT(a->m_occs == 0);
                a->m_occs = new (get_region()) var_pos_occ(v_arg, idx);
            }
        }
    }

    void theory_bv::process_args(app * n) {
        context & ctx     = get_context();
        unsigned num_args = n->get_num_args();
        for (unsigned i = 0; i < num_args; i++)
            ctx.internalize(n->get_arg(i), false);
    }

    enode * theory_bv::mk_enode(app * n) {
        context & ctx = get_context();
        enode * e;
        if (ctx.e_internalized(n)) {
            e = ctx.get_enode(n);
        }
        else {
            e = ctx.mk_enode(n, !m_params.m_bv_reflect, false, m_params.m_bv_cc);
            mk_var(e);
        }
        SASSERT(e->get_th_var(get_id()) != null_theory_var);
        return e;
    }

    theory_var theory_bv::get_var(enode * n) {
        theory_var v = n->get_th_var(get_id());
        if (v == null_theory_var) {
            v = mk_var(n);
            mk_bits(v);
        }
        return v;
    }

    enode * theory_bv::get_arg(enode * n, unsigned idx) {
        if (m_params.m_bv_reflect) {
            return n->get_arg(idx);
        }
        else {
            context & ctx = get_context();
            app * arg     = to_app(n->get_owner()->get_arg(idx));
            SASSERT(ctx.e_internalized(arg));
            return ctx.get_enode(arg);
        }
    }
    
    inline theory_var theory_bv::get_arg_var(enode * n, unsigned idx) {
        return get_var(get_arg(n, idx));
    }

    void theory_bv::get_bits(theory_var v, expr_ref_vector & r) {
        context & ctx         = get_context();
        literal_vector & bits = m_bits[v];
        literal_vector::const_iterator it  = bits.begin();
        literal_vector::const_iterator end = bits.end();
        for (; it != end; ++it) {
            expr_ref l(get_manager());
            ctx.literal2expr(*it, l);
            r.push_back(l);
        }
    }

    inline void theory_bv::get_bits(enode * n, expr_ref_vector & r) {
        get_bits(get_var(n), r);
    }

    inline void theory_bv::get_arg_bits(enode * n, unsigned idx, expr_ref_vector & r) {
        get_bits(get_arg_var(n, idx), r);
    }

    inline void theory_bv::get_arg_bits(app * n, unsigned idx, expr_ref_vector & r) {
        context & ctx = get_context();
        app * arg     = to_app(n->get_arg(idx));
        SASSERT(ctx.e_internalized(arg));
        get_bits(ctx.get_enode(arg), r);
    }
    
    class add_var_pos_trail : public trail<theory_bv> {
        theory_bv::bit_atom * m_atom;
    public:
        add_var_pos_trail(theory_bv::bit_atom * a):m_atom(a) {}
        virtual void undo(theory_bv & th) {
            SASSERT(m_atom->m_occs);
            m_atom->m_occs = m_atom->m_occs->m_next;
        }
    };

    /**
       \brief v1[idx] = ~v2[idx], then v1 /= v2 is a theory axiom.
    */
    void theory_bv::mk_new_diseq_axiom(theory_var v1, theory_var v2, unsigned idx) {
        SASSERT(m_bits[v1][idx] == ~m_bits[v2][idx]);
        TRACE("bv_diseq_axiom", tout << "found new diseq axiom\n"; display_var(tout, v1); display_var(tout, v2););
        // found new disequality
        m_stats.m_num_diseq_static++;
        enode * e1       = get_enode(v1);
        enode * e2       = get_enode(v2);
        literal l        = ~(mk_eq(e1->get_owner(), e2->get_owner(), true));
        context & ctx    = get_context();
        ctx.mk_th_axiom(get_id(), 1, &l);
        if (ctx.relevancy()) {
            expr * eq    = ctx.bool_var2expr(l.var());
            relevancy_eh * eh = ctx.mk_relevancy_eh(pair_relevancy_eh(e1->get_owner(), e2->get_owner(), eq));
            ctx.add_relevancy_eh(e1->get_owner(), eh);
            ctx.add_relevancy_eh(e2->get_owner(), eh);
        }
    }

    void theory_bv::register_true_false_bit(theory_var v, unsigned idx) {
        SASSERT(m_bits[v][idx] == true_literal || m_bits[v][idx] == false_literal);
        bool is_true = (m_bits[v][idx] == true_literal);
        zero_one_bits & bits = m_zero_one_bits[v];
        bits.push_back(zero_one_bit(v, idx, is_true));
    }

    /**
       \brief v[idx] = ~v'[idx], then v /= v' is a theory axiom.
    */
    void theory_bv::find_new_diseq_axioms(var_pos_occ * occs, theory_var v, unsigned idx) {
        literal l = m_bits[v][idx];
        l.neg();
        while (occs) {
            theory_var v2   = occs->m_var;
            unsigned   idx2 = occs->m_idx;
            if (idx == idx2 && m_bits[v2][idx2] == l && get_bv_size(v2) == get_bv_size(v)) 
                mk_new_diseq_axiom(v, v2, idx);
            occs = occs->m_next;
        }
    }

    /**
       \brief Add bit l to the given variable.
    */
    void theory_bv::add_bit(theory_var v, literal l) {
        context & ctx         = get_context();
        literal_vector & bits = m_bits[v];
        unsigned idx          = bits.size();
        bits.push_back(l);
        if (l.var() == true_bool_var) {
            register_true_false_bit(v, idx);
        }
        else {
            theory_id th_id       = ctx.get_var_theory(l.var());
            if (th_id == get_id()) {
                atom * a     = get_bv2a(l.var());
                SASSERT(a && a->is_bit());
                bit_atom * b = static_cast<bit_atom*>(a);
                find_new_diseq_axioms(b->m_occs, v, idx);
                m_trail_stack.push(add_var_pos_trail(b));
                b->m_occs = new (get_region()) var_pos_occ(v, idx, b->m_occs);
            }
            else {
                SASSERT(th_id == null_theory_id);
                ctx.set_var_theory(l.var(), get_id());
                SASSERT(ctx.get_var_theory(l.var()) == get_id());
                bit_atom * b = new (get_region()) bit_atom();
                insert_bv2a(l.var(), b);
                m_trail_stack.push(mk_atom_trail(l.var()));
                SASSERT(b->m_occs == 0);
                b->m_occs = new (get_region()) var_pos_occ(v, idx);
            }
        }
    }

    void theory_bv::simplify_bit(expr * s, expr_ref & r) {
        // proof_ref p(get_manager());
        // if (get_context().at_base_level())
        //    m_simplifier->operator()(s, r, p);
        // else
        r = s;
    }

    void theory_bv::init_bits(enode * n, expr_ref_vector const & bits) {
        context & ctx           = get_context();
        ast_manager & m         = get_manager();
        theory_var v            = n->get_th_var(get_id());
        SASSERT(v != null_theory_var);
        unsigned sz             = bits.size();
        SASSERT(get_bv_size(n) == sz);
        for (unsigned i = 0; i < sz; i++) {
            expr * bit          = bits.get(i);
            expr_ref s_bit(m);
            simplify_bit(bit, s_bit);
            ctx.internalize(s_bit, true);
            literal l           = ctx.get_literal(s_bit.get());
            TRACE("init_bits", tout << "bit " << i << " of #" << n->get_owner_id() << "\n" << mk_ll_pp(s_bit, m) << "\n";);
            add_bit(v, l);
        }
        find_wpos(v);
    }

    /**
       \brief Find an unassigned bit for m_wpos[v], if such bit cannot be found invoke fixed_var_eh
    */
    void theory_bv::find_wpos(theory_var v) {
        context & ctx               = get_context();
        literal_vector const & bits = m_bits[v];
        unsigned sz                 = bits.size();
        unsigned & wpos             = m_wpos[v];
        unsigned init               = wpos;
        for (; wpos < sz; wpos++) {
            TRACE("find_wpos", tout << "curr bit: " << bits[wpos] << "\n";);
            if (ctx.get_assignment(bits[wpos]) == l_undef) {
                TRACE("find_wpos", tout << "moved wpos of v" << v << " to " << wpos << "\n";);
                return;
            }
        }
        wpos = 0;
        for (; wpos < init; wpos++) {
            if (ctx.get_assignment(bits[wpos]) == l_undef) {
                TRACE("find_wpos", tout << "moved wpos of v" << v << " to " << wpos << "\n";);
                return;
            }
        }
        TRACE("find_wpos", tout << "v" << v << " is a fixed variable.\n";);
        fixed_var_eh(v);
    }
    
    class fixed_eq_justification : public justification {
        theory_bv & m_th;
        theory_var  m_var1;
        theory_var  m_var2;

        void mark_bits(conflict_resolution & cr, literal_vector const & bits) {
            context & ctx = cr.get_context();
            literal_vector::const_iterator it  = bits.begin();
            literal_vector::const_iterator end = bits.end();
            for (; it != end; ++it) {
                if (it->var() != true_bool_var) {
                    if (ctx.get_assignment(*it) == l_true)
                        cr.mark_literal(*it);
                    else
                        cr.mark_literal(~(*it));
                }
            }
        }

        void get_proof(conflict_resolution & cr, literal l, ptr_buffer<proof> & prs, bool & visited) {
            if (l.var() == true_bool_var)
                return;
            proof * pr = 0;
            if (cr.get_context().get_assignment(l) == l_true)
                pr = cr.get_proof(l);
            else
                pr = cr.get_proof(~l);
            if (pr) 
                prs.push_back(pr);
            else
                visited = false;
        }

    public:
        fixed_eq_justification(theory_bv & th, theory_var v1, theory_var v2):
            m_th(th), m_var1(v1), m_var2(v2) {
        }
        
        virtual void get_antecedents(conflict_resolution & cr) {
            mark_bits(cr, m_th.m_bits[m_var1]);
            mark_bits(cr, m_th.m_bits[m_var2]);
        }
        
        virtual proof * mk_proof(conflict_resolution & cr) {
            ptr_buffer<proof> prs;
            context & ctx                       = cr.get_context();
            bool visited                        = true;
            literal_vector const & bits1        = m_th.m_bits[m_var1];
            literal_vector const & bits2        = m_th.m_bits[m_var2];
            literal_vector::const_iterator it1  = bits1.begin();
            literal_vector::const_iterator it2  = bits2.begin();
            literal_vector::const_iterator end1 = bits1.end();
            for (; it1 != end1; ++it1, ++it2) {
                get_proof(cr, *it1, prs, visited);
                get_proof(cr, *it2, prs, visited);
            }
            if (!visited)
                return 0;
            expr * fact     = ctx.mk_eq_atom(m_th.get_enode(m_var1)->get_owner(), m_th.get_enode(m_var2)->get_owner());
            ast_manager & m = ctx.get_manager();
            return m.mk_th_lemma(get_from_theory(), fact, prs.size(), prs.c_ptr());
        }

        virtual theory_id get_from_theory() const {
            return m_th.get_id();
        }
        
        virtual char const * get_name() const { return "bv-fixed-eq"; }

    };

    void theory_bv::fixed_var_eh(theory_var v) {
        numeral val;
        bool r      = get_fixed_value(v, val);
        SASSERT(r);
        unsigned sz = get_bv_size(v);
        value_sort_pair key(val, sz);
        theory_var v2;
        if (m_fixed_var_table.find(key, v2)) {
            numeral val2;
            if (v2 < static_cast<int>(get_num_vars()) && is_bv(v2) && 
                get_bv_size(v2) == sz && get_fixed_value(v2, val2) && val == val2) {
                if (get_enode(v)->get_root() != get_enode(v2)->get_root()) {
                    SASSERT(get_bv_size(v) == get_bv_size(v2));
                    context & ctx      = get_context();
                    justification * js = ctx.mk_justification(fixed_eq_justification(*this, v, v2));
                    TRACE("fixed_var_eh", tout << "detected equality: v" << v << " = v" << v2 << "\n";
                          display_var(tout, v);
                          display_var(tout, v2););
                    m_stats.m_num_th2core_eq++;
                    ctx.assign_eq(get_enode(v), get_enode(v2), eq_justification(js));
                }
            }
            else {
                // the original fixed variable v2 was deleted or it is not fixed anymore.
                m_fixed_var_table.erase(key);
                m_fixed_var_table.insert(key, v);
            }
        }
        else {
            m_fixed_var_table.insert(key, v);
        }
    }

    bool theory_bv::get_fixed_value(theory_var v, numeral & result) const {
        context & ctx                      = get_context();
        result.reset();
        literal_vector const & bits        = m_bits[v];
        literal_vector::const_iterator it  = bits.begin();
        literal_vector::const_iterator end = bits.end();
        for (unsigned i = 0; it != end; ++it, ++i) {
            switch (ctx.get_assignment(*it)) {
            case l_false: break;
            case l_undef: return false; 
            case l_true:  result += m_bb.power(i); break;
            }
        }
        return true;
    }

    bool theory_bv::get_fixed_value(app* x, numeral & result) const {
        context& ctx = get_context();
        if (!ctx.e_internalized(x)) return false;
        enode * e    = ctx.get_enode(x);
        theory_var v = e->get_th_var(get_id());
        return get_fixed_value(v, result);
    }


    void theory_bv::internalize_num(app * n) {
        SASSERT(!get_context().e_internalized(n));
        ast_manager & m = get_manager();
        numeral val;
        unsigned sz;
        m_util.is_numeral(n, val, sz);
        enode * e    = mk_enode(n);
        // internalizer is marking enodes as interpreted whenever the associated ast is a value and a constant.
        // e->mark_as_interpreted();
        theory_var v = e->get_th_var(get_id());
        expr_ref_vector bits(m);
        m_bb.num2bits(val, sz, bits);
        SASSERT(bits.size() == sz);
        literal_vector & c_bits = m_bits[v];
        for (unsigned i = 0; i < sz; i++) {
            expr * l = bits.get(i);
            if (m.is_true(l)) {
                c_bits.push_back(true_literal);
            }
            else {
                SASSERT(m.is_false(l));
                c_bits.push_back(false_literal);
            }
            register_true_false_bit(v, i);
        }
        fixed_var_eh(v);
    }

    void theory_bv::internalize_mkbv(app* n) {
        ast_manager& m = get_manager();
        expr_ref_vector bits(m);
        process_args(n);
        enode * e = mk_enode(n);
        bits.append(n->get_num_args(), n->get_args());
        init_bits(e, bits);
    }

    void theory_bv::internalize_bv2int(app* n) {
        SASSERT(!get_context().e_internalized(n));
        ast_manager & m = get_manager();
        context& ctx = get_context();
        TRACE("bv", tout << mk_bounded_pp(n, m) << "\n";);
        process_args(n);
        mk_enode(n);
        if (!ctx.relevancy()) {
            assert_bv2int_axiom(n);
        }
    }


    void theory_bv::assert_bv2int_axiom(app * n) {
        // 
        // create the axiom:
        // n = bv2int(k) = ite(bit2bool(k[sz-1],2^{sz-1},0) + ... + ite(bit2bool(k[0],1,0))
        // 
        SASSERT(get_context().e_internalized(n));
        SASSERT(m_util.is_bv2int(n));
        ast_manager & m = get_manager();
        TRACE("bv2int_bug", tout << "bv2int:\n" << mk_pp(n, m) << "\n";);
        context & ctx   = get_context();
        sort * int_sort = m.get_sort(n);
        app * k = to_app(n->get_arg(0));
        SASSERT(m_util.is_bv_sort(m.get_sort(k)));
        expr_ref_vector k_bits(m);
        enode * k_enode = mk_enode(k);
        get_bits(k_enode, k_bits);
        unsigned sz = m_util.get_bv_size(k);
        expr_ref_vector args(m);
        expr_ref zero(m_autil.mk_numeral(numeral(0), int_sort), m);
        numeral num(1);
        for (unsigned i = 0; i < sz; ++i) {
            // Remark: A previous version of this method was using
            //
            //        expr* b = mk_bit2bool(k,i); 
            //
            // This is not correct. The predicate bit2bool is an
            // internal construct, and it was not meant for building
            // axioms directly.  It is used to represent the bits of a
            // constant, and in some cases the bits of a complicated
            // bit-vector expression.  In most cases, the bits of a
            // composite bit-vector expression T are just boolean
            // combinations of bit2bool atoms of the bit-vector
            // constants contained in T. So, instead of using
            // mk_bit2bool to access a particular bit of T, we should
            // use the method get_bits.
            // 
            expr * b = k_bits.get(i);
            expr_ref n(m_autil.mk_numeral(num, int_sort), m);
            args.push_back(m.mk_ite(b, n, zero));
            num *= numeral(2);
        }
        expr_ref sum(m);
        arith_simp().mk_add(sz, args.c_ptr(), sum);
        TRACE("bv", 
              tout << mk_pp(n, m) << "\n";
              tout << mk_pp(sum, m) << "\n";
              );

        literal l(mk_eq(n, sum, false));
       
        ctx.mark_as_relevant(l);
        ctx.mk_th_axiom(get_id(), 1, &l);
    }

    void theory_bv::internalize_int2bv(app* n) {    
        SASSERT(!get_context().e_internalized(n));
        SASSERT(n->get_num_args() == 1);
        context& ctx = get_context();
        process_args(n);
        mk_enode(n);
        mk_bits(ctx.get_enode(n)->get_th_var(get_id()));
        if (!ctx.relevancy()) {
            assert_int2bv_axiom(n);
        }
    }
    
    void theory_bv::assert_int2bv_axiom(app* n) {
        //
        // create the axiom:
        // bv2int(n) = e mod 2^bit_width 
        //
        // where n = int2bv(e)
        //
        SASSERT(get_context().e_internalized(n));
        SASSERT(m_util.is_int2bv(n));
        ast_manager & m = get_manager();
        context& ctx = get_context();

        parameter param(m_autil.mk_int());
        expr* n_expr = n;
        expr* lhs = m.mk_app(get_id(), OP_BV2INT, 1, &param, 1, &n_expr);
        unsigned sz = m_util.get_bv_size(n);
        numeral mod = power(numeral(2), sz);
        expr* rhs = m_autil.mk_mod(n->get_arg(0), m_autil.mk_numeral(mod, true));

        literal l(mk_eq(lhs, rhs, false));
        ctx.mark_as_relevant(l);
        ctx.mk_th_axiom(get_id(), 1, &l);
        
        TRACE("bv", 
              tout << mk_pp(lhs, m) << " == \n";
              tout << mk_pp(rhs, m) << "\n";
              );
    }


#define MK_UNARY(NAME, BLAST_OP)                                        \
    void theory_bv::NAME(app * n) {                                     \
        SASSERT(!get_context().e_internalized(n));                      \
        SASSERT(n->get_num_args() == 1);                                \
        process_args(n);                                                \
        ast_manager & m = get_manager();                                \
        enode * e       = mk_enode(n);                                  \
        expr_ref_vector arg1_bits(m), bits(m);                          \
        get_arg_bits(e, 0, arg1_bits);                                  \
        m_bb.BLAST_OP(arg1_bits.size(), arg1_bits.c_ptr(), bits);       \
        init_bits(e, bits);                                             \
    }

#define MK_BINARY(NAME, BLAST_OP)                                                       \
    void theory_bv::NAME(app * n) {                                                     \
        SASSERT(!get_context().e_internalized(n));                                      \
        SASSERT(n->get_num_args() == 2);                                                \
        process_args(n);                                                                \
        ast_manager & m = get_manager();                                                \
        enode * e       = mk_enode(n);                                                  \
        expr_ref_vector arg1_bits(m), arg2_bits(m), bits(m);                            \
        get_arg_bits(e, 0, arg1_bits);                                                  \
        get_arg_bits(e, 1, arg2_bits);                                                  \
        SASSERT(arg1_bits.size() == arg2_bits.size());                                  \
        m_bb.BLAST_OP(arg1_bits.size(), arg1_bits.c_ptr(), arg2_bits.c_ptr(), bits);    \
        init_bits(e, bits);                                                             \
    }


#define MK_AC_BINARY(NAME, BLAST_OP)                                                            \
    void theory_bv::NAME(app * n) {                                                             \
        SASSERT(!get_context().e_internalized(n));                                              \
        SASSERT(n->get_num_args() >= 2);                                                        \
        process_args(n);                                                                        \
        ast_manager & m = get_manager();                                                        \
        enode * e       = mk_enode(n);                                                          \
        expr_ref_vector arg_bits(m);                                                            \
        expr_ref_vector bits(m);                                                                \
        expr_ref_vector new_bits(m);                                                            \
        unsigned i = n->get_num_args();                                                         \
        --i;                                                                                    \
        get_arg_bits(e, i, bits);                                                               \
        while (i > 0) {                                                                         \
            --i;                                                                                \
            arg_bits.reset();                                                                   \
            get_arg_bits(e, i, arg_bits);                                                       \
            SASSERT(arg_bits.size() == bits.size());                                            \
            new_bits.reset();                                                                   \
            m_bb.BLAST_OP(arg_bits.size(), arg_bits.c_ptr(), bits.c_ptr(), new_bits);           \
            bits.swap(new_bits);                                                                \
        }                                                                                       \
        init_bits(e, bits);                                                                     \
    }


#define MK_BINARY_COND(NAME, BLAST_OP)                                                  \
    void theory_bv::NAME(app * n) {                                                     \
        SASSERT(!get_context().e_internalized(n));                                      \
        SASSERT(n->get_num_args() == 2);                                                \
        process_args(n);                                                                \
        ast_manager & m = get_manager();                                                \
        context& ctx = get_context();                                   \
        enode * e       = mk_enode(n);                                                  \
        expr_ref_vector arg1_bits(m), arg2_bits(m), bits(m);                            \
        expr_ref        cond(m), s_cond(m);                                             \
        get_arg_bits(e, 0, arg1_bits);                                                  \
        get_arg_bits(e, 1, arg2_bits);                                                  \
        SASSERT(arg1_bits.size() == arg2_bits.size());                                  \
        m_bb.BLAST_OP(arg1_bits.size(), arg1_bits.c_ptr(), arg2_bits.c_ptr(), bits, cond); \
        init_bits(e, bits);                                                             \
        simplify_bit(cond, s_cond);                                     \
        ctx.internalize(s_cond, true);                                  \
        literal l(ctx.get_literal(s_cond));                             \
        ctx.mark_as_relevant(l);                                        \
        ctx.mk_th_axiom(get_id(), 1, &l);                               \
        TRACE("bv", tout << mk_pp(cond, get_manager()) << "\n"; tout << l << "\n";); \
    }

    MK_UNARY(internalize_not,       mk_not);
    MK_UNARY(internalize_redand,    mk_redand);
    MK_UNARY(internalize_redor,     mk_redor);

    MK_AC_BINARY(internalize_add,      mk_adder);
    MK_AC_BINARY(internalize_mul,      mk_multiplier);
    MK_BINARY(internalize_udiv,     mk_udiv);
    MK_BINARY(internalize_sdiv,     mk_sdiv);
    MK_BINARY(internalize_urem,     mk_urem);
    MK_BINARY(internalize_srem,     mk_srem);
    MK_BINARY(internalize_smod,     mk_smod);
    MK_BINARY(internalize_shl,      mk_shl);
    MK_BINARY(internalize_lshr,     mk_lshr);
    MK_BINARY(internalize_ashr,     mk_ashr);
    MK_BINARY(internalize_ext_rotate_left,  mk_ext_rotate_left);
    MK_BINARY(internalize_ext_rotate_right, mk_ext_rotate_right);
    MK_AC_BINARY(internalize_and,      mk_and);
    MK_AC_BINARY(internalize_or,       mk_or);
    MK_AC_BINARY(internalize_xor,      mk_xor);
    MK_AC_BINARY(internalize_nand,     mk_nand);
    MK_AC_BINARY(internalize_nor,      mk_nor);
    MK_AC_BINARY(internalize_xnor,     mk_xnor);
    MK_BINARY(internalize_comp,     mk_comp);

#define MK_PARAMETRIC_UNARY(NAME, BLAST_OP)                                     \
    void theory_bv::NAME(app * n) {                                             \
        SASSERT(!get_context().e_internalized(n));                              \
        SASSERT(n->get_num_args() == 1);                                        \
        process_args(n);                                                        \
        ast_manager & m = get_manager();                                        \
        enode * e       = mk_enode(n);                                          \
        expr_ref_vector arg1_bits(m), bits(m);                                  \
        get_arg_bits(e, 0, arg1_bits);                                          \
        unsigned param  = n->get_decl()->get_parameter(0).get_int();            \
        m_bb.BLAST_OP(arg1_bits.size(), arg1_bits.c_ptr(), param, bits);        \
        init_bits(e, bits);                                                     \
    }
    
    MK_PARAMETRIC_UNARY(internalize_sign_extend, mk_sign_extend);
    MK_PARAMETRIC_UNARY(internalize_zero_extend, mk_zero_extend);
    MK_PARAMETRIC_UNARY(internalize_rotate_left, mk_rotate_left);
    MK_PARAMETRIC_UNARY(internalize_rotate_right, mk_rotate_right);

    void theory_bv::internalize_concat(app * n) {
        process_args(n);        
        enode * e          = mk_enode(n);  
        theory_var v       = e->get_th_var(get_id());
        unsigned num_args  = n->get_num_args();
        unsigned i         = num_args;
        while (i > 0) {
            i--;
            theory_var arg = get_arg_var(e, i);
            literal_vector::const_iterator it  = m_bits[arg].begin();
            literal_vector::const_iterator end = m_bits[arg].end();
            for (; it != end; ++it)
                add_bit(v, *it);
        }
        find_wpos(v);
    }

    void theory_bv::internalize_extract(app * n) {
        SASSERT(n->get_num_args() == 1);
        process_args(n);            
        enode * e          = mk_enode(n);  
        theory_var v       = e->get_th_var(get_id());
        theory_var arg     = get_arg_var(e, 0);
        unsigned start     = n->get_decl()->get_parameter(1).get_int();
        unsigned end       = n->get_decl()->get_parameter(0).get_int();
        SASSERT(start <= end);
        literal_vector & arg_bits = m_bits[arg];
        for (unsigned i = start; i <= end; ++i)
            add_bit(v, arg_bits[i]);
        find_wpos(v);
    }

    bool theory_bv::internalize_term(app * term) {
        SASSERT(term->get_family_id() == get_family_id());
        TRACE("bv", tout << "internalizing term: " << mk_bounded_pp(term, get_manager()) << "\n";);
        if (approximate_term(term)) {
            return false;
        }
        switch (term->get_decl_kind()) {
        case OP_BV_NUM:         internalize_num(term); return true;
        case OP_BADD:           internalize_add(term); return true;
        case OP_BMUL:           internalize_mul(term); return true;
        case OP_BSDIV_I:        internalize_sdiv(term); return true;
        case OP_BUDIV_I:        internalize_udiv(term); return true;
        case OP_BSREM_I:        internalize_srem(term); return true;
        case OP_BUREM_I:        internalize_urem(term); return true;
        case OP_BSMOD_I:        internalize_smod(term); return true;
        case OP_BAND:           internalize_and(term); return true;
        case OP_BOR:            internalize_or(term); return true;
        case OP_BNOT:           internalize_not(term); return true;
        case OP_BXOR:           internalize_xor(term); return true;
        case OP_BNAND:          internalize_nand(term); return true;
        case OP_BNOR:           internalize_nor(term); return true;
        case OP_BXNOR:          internalize_xnor(term); return true;
        case OP_CONCAT:         internalize_concat(term); return true;
        case OP_SIGN_EXT:       internalize_sign_extend(term); return true;
        case OP_ZERO_EXT:       internalize_zero_extend(term); return true;
        case OP_EXTRACT:        internalize_extract(term); return true;
        case OP_BREDOR:         internalize_redor(term); return true;
        case OP_BREDAND:        internalize_redand(term); return true;
        case OP_BCOMP:          internalize_comp(term); return true;
        case OP_BSHL:           internalize_shl(term); return true;
        case OP_BLSHR:          internalize_lshr(term); return true;
        case OP_BASHR:          internalize_ashr(term); return true;
        case OP_ROTATE_LEFT:    internalize_rotate_left(term); return true;
        case OP_ROTATE_RIGHT:   internalize_rotate_right(term); return true;
        case OP_EXT_ROTATE_LEFT:  internalize_ext_rotate_left(term); return true;
        case OP_EXT_ROTATE_RIGHT: internalize_ext_rotate_right(term); return true;
        case OP_BSDIV0:         return false;
        case OP_BUDIV0:         return false;
        case OP_BSREM0:         return false;
        case OP_BUREM0:         return false;
        case OP_BSMOD0:         return false;
        case OP_MKBV:           internalize_mkbv(term); return true;
        case OP_INT2BV:         
            if (m_params.m_bv_enable_int2bv2int) {
                internalize_int2bv(term); 
            }
            return m_params.m_bv_enable_int2bv2int;
        case OP_BV2INT:         
            if (m_params.m_bv_enable_int2bv2int) {
                internalize_bv2int(term); 
            }
            return m_params.m_bv_enable_int2bv2int;
        default:
            TRACE("bv_op", tout << "unsupported operator: " << mk_ll_pp(term, get_manager()) << "\n";);
            UNREACHABLE();
            return false;
        }
    }

#define MK_NO_OVFL(NAME, OP)                                                                                    \
    void theory_bv::NAME(app *n) {                                                                              \
        SASSERT(n->get_num_args() == 2);                                                                        \
        process_args(n);                                                                                        \
        ast_manager & m = get_manager();                                                                        \
        context & ctx   = get_context();                                                                        \
        expr_ref_vector arg1_bits(m), arg2_bits(m);                                                             \
        get_arg_bits(n, 0, arg1_bits);                                                                          \
        get_arg_bits(n, 1, arg2_bits);                                                                          \
        expr_ref out(m);                                                                                        \
        m_bb.OP(arg1_bits.size(), arg1_bits.c_ptr(), arg2_bits.c_ptr(), out);                                   \
        expr_ref s_out(m);                                                                                      \
        simplify_bit(out, s_out);                                                                               \
        ctx.internalize(s_out, true);                                                                           \
        literal def = ctx.get_literal(s_out);                                                                   \
        literal l(ctx.mk_bool_var(n));                                                                          \
        ctx.set_var_theory(l.var(), get_id());                                                                  \
        le_atom * a     = new (get_region()) le_atom(l, def); /* abuse le_atom */                               \
        insert_bv2a(l.var(), a);                                                                                \
        m_trail_stack.push(mk_atom_trail(l.var()));                                                             \
        /* smul_no_overflow and umul_no_overflow are using the le_atom (THIS IS A BIG HACK)... */               \
        /* the connection between the l and def was never realized when                        */               \
        /* relevancy() is true and m_bv_lazy_le is false (the default configuration).          */               \
        /* So, we need to check also the m_bv_lazy_le flag here.                               */               \
        /* Maybe, we should rename the le_atom to bridge_atom, and m_bv_lazy_le option to m_bv_lazy_bridge. */  \
        if (!ctx.relevancy() || !m_params.m_bv_lazy_le) {                                                       \
            ctx.mk_th_axiom(get_id(),  l, ~def);                                                                \
            ctx.mk_th_axiom(get_id(), ~l,  def);                                                                \
        }                                                                                                       \
    }

    MK_NO_OVFL(internalize_umul_no_overflow, mk_umul_no_overflow);
    MK_NO_OVFL(internalize_smul_no_overflow, mk_smul_no_overflow);
    MK_NO_OVFL(internalize_smul_no_underflow, mk_smul_no_underflow);

    template<bool Signed>
    void theory_bv::internalize_le(app * n) {
        SASSERT(n->get_num_args() == 2);                                                
        process_args(n);                          
        ast_manager & m = get_manager();                                                
        context & ctx   = get_context();
        expr_ref_vector arg1_bits(m), arg2_bits(m);
        get_arg_bits(n, 0, arg1_bits);                                                  
        get_arg_bits(n, 1, arg2_bits);                                                  
        expr_ref le(m);
        if (Signed)
            m_bb.mk_sle(arg1_bits.size(), arg1_bits.c_ptr(), arg2_bits.c_ptr(), le);
        else
            m_bb.mk_ule(arg1_bits.size(), arg1_bits.c_ptr(), arg2_bits.c_ptr(), le);
        expr_ref s_le(m);
        simplify_bit(le, s_le);
        ctx.internalize(s_le, true);
        literal def = ctx.get_literal(s_le);
        literal l(ctx.mk_bool_var(n));
        ctx.set_var_theory(l.var(), get_id());
        le_atom * a     = new (get_region()) le_atom(l, def);
        insert_bv2a(l.var(), a);
        m_trail_stack.push(mk_atom_trail(l.var()));
        if (!ctx.relevancy() || !m_params.m_bv_lazy_le) {
            ctx.mk_th_axiom(get_id(),  l, ~def);
            ctx.mk_th_axiom(get_id(), ~l,  def);
        }
    }

    bool theory_bv::internalize_carry(app * n, bool gate_ctx) {
        context & ctx = get_context();
        ctx.internalize(n->get_arg(0), true);
        ctx.internalize(n->get_arg(1), true);
        ctx.internalize(n->get_arg(2), true);
        bool is_new_var = false;
        bool_var v;
        if (!ctx.b_internalized(n)) {
            is_new_var  = true;
            v           = ctx.mk_bool_var(n);
            literal r(v);
            literal l1 = ctx.get_literal(n->get_arg(0));
            literal l2 = ctx.get_literal(n->get_arg(1));
            literal l3 = ctx.get_literal(n->get_arg(2));
            ctx.mk_gate_clause(~r,  l1,  l2);
            ctx.mk_gate_clause(~r,  l1,  l3);
            ctx.mk_gate_clause(~r,  l2,  l3);
            ctx.mk_gate_clause( r, ~l1, ~l2);
            ctx.mk_gate_clause( r, ~l1, ~l3);
            ctx.mk_gate_clause( r, ~l2, ~l3);
        }
        else {
            v = ctx.get_bool_var(n);
        }

        if (!ctx.e_internalized(n) && !gate_ctx) {
            bool suppress_args = true;
            bool merge_tf      = !gate_ctx;
            ctx.mk_enode(n, suppress_args, merge_tf, true);
            ctx.set_enode_flag(v, is_new_var);
        }
        return true;
    }

    bool theory_bv::internalize_xor3(app * n, bool gate_ctx) {
        context & ctx = get_context();
        ctx.internalize(n->get_arg(0), true);
        ctx.internalize(n->get_arg(1), true);
        ctx.internalize(n->get_arg(2), true);
        bool is_new_var = false;
        bool_var v;
        if (!ctx.b_internalized(n)) {
            is_new_var  = true;
            v           = ctx.mk_bool_var(n);
            literal r(v);
            literal l1 = ctx.get_literal(n->get_arg(0));
            literal l2 = ctx.get_literal(n->get_arg(1));
            literal l3 = ctx.get_literal(n->get_arg(2));
            ctx.mk_gate_clause(~r,  l1,  l2,  l3);
            ctx.mk_gate_clause(~r, ~l1, ~l2,  l3);
            ctx.mk_gate_clause(~r, ~l1,  l2, ~l3);
            ctx.mk_gate_clause(~r,  l1, ~l2, ~l3);
            ctx.mk_gate_clause( r, ~l1,  l2,  l3);
            ctx.mk_gate_clause( r,  l1, ~l2,  l3);
            ctx.mk_gate_clause( r,  l1,  l2, ~l3);
            ctx.mk_gate_clause( r, ~l1, ~l2, ~l3);
        }
        else {
            v = ctx.get_bool_var(n);
        }

        if (!ctx.e_internalized(n) && !gate_ctx) {
            bool suppress_args = true;
            bool merge_tf      = !gate_ctx;
            ctx.mk_enode(n, suppress_args, merge_tf, true);
            ctx.set_enode_flag(v, is_new_var);
        }
        return true;
    }

    bool theory_bv::internalize_atom(app * atom, bool gate_ctx) {
        TRACE("bv", tout << "internalizing atom: " << mk_bounded_pp(atom, get_manager()) << "\n";);
        SASSERT(atom->get_family_id() == get_family_id());
        if (approximate_term(atom)) {
            return false;
        }
        switch (atom->get_decl_kind()) {
        case OP_BIT2BOOL:   mk_bit2bool(atom); return true;
        case OP_ULEQ:       internalize_le<false>(atom); return true;
        case OP_SLEQ:       internalize_le<true>(atom); return true;
        case OP_XOR3:       return internalize_xor3(atom, gate_ctx); 
        case OP_CARRY:      return internalize_carry(atom, gate_ctx); 
        case OP_BUMUL_NO_OVFL:  internalize_umul_no_overflow(atom); return true;
        case OP_BSMUL_NO_OVFL:  internalize_smul_no_overflow(atom); return true;
        case OP_BSMUL_NO_UDFL:  internalize_smul_no_underflow(atom); return true;
        default:
            UNREACHABLE();
            return false;
        }
    }

    //
    // Determine whether bit-vector expression should be approximated
    // based on the number of bits used by the arguments.
    // 
    bool theory_bv::approximate_term(app* n) {
        if (m_params.m_bv_blast_max_size == INT_MAX) {
            return false;
        }
        unsigned num_args = n->get_num_args();
        for (unsigned i = 0; i <= num_args; i++) {
            expr* arg = (i == num_args)?n:n->get_arg(i);
            sort* s = get_manager().get_sort(arg);
            s = get_manager().get_sort(arg);
            if (m_util.is_bv_sort(s) && m_util.get_bv_size(arg) > m_params.m_bv_blast_max_size) {                
                if (!m_approximates_large_bvs) {
                    TRACE("bv", tout << "found large size bit-vector:\n" << mk_pp(n, get_manager()) << "\n";);
                    get_context().push_trail(value_trail<context, bool>(m_approximates_large_bvs));
                    m_approximates_large_bvs = true;
                }
                return true;
            }
        }
        return false;

    }

    void theory_bv::apply_sort_cnstr(enode * n, sort * s) {
        if (!is_attached_to_var(n) && !approximate_term(n->get_owner())) {
            theory_var v = mk_var(n);
            mk_bits(v);
        }
    }
    
    void theory_bv::new_eq_eh(theory_var v1, theory_var v2) {
        TRACE("bv_eq", tout << "new_eq: " << mk_pp(get_enode(v1)->get_owner(), get_manager()) << " = " << mk_pp(get_enode(v2)->get_owner(), get_manager()) << "\n";);
        TRACE("bv", tout << "new_eq_eh v" << v1 << " = v" << v2 << 
              " relevant1: " << get_context().is_relevant(get_enode(v1)) << 
              " relevant2: " << get_context().is_relevant(get_enode(v2)) << "\n";);
        m_find.merge(v1, v2);
    }

    void theory_bv::new_diseq_eh(theory_var v1, theory_var v2) {
        if (is_bv(v1)) {
            expand_diseq(v1, v2);
        }
    }

    void theory_bv::expand_diseq(theory_var v1, theory_var v2) {
        SASSERT(get_bv_size(v1) == get_bv_size(v2));
        context & ctx         = get_context();
        ast_manager & m       = get_manager();
#ifdef _TRACE
        unsigned num_bool_vars = ctx.get_num_bool_vars();
#endif 
        literal_vector & lits = m_tmp_literals;
        lits.reset();
        lits.push_back(mk_eq(get_enode(v1)->get_owner(), get_enode(v2)->get_owner(), true));
        literal_vector const & bits1        = m_bits[v1];
        literal_vector::const_iterator it1  = bits1.begin();
        literal_vector::const_iterator end1 = bits1.end();
        literal_vector const & bits2        = m_bits[v2];
        literal_vector::const_iterator it2  = bits2.begin();
        for (; it1 != end1; ++it1, ++it2) {
            if (*it1 == ~(*it2))
                return; // static diseq
        }
        it1 = bits1.begin();
        it2 = bits2.begin();
        for (; it1 != end1; ++it1, ++it2) {
            expr_ref l1(m), l2(m), diff(m);
            ctx.literal2expr(*it1, l1);
            ctx.literal2expr(*it2, l2);
            m_bb.mk_xor(l1, l2, diff);
            ctx.internalize(diff, true);
            literal arg = ctx.get_literal(diff);
            lits.push_back(arg);
        }
        m_stats.m_num_diseq_dynamic++;
        ctx.mk_th_axiom(get_id(), lits.size(), lits.c_ptr());
        TRACE_CODE({
            static unsigned num = 0;
            static unsigned new_bool_vars = 0;
            new_bool_vars += (ctx.get_num_bool_vars() - num_bool_vars);
            if (num % 1000 == 0)
                TRACE("expand_diseq", tout << "num: " << num << " " << new_bool_vars << "\n";);
            num++;
        });
    }

    void theory_bv::assign_eh(bool_var v, bool is_true) {
        context & ctx = get_context();
        atom * a      = get_bv2a(v);
        TRACE("bv", tout << "assert: v" << v << " #" << ctx.bool_var2expr(v)->get_id() << " is_true: " << is_true << "\n";);
        if (a->is_bit()) {
            // The following optimization is not correct.
            // Boolean variables created for performing bit-blasting are reused.
            // See regression\trevor6.smt for example.
            // 
            // if (ctx.has_th_justification(v, get_id())) {
            //    TRACE("bv", tout << "has th_justification\n";);
            //    return;
            // }
            m_prop_queue.reset();
            bit_atom * b = static_cast<bit_atom*>(a);
            var_pos_occ * curr = b->m_occs;
            while (curr) {
                m_prop_queue.push_back(var_pos(curr->m_var, curr->m_idx));
                curr = curr->m_next;
            }
            TRACE("bv", tout << m_prop_queue.size() << "\n";);
            propagate_bits();
        }
    }
    
    void theory_bv::propagate_bits() {
        context & ctx = get_context();
        for (unsigned i = 0; i < m_prop_queue.size(); i++) {
            var_pos const & entry = m_prop_queue[i];
            theory_var v          = entry.first;
            unsigned idx          = entry.second;

            if (m_wpos[v] == idx)
                find_wpos(v);
            

            literal_vector & bits = m_bits[v];
            literal bit           = bits[idx];
            lbool    val          = ctx.get_assignment(bit); 
            theory_var v2         = next(v);
            TRACE("bv_bit_prop", tout << "propagating #" << get_enode(v)->get_owner_id() << "[" << idx << "] = " << val << "\n";);
            while (v2 != v) {
                literal_vector & bits2   = m_bits[v2];
                literal bit2             = bits2[idx];
                SASSERT(bit != ~bit2);
                lbool   val2             = ctx.get_assignment(bit2);
                TRACE("bv_bit_prop", tout << "propagating #" << get_enode(v2)->get_owner_id() << "[" << idx << "] = " << val2 << "\n";);
                if (val != val2) {
                    literal antecedent = bit;
                    literal consequent = bit2;
                    if (val == l_false) {
                        antecedent.neg();
                        consequent.neg();
                    }
                    SASSERT(ctx.get_assignment(antecedent) == l_true);
                    assign_bit(consequent, v, v2, idx, antecedent, false);
                    if (ctx.inconsistent()) {
                        TRACE("bv_bit_prop", tout << "inconsistent " << bit <<  " " << bit2 << "\n";);
                        return;
                    }
                }
                v2 = next(v2);
            }            
        }
        m_prop_queue.reset();
        TRACE("bv_bit_prop", tout << "done propagating\n";);
    }

    void theory_bv::assign_bit(literal consequent, theory_var v1, theory_var v2, unsigned idx, literal antecedent, bool propagate_eqc) {
        m_stats.m_num_bit2core++;
        context & ctx = get_context();
        SASSERT(ctx.get_assignment(antecedent) == l_true);
        SASSERT(m_bits[v2][idx].var() == consequent.var());
        SASSERT(consequent.var() != antecedent.var());
        TRACE("bv_bit_prop", tout << "assigning: "; ctx.display_literal(tout, consequent);
              tout << " using "; ctx.display_literal(tout, antecedent); 
              tout << " #" << get_enode(v1)->get_owner_id() << " #" << get_enode(v2)->get_owner_id() << " idx: " << idx << "\n";
              tout << "propagate_eqc: " << propagate_eqc << "\n";);
        if (consequent == false_literal) {
            m_stats.m_num_conflicts++;
            ctx.set_conflict(mk_bit_eq_justification(v1, v2, consequent, antecedent));
        }
        else {
            ctx.assign(consequent, mk_bit_eq_justification(v1, v2, consequent, antecedent));
            if (m_wpos[v2] == idx)
                find_wpos(v2);
            // REMARK: bit_eq_justification is marked as a theory_bv justification.
            // Thus, the assignment to consequent will not be notified back to the theory.
            // So, we need to propagate the assignment to other bits.
            bool_var bv = consequent.var();
            atom * a    = get_bv2a(bv);
            SASSERT(a->is_bit());
            bit_atom * b = static_cast<bit_atom*>(a);
            var_pos_occ * curr = b->m_occs;
            while (curr) {
                TRACE("assign_bit_bug", tout << "curr->m_var: v" << curr->m_var << ", curr->m_idx: " << curr->m_idx << ", v2: v" << v2 << ", idx: " << idx << "\n";
                      tout << "find(curr->m_var): v" << find(curr->m_var) << ", find(v2): v" << find(v2) << "\n";
                      tout << "is bit of #" << get_enode(curr->m_var)->get_owner_id() << "\n";
                      );
                // If find(curr->m_var) == find(v2) && curr->m_idx == idx and propagate_eqc == false, then
                // this bit will be propagated to the equivalence class of v2 by assign_bit caller.
                if (propagate_eqc || find(curr->m_var) != find(v2) || curr->m_idx != idx)
                    m_prop_queue.push_back(var_pos(curr->m_var, curr->m_idx));
                curr = curr->m_next;
            }
        }
    }

    void theory_bv::relevant_eh(app * n) {
        ast_manager & m = get_manager();
        context & ctx   = get_context();
        if (m.is_bool(n)) {
            bool_var v = ctx.get_bool_var(n);
            atom * a   = get_bv2a(v);
            if (a && !a->is_bit()) {
                le_atom * le = static_cast<le_atom*>(a);
                ctx.mark_as_relevant(le->m_def);
                if (m_params.m_bv_lazy_le) {
                    ctx.mk_th_axiom(get_id(), le->m_var, ~le->m_def);
                    ctx.mk_th_axiom(get_id(), ~le->m_var, le->m_def);
                }
            }
        }
        else if (m_params.m_bv_enable_int2bv2int && m_util.is_bv2int(n)) {
            ctx.mark_as_relevant(n->get_arg(0));
            assert_bv2int_axiom(n);
        }
        else if (m_params.m_bv_enable_int2bv2int && m_util.is_int2bv(n)) {
            ctx.mark_as_relevant(n->get_arg(0));
            assert_int2bv_axiom(n);
        }
        else if (ctx.e_internalized(n)) {
            enode * e    = ctx.get_enode(n);
            theory_var v = e->get_th_var(get_id());
            if (v != null_theory_var) {
                literal_vector & bits        = m_bits[v];
                literal_vector::iterator it  = bits.begin();
                literal_vector::iterator end = bits.end();
                for (; it != end; ++it)
                    ctx.mark_as_relevant(*it);
            }
        }
    }

    void theory_bv::push_scope_eh() {
        theory::push_scope_eh();
        m_trail_stack.push_scope();
    }
    
    void theory_bv::pop_scope_eh(unsigned num_scopes) {
        TRACE("bv",tout << num_scopes << "\n";);
        m_trail_stack.pop_scope(num_scopes);
        unsigned num_old_vars = get_old_num_vars(num_scopes);
        m_bits.shrink(num_old_vars);
        m_wpos.shrink(num_old_vars);
        m_zero_one_bits.shrink(num_old_vars);
        theory::pop_scope_eh(num_scopes);
    }

    final_check_status theory_bv::final_check_eh() {
        SASSERT(check_invariant());
        if (m_approximates_large_bvs) {
            return FC_GIVEUP;
        }
        return FC_DONE;
    }

    void theory_bv::reset_eh() {
        pop_scope_eh(m_trail_stack.get_num_scopes());
        m_bool_var2atom.reset();
        m_fixed_var_table.reset();
        theory::reset_eh();
    }

    theory_bv::theory_bv(ast_manager & m, theory_bv_params const & params, bit_blaster_params const & bb_params):
        theory(m.mk_family_id("bv")),
        m_params(params),
        m_util(m),
        m_autil(m),
        m_simplifier(0),
        m_bb(m, bb_params),
        m_trail_stack(*this),
        m_find(*this),
        m_approximates_large_bvs(false) {
    }

    theory_bv::~theory_bv() {
    }
    
    void theory_bv::merge_eh(theory_var r1, theory_var r2, theory_var v1, theory_var v2) {
        TRACE("bv", tout << "merging: #" << get_enode(v1)->get_owner_id() << " #" << get_enode(v2)->get_owner_id() << "\n";);
        TRACE("bv_bit_prop", tout << "merging: #" << get_enode(v1)->get_owner_id() << " #" << get_enode(v2)->get_owner_id() << "\n";);
        if (!merge_zero_one_bits(r1, r2)) {
            TRACE("bv", tout << "conflict detected\n";);
            return; // conflict was detected
        }
        m_prop_queue.reset();
        context & ctx                 = get_context();
        literal_vector & bits1        = m_bits[v1];
        literal_vector & bits2        = m_bits[v2];
        SASSERT(bits1.size() == bits2.size());
        unsigned sz                   = bits1.size();
        bool changed;
        TRACE("bv", tout << "bits size: " << sz << "\n";);
        do {
            // This outerloop is necessary to avoid missing propagation steps.
            // For example, let's assume that bits1 and bits2 contains the following
            // sequence of bits:
            //        b4 b3 b2 b1
            //        b5 b4 b3 b2
            // Let's also assume that b1 is assigned, and b2, b3, b4, and b5 are not.
            // Only the propagation from b1 to b2 is performed by the first iteration of this
            // loop. 
            //
            // In the worst case, we need to execute this loop bits1.size() times.
            //
            // Remark: the assignment to b2 is marked as a bv theory propagation,
            // then it is not notified to the bv theory.
            changed                   = false;
            for (unsigned idx = 0; idx < sz; idx++) {
                literal bit1  = bits1[idx];
                literal bit2  = bits2[idx];
                CTRACE("bv_bug", bit1 == ~bit2, display_var(tout, v1); display_var(tout, v2); tout << "idx: " << idx << "\n";);
                SASSERT(bit1 != ~bit2);
                lbool val1    = ctx.get_assignment(bit1);
                lbool val2    = ctx.get_assignment(bit2);
                if (val1 == val2)
                    continue;
                changed = true;
                if (val1 != l_undef && val2 != l_undef) {
                    TRACE("bv", tout << "inconsistent "; display_var(tout, v1); display_var(tout, v2); tout << "idx: " << idx << "\n";);
                }
                if (val1 != l_undef) {
                    literal antecedent = bit1;
                    literal consequent = bit2;
                    if (val1 == l_false) {
                        consequent.neg();
                        antecedent.neg();
                    }
                    assign_bit(consequent, v1, v2, idx, antecedent, true);
                }
                else if (val2 != l_undef) {
                    literal antecedent = bit2;
                    literal consequent = bit1;
                    if (val2 == l_false) {
                        consequent.neg();
                        antecedent.neg();
                    }
                    assign_bit(consequent, v2, v1, idx, antecedent, true);
                }
                if (ctx.inconsistent())
                    return;
                if (val1 != l_undef && val2 != l_undef && val1 != val2) {
                    UNREACHABLE();
                }
                
            }
        }
        while(changed);

        propagate_bits();
    }

    bool theory_bv::merge_zero_one_bits(theory_var r1, theory_var r2) {
        zero_one_bits & bits2 = m_zero_one_bits[r2];
        if (bits2.empty())
            return true;
        zero_one_bits & bits1 = m_zero_one_bits[r1];
        unsigned bv_size = get_bv_size(r1);
        SASSERT(bv_size == get_bv_size(r2));
        m_merge_aux[0].reserve(bv_size+1, null_theory_var);
        m_merge_aux[1].reserve(bv_size+1, null_theory_var);
#define RESET_MERGET_AUX() {                                                    \
            zero_one_bits::iterator it  = bits1.begin();                        \
            zero_one_bits::iterator end = bits1.end();                          \
            for (; it != end; ++it)                                             \
                m_merge_aux[it->m_is_true][it->m_idx] = null_theory_var;        \
        }
        DEBUG_CODE(for (unsigned i = 0; i < bv_size; i++) { SASSERT(m_merge_aux[0][i] == null_theory_var || m_merge_aux[1][i] == null_theory_var); });
        // save info about bits1
        zero_one_bits::iterator it  = bits1.begin();
        zero_one_bits::iterator end = bits1.end();
        for (; it != end; ++it)
            m_merge_aux[it->m_is_true][it->m_idx] = it->m_owner;
        // check if bits2 is consistent with bits1, and copy new bits to bits1
        it  = bits2.begin();
        end = bits2.end();
        for (; it != end; ++it) {
            theory_var v2 = it->m_owner;
            theory_var v1 = m_merge_aux[!it->m_is_true][it->m_idx];
            if (v1 != null_theory_var) {
                // conflict was detected ... v1 and v2 have complementary bits
                SASSERT(m_bits[v1][it->m_idx] == ~(m_bits[v2][it->m_idx]));
                mk_new_diseq_axiom(v1, v2, it->m_idx);
                RESET_MERGET_AUX();
                return false;
            }
            if (m_merge_aux[it->m_is_true][it->m_idx] == null_theory_var) {
                // copy missing variable to bits1
                bits1.push_back(*it);
            }
        }
        // reset m_merge_aux vector
        RESET_MERGET_AUX();
        DEBUG_CODE(for (unsigned i = 0; i < bv_size; i++) { SASSERT(m_merge_aux[0][i] == null_theory_var || m_merge_aux[1][i] == null_theory_var); });
        return true;
    }

    class bit_eq_justification : public justification {
        enode *   m_v1;
        enode *   m_v2;
        theory_id m_th_id; // TODO: steal 4 bits from each one of the following literas and use them to represent the th_id.
        literal   m_consequent;
        literal   m_antecedent;
    public:
        bit_eq_justification(theory_id th_id, enode * v1, enode * v2, literal c, literal a):
            m_v1(v1), m_v2(v2), m_th_id(th_id), m_consequent(c), m_antecedent(a) {}

        virtual void get_antecedents(conflict_resolution & cr) {
            cr.mark_eq(m_v1, m_v2);
            if (m_antecedent.var() != true_bool_var)
                cr.mark_literal(m_antecedent);
        }

        virtual proof * mk_proof(conflict_resolution & cr) {
            bool visited = true;
            ptr_buffer<proof> prs;
            proof * pr = cr.get_proof(m_v1, m_v2);
            if (pr)
                prs.push_back(pr);
            else 
                visited = false;
            if (m_antecedent.var() != true_bool_var) {
                proof * pr = cr.get_proof(m_antecedent);
                if (pr)
                    prs.push_back(pr);
                else
                    visited = false;
            }
            if (!visited)
                return 0;
            context & ctx = cr.get_context();
            ast_manager & m = cr.get_manager();
            expr_ref fact(m);
            ctx.literal2expr(m_consequent, fact);
            return m.mk_th_lemma(get_from_theory(), fact, prs.size(), prs.c_ptr());
        }

        virtual theory_id get_from_theory() const {
            return m_th_id;
        }

        virtual char const * get_name() const { return "bv-bit-eq"; }
    };

    inline justification * theory_bv::mk_bit_eq_justification(theory_var v1, theory_var v2, literal consequent, literal antecedent) {
        return get_context().mk_justification(bit_eq_justification(get_id(), get_enode(v1), get_enode(v2), consequent, antecedent));
    }

    void theory_bv::unmerge_eh(theory_var v1, theory_var v2) {
        // v1 was the root of the equivalence class
        // I must remove the zero_one_bits that are from v2.

        // REMARK: it is unsafe to invoke check_zero_one_bits, since
        // the enode associated with v1 and v2 may have already been
        // deleted. 
        //
        // The logical context trail_stack is popped before
        // the theories pop_scope_eh is invoked.

        zero_one_bits & bits = m_zero_one_bits[v1]; 
        if (bits.empty()) {
            // SASSERT(check_zero_one_bits(v1));
            // SASSERT(check_zero_one_bits(v2));
            return;
        }
        unsigned j  = bits.size();
        while (j > 0) {
            --j;
            zero_one_bit & bit = bits[j];
            if (find(bit.m_owner) == v1) {
                bits.shrink(j+1);
                // SASSERT(check_zero_one_bits(v1));
                // SASSERT(check_zero_one_bits(v2));
                return;
            }
        }
        bits.shrink(0);
        // SASSERT(check_zero_one_bits(v1));
        // SASSERT(check_zero_one_bits(v2));
    }

    void theory_bv::init_model(model_generator & m) {
        m_factory = alloc(bv_factory, get_manager());
        m.register_factory(m_factory);
    }

    model_value_proc * theory_bv::mk_value(enode * n, model_generator & mg) {
        numeral val;
        theory_var v = n->get_th_var(get_id());
        SASSERT(v != null_theory_var);
#ifdef Z3DEBUG
        bool r = 
#endif
        get_fixed_value(v, val);
        SASSERT(r);
        return alloc(expr_wrapper_proc, m_factory->mk_value(val, get_bv_size(v)));
    }

    void theory_bv::display_var(std::ostream & out, theory_var v) const {
        out << "v";
        out.width(4);
        out << std::left << v;
        out << " #";
        out.width(4);
        out << get_enode(v)->get_owner_id() << " -> #";
        out.width(4);
        out << get_enode(find(v))->get_owner_id();
        out << std::right << ", bits:";
        context & ctx = get_context();
        literal_vector const & bits = m_bits[v];
        literal_vector::const_iterator it  = bits.begin();
        literal_vector::const_iterator end = bits.end();
        for (; it != end; ++it) {
            out << " ";
            ctx.display_literal(out, *it);
        }
        numeral val;
        if (get_fixed_value(v, val))
            out << ", value: " << val;
        out << "\n";
    }

    void theory_bv::display_bit_atom(std::ostream & out, bool_var v, bit_atom const * a) const {
        context & ctx = get_context();
        out << "#" << ctx.bool_var2expr(v)->get_id() << " ->";
        var_pos_occ * curr = a->m_occs;
        while (curr) {
            out << " #" << get_enode(curr->m_var)->get_owner_id() << "[" << curr->m_idx << "]";
            curr = curr->m_next;
        }
        out << "\n";
    }

    void theory_bv::display_atoms(std::ostream & out) const {
        out << "atoms:\n";
        context & ctx = get_context();
        unsigned num  = ctx.get_num_bool_vars();
        for (unsigned v = 0; v < num; v++) {
            atom * a = get_bv2a(v);
            if (a && a->is_bit())
                display_bit_atom(out, v, static_cast<bit_atom*>(a));
        }
    }

    void theory_bv::display(std::ostream & out) const {
        out << "Theory bv:\n";
        unsigned num_vars = get_num_vars();
        for (unsigned v = 0; v < num_vars; v++) {
            display_var(out, v);
        }
        display_atoms(out);
    }

    void theory_bv::collect_statistics(::statistics & st) const {
        st.update("bv conflicts", m_stats.m_num_conflicts);
        st.update("bv diseqs", m_stats.m_num_diseq_static);
        st.update("bv dynamic diseqs", m_stats.m_num_diseq_dynamic);
        st.update("bv bit2core", m_stats.m_num_bit2core);
        st.update("bv->core eq", m_stats.m_num_th2core_eq);
    }

#ifdef Z3DEBUG
    bool theory_bv::check_assignment(theory_var v) const {
        context & ctx                 = get_context();
        if (!is_root(v))
            return true;
        if (!ctx.is_relevant(get_enode(v))) {
            return true;
        }

        theory_var v2                 = v;
        literal_vector const & bits2  = m_bits[v2];
        theory_var v1                 = v2;
        do {
            literal_vector const & bits1   = m_bits[v1];
            SASSERT(bits1.size() == bits2.size());
            unsigned sz = bits1.size();
            for (unsigned i = 0; i < sz; i++) {
                literal bit1 = bits1[i];
                literal bit2 = bits2[i];
                lbool val1   = ctx.get_assignment(bit1);
                lbool val2   = ctx.get_assignment(bit2);
                CTRACE("bv_bug", val1 != val2, 
                       tout << "equivalence class is inconsistent, i: " << i << "\n";
                       display_var(tout, v1);
                       display_var(tout, v2);
                       tout << "val1: " << val1 << " lvl: " << ctx.get_assign_level(bit1.var()) << " bit " << bit1 << "\n";
                       tout << "val2: " << val2 << " lvl: " << ctx.get_assign_level(bit2.var()) << " bit " << bit2 << "\n";);
                SASSERT(val1 == val2);
            }
            SASSERT(ctx.is_relevant(get_enode(v1)));
            v1 = next(v1);
        }
        while (v1 != v);
        return true;
    }

    /**
       \brief Check whether m_zero_one_bits is an accurate summary of the bits in the 
       equivalence class rooted by v.
       
       \remark The method does nothing if v is not the root of the equivalence class.
    */
    bool theory_bv::check_zero_one_bits(theory_var v) const {
        if (get_context().inconsistent())
            return true; // property is only valid if the context is not in a conflict.
        if (is_root(v) && is_bv(v)) {
            svector<bool> bits[2];
            unsigned      num_bits = 0;
            unsigned      bv_sz    = get_bv_size(v);
            bits[0].resize(bv_sz, false);
            bits[1].resize(bv_sz, false);
            theory_var curr = v;
            do {
                literal_vector const & lits = m_bits[curr];
                for (unsigned i = 0; i < lits.size(); i++) {
                    literal l = lits[i];
                    if (l.var() == true_bool_var) {
                        unsigned is_true = (l == true_literal);
                        SASSERT(!bits[!is_true][i]); // no complementary bits
                        if (!bits[is_true][i]) {
                            bits[is_true][i] = true;
                            num_bits++;
                        }
                    }
                }
                curr = next(curr);
            }
            while (curr != v);

            zero_one_bits const & _bits = m_zero_one_bits[v];
            SASSERT(_bits.size() == num_bits);
            svector<bool> already_found;
            already_found.resize(bv_sz, false);
            zero_one_bits::const_iterator it  = _bits.begin();
            zero_one_bits::const_iterator end = _bits.end();
            for (; it != end; ++it) {
                SASSERT(find(it->m_owner) == v);
                SASSERT(bits[it->m_is_true][it->m_idx]);
                SASSERT(!already_found[it->m_idx]);
                already_found[it->m_idx] = true;
            }
        }
        return true;
    }

    bool theory_bv::check_invariant() const {
        unsigned num = get_num_vars();
        for (unsigned v = 0; v < num; v++) {
            check_assignment(v);
            check_zero_one_bits(v);
        }
        return true;
    }

#endif

};
