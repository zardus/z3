/*++
Copyright (c) 2013 Microsoft Corporation

Module Name:

    weighted_maxsat.h

Abstract:
    Weighted MAXSAT module

Author:

    Anh-Dung Phan (t-anphan) 2013-10-16

Notes:

--*/

#include "weighted_maxsat.h"
#include "smt_theory.h"
#include "smt_context.h"
#include "ast_pp.h"

namespace smt {

    class theory_weighted_maxsat : public theory {
        app_ref_vector           m_vars;
        expr_ref_vector          m_fmls;
        vector<rational>         m_weights;    // weights of theory variables.
        svector<theory_var>      m_costs;      // set of asserted theory variables
        svector<theory_var>      m_cost_save;  // set of asserted theory variables
        rational                 m_cost;       // current sum of asserted costs
        rational                 m_min_cost;   // current minimal cost assignment.
        u_map<theory_var>        m_bool2var;   // bool_var -> theory_var
        u_map<bool_var>          m_var2bool;   // theory_var -> bool_var
    public:
        theory_weighted_maxsat(ast_manager& m):
            theory(m.mk_family_id("weighted_maxsat")),
            m_vars(m),
            m_fmls(m)
        {}

        /**
           \brief return the complement of variables that are currently assigned.
        */
        void get_assignment(expr_ref_vector& result) {
            result.reset();
            std::sort(m_cost_save.begin(), m_cost_save.end());
            for (unsigned i = 0, j = 0; i < m_vars.size(); ++i) {
                if (j < m_cost_save.size() && m_cost_save[j] == i) {
                    ++j;
                }
                else {
                    result.push_back(m_fmls[i].get());
                }
            }
        }

        virtual void init_search_eh() {
            context & ctx = get_context();
            ast_manager& m = get_manager();
            for (unsigned i = 0; i < m_vars.size(); ++i) {
                app* var = m_vars[i].get();  
                bool_var bv;
                enode* x;
                if (!ctx.e_internalized(var)) {
                    x = ctx.mk_enode(var, false, true, true);
                }
                else {
                    x = ctx.get_enode(var);
                }
                if (ctx.b_internalized(var)) {
                    bv = ctx.get_bool_var(var);
                }
                else {
                    bv = ctx.mk_bool_var(var);
                }
                ctx.set_var_theory(bv, get_id());
                ctx.set_enode_flag(bv, true);
                theory_var v = mk_var(x);
                ctx.attach_th_var(x, this, v);
                m_bool2var.insert(bv, v);
                m_var2bool.insert(v, bv);
            }
        }

        void assert_weighted(expr* fml, rational const& w) {
            context & ctx = get_context();
            ast_manager& m = get_manager();
            app_ref var(m), wfml(m);
            var = m.mk_fresh_const("w", m.mk_bool_sort());
            wfml = m.mk_or(var, fml);
            ctx.assert_expr(wfml);
            m_weights.push_back(w);
            m_vars.push_back(var);
            m_fmls.push_back(fml);
            m_min_cost += w;
        }

        virtual void assign_eh(bool_var v, bool is_true) {
            IF_VERBOSE(3, verbose_stream() << "Assign " << v << " " << is_true << "\n";);
            if (is_true) {
                context& ctx = get_context();
                theory_var tv = m_bool2var[v];
                rational const& w = m_weights[tv];
                ctx.push_trail(value_trail<context, rational>(m_cost));
                ctx.push_trail(push_back_vector<context, svector<theory_var> >(m_costs));
                m_cost += w;
                m_costs.push_back(tv);
                if (m_cost > m_min_cost) {
                    block();
                }
            }
        }

        virtual final_check_status final_check_eh() {
            if (block()) {
                return FC_CONTINUE;
            }
            else {
                return FC_DONE;
            }
        }

        virtual bool use_diseqs() const { 
            return false;
        }

        virtual bool build_models() const { 
            return false;
        }


        virtual void reset_eh() {
            theory::reset_eh();
            m_vars.reset();
            m_weights.reset();
            m_costs.reset();
            m_cost.reset();
            m_min_cost.reset();
            m_cost_save.reset();
        }

        virtual theory * mk_fresh(context * new_ctx) { UNREACHABLE(); return 0;} // TBD
        virtual bool internalize_atom(app * atom, bool gate_ctx) { return false; }
        virtual bool internalize_term(app * term) { return false; }
        virtual void new_eq_eh(theory_var v1, theory_var v2) { }
        virtual void new_diseq_eh(theory_var v1, theory_var v2) { }


    private:
       
        class compare_cost {
            theory_weighted_maxsat& m_th;
        public:
            compare_cost(theory_weighted_maxsat& t):m_th(t) {}
            bool operator() (theory_var v, theory_var w) const { 
                return m_th.m_weights[v] > m_th.m_weights[w]; 
            }
        };

        bool block() {
            ast_manager& m = get_manager();
            context& ctx = get_context();
            literal_vector lits;
            compare_cost compare_cost(*this);
            svector<theory_var> costs(m_costs);
            std::sort(costs.begin(), costs.end(), compare_cost);
            rational weight(0);
            for (unsigned i = 0; i < costs.size() && weight < m_min_cost; ++i) {
                weight += m_weights[costs[i]];
                lits.push_back(~literal(m_var2bool[costs[i]]));
            }
            IF_VERBOSE(2, verbose_stream() << "block: " << m_costs.size() << " " << lits.size() << " " << m_min_cost << "\n";);

            ctx.mk_th_axiom(get_id(), lits.size(), lits.c_ptr());
            if (m_cost < m_min_cost) {
                m_min_cost = weight;
                m_cost_save.reset();
                m_cost_save.append(m_costs);
            }
            return !lits.empty();
        }        
    };

}

namespace opt {


    /**
       Takes solver with hard constraints added.
       Returns a maximal satisfying subset of weighted soft_constraints
       that are still consistent with the solver state.
    */
    
    lbool weighted_maxsat(opt_solver& s, expr_ref_vector& soft_constraints, vector<rational> const& weights) {
        ast_manager& m = soft_constraints.get_manager();
        smt::context& ctx = s.get_context();                        
        smt::theory_id th_id = m.get_family_id("weighted_maxsat");
        smt::theory* th = ctx.get_theory(th_id);               
        if (!th) {
            th = alloc(smt::theory_weighted_maxsat, m);
            ctx.register_plugin(th);
        }
        smt::theory_weighted_maxsat* wth = dynamic_cast<smt::theory_weighted_maxsat*>(th);
        for (unsigned i = 0; i < soft_constraints.size(); ++i) {
            wth->assert_weighted(soft_constraints[i].get(), weights[i]);
        }
        lbool result = s.check_sat_core(0,0);
        wth->get_assignment(soft_constraints);
        if (!soft_constraints.empty() && result == l_false) {
            result = l_true;
        }
        return result;
    }
};

