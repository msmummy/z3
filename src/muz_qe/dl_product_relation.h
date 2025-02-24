/*++
Copyright (c) 2010 Microsoft Corporation

Module Name:

    dl_product_relation.h

Abstract:

    A Relation relation combinator.

Author:

    Nikolaj Bjorner (nbjorner) 2010-4-11

Revision History:

--*/
#ifndef _DL_PRODUCT_RELATION_H_
#define _DL_PRODUCT_RELATION_H_


#include "dl_context.h"

namespace datalog {

    class product_relation;

    class product_relation_plugin : public relation_plugin {
        friend class product_relation;
    public:
        typedef svector<family_id> rel_spec;
    private:
        class join_fn;
        class transform_fn;
        class mutator_fn;
        class aligned_union_fn;
        class unaligned_union_fn;
        class single_non_transparent_src_union_fn;
        class filter_equal_fn;
        class filter_identical_fn;
        class filter_interpreted_fn;
        struct fid_hash {
            typedef family_id data;
            unsigned operator()(data x) const { return static_cast<unsigned>(x); }
        };

        rel_spec_store<rel_spec, svector_hash<fid_hash> > m_spec_store;

        family_id get_relation_kind(const product_relation & r);

        bool is_product_relation(relation_base * r) { return r->get_plugin().is_product_relation(); }

    public:
        static product_relation_plugin& get_plugin(relation_manager & rmgr);

        product_relation_plugin(relation_manager& m);

        virtual void initialize(family_id fid);

        virtual bool can_handle_signature(const relation_signature & s);
        virtual bool can_handle_signature(const relation_signature & s, family_id kind);

        static symbol get_name() { return symbol("product_relation"); }

        family_id get_relation_kind(const relation_signature & sig, const rel_spec & spec);

        virtual relation_base * mk_empty(const relation_signature & s);
        virtual relation_base * mk_empty(const relation_signature & s, family_id kind);

        virtual relation_base * mk_full(func_decl* p, const relation_signature & s);
        virtual relation_base * mk_full(func_decl* p, const relation_signature & s, family_id kind);

    protected:
        virtual relation_join_fn * mk_join_fn(const relation_base & t1, const relation_base & t2,
            unsigned col_cnt, const unsigned * cols1, const unsigned * cols2);
        virtual relation_transformer_fn * mk_project_fn(const relation_base & t, unsigned col_cnt, 
            const unsigned * removed_cols);
        virtual relation_transformer_fn * mk_rename_fn(const relation_base & t, unsigned permutation_cycle_len, 
            const unsigned * permutation_cycle);
        virtual relation_union_fn * mk_union_fn(const relation_base & tgt, const relation_base & src, 
            const relation_base * delta);
        virtual relation_union_fn * mk_widen_fn(const relation_base & tgt, const relation_base & src, 
            const relation_base * delta);
        virtual relation_mutator_fn * mk_filter_identical_fn(const relation_base & t, unsigned col_cnt, 
            const unsigned * identical_cols);
        virtual relation_mutator_fn * mk_filter_equal_fn(const relation_base & t, const relation_element & value, 
            unsigned col);
        virtual relation_mutator_fn * mk_filter_interpreted_fn(const relation_base & t, app * condition);

        static bool is_product_relation(relation_base const& r);

    private:
        static product_relation& get(relation_base& r);
        static product_relation const & get(relation_base const& r);   
        static product_relation* get(relation_base* r);
        static product_relation const* get(relation_base const* r);

        relation_union_fn * mk_union_w_fn(const relation_base & tgt, const relation_base & src, 
            const relation_base * delta, bool is_widen);

        bool are_aligned(const product_relation& r1, const product_relation& r2);
        static void get_common_spec(const ptr_vector<const product_relation> & rels, rel_spec & res);
    };

    
    class product_relation : public relation_base {
        friend class product_relation_plugin;

        friend class product_relation_plugin::join_fn;
        friend class product_relation_plugin::transform_fn;
        friend class product_relation_plugin::mutator_fn;
        friend class product_relation_plugin::aligned_union_fn;
        friend class product_relation_plugin::unaligned_union_fn;
        friend class product_relation_plugin::single_non_transparent_src_union_fn;
        friend class product_relation_plugin::filter_equal_fn;
        friend class product_relation_plugin::filter_identical_fn;
        friend class product_relation_plugin::filter_interpreted_fn;


        typedef product_relation_plugin::rel_spec rel_spec;

        /**
           If m_relations is empty, value of this determines whether the relation is empty or full.
        */
        bool m_default_empty;

        /**
           There must not be two relations of the same kind
        */
        ptr_vector<relation_base> m_relations;

        /**
           Array of kinds of inner relations.
           
           If two product relations have equal signature and specification, their
           m_relations arrays contain corresponding relations at the same indexes.

           The value returned by get_kind() depends uniquely on the specification.
        */
        rel_spec m_spec;

        /**
            \brief Ensure the kind assigned to this relation reflects the types of inner relations.
        */
        void ensure_correct_kind();
        /**
           The current specification must be a subset of the new one.
        */
        void convert_spec(const rel_spec & spec);
    public:
        product_relation(product_relation_plugin& p, relation_signature const& s);
        product_relation(product_relation_plugin& p, relation_signature const& s, unsigned num_relations, relation_base** relations);

        ~product_relation();

        virtual bool empty() const;
        virtual void add_fact(const relation_fact & f);
        virtual bool contains_fact(const relation_fact & f) const;
        virtual product_relation * clone() const;
        virtual product_relation * complement(func_decl* p) const;
        virtual void display(std::ostream & out) const;
        virtual void to_formula(expr_ref& fml) const;
        product_relation_plugin& get_plugin() const; 

        unsigned size() const { return m_relations.size(); }
        relation_base& operator[](unsigned i) const { return *m_relations[i]; }

        /**
           If all relations except one are sieve_relations with no inner columns,
           return true and into \c idx assign index of that relation. Otherwise return 
           false.
        */
        bool try_get_single_non_transparent(unsigned & idx) const;
    };
        
};

#endif 

