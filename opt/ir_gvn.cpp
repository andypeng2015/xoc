/*@
XOC Release License

Copyright (c) 2013-2014, Alibaba Group, All rights reserved.

    compiler@aliexpress.com

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the Su Zhenyu nor the names of its contributors
      may be used to endorse or promote products derived from this software
      without specific prior written permission.

THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS
BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

author: Su Zhenyu
@*/
#include "cominc.h"
#include "comopt.h"

namespace xoc {

class VNTypeDesc {
public:
    VN_TYPE vt;
    CHAR const* vt_name;

public:
    static CHAR const* getVTName(VN_TYPE vt);
};

static VNTypeDesc g_vntype_desc[] = {
    { VN_UNKNOWN, "", },
    { VN_OP, "OP", },
    { VN_VAR, "VAR", },
    { VN_INT, "INT", },
    { VN_FP, "FP", },
    { VN_STR, "STR", },
    { VN_MC_INT, "MC_INT", },
    { VN_NUM, "", },
};


CHAR const* VNTypeDesc::getVTName(VN_TYPE vt)
{
    ASSERT0(vt > VN_UNKNOWN && vt < VN_NUM);
    return g_vntype_desc[vt].vt_name;
}


//
//START GVN
//
GVN::GVN(Region * rg) : Pass(rg)
{
    ASSERT0(rg != nullptr);
    m_md_sys = m_rg->getMDSystem();
    m_du = m_rg->getDUMgr();
    m_tm = m_rg->getTypeMgr();
    m_cfg = m_rg->getCFG();
    ASSERT0(m_cfg && m_du && m_md_sys && m_tm);
    m_is_vn_fp = false;
    m_is_comp_lda_string = false;
    m_is_alloc_livein_vn = false;
    m_pool = nullptr;
    init();
}


GVN::~GVN()
{
    destroy();
}


VN * GVN::allocVN()
{
    VN * vn = m_free_lst.remove_head();
    if (vn == nullptr) {
        vn = (VN*)xmalloc(sizeof(VN));
    } else {
        vn->clean();
    }
    VN_id(vn) = m_vn_count++;
    return vn;
}


bool GVN::isUnary(IR_CODE irt) const
{
    switch (irt) {
    SWITCH_CASE_UNA:
        return true;
    default: break;
    }
    return false;
}


bool GVN::isBinary(IR_CODE irt) const
{
    //Regard LDA as binary op.
    return xoc::isBinaryOp(irt) || irt == IR_LDA || irt == IR_GETELEM;
}


bool GVN::isTriple(IR_CODE irt) const
{
    return irt == IR_ILD || irt == IR_SETELEM || irt == IR_SELECT;
}


bool GVN::isQuad(IR_CODE irt) const
{
    return irt == IR_ARRAY;
}


void GVN::init()
{
    if (m_pool != nullptr) { return; }
    m_vn_count = 1;
    m_zero_vn = nullptr;
    m_mc_zero_vn = nullptr;
    m_mdssamgr = nullptr;
    m_prssamgr = nullptr;
    m_oc = nullptr;
    List<IRBB*> * bbl = m_rg->getBBList();
    UINT n = 0;
    for (IRBB * bb = bbl->get_head(); bb != nullptr; bb = bbl->get_next()) {
        n += bb->getNumOfIR();
    }
    m_stmt2domdef.init(MAX(4, xcom::getNearestPowerOf2(n/2)));
    m_pool = smpoolCreate(sizeof(VN) * 4, MEM_COMM);
}


void GVN::destroy()
{
    if (m_pool == nullptr) { return; }
    destroyLocalUsed();
    smpoolDelete(m_pool);
    m_pool = nullptr;
}


void GVN::destroyLocalUsed()
{
    m_md2vn.destroy(); //will be initialized dynamically.
    m_ll2vn.destroy(); //will be initialized dynamically.
    m_fp2vn.destroy(); //will be initialized dynamically.
    m_str2vn.destroy(); //will be initialized dynamically.

    m_def2ildtab.destroy();
    m_def2ildtab.init();

    m_def2arrtab.destroy();
    m_def2arrtab.init();

    m_def2sctab.destroy();
    m_def2sctab.init();

    UINT bucket_size = m_stmt2domdef.get_bucket_size();
    m_stmt2domdef.destroy();
    m_stmt2domdef.init(bucket_size);

    for (Tab1 * v = m_tab_lst.get_head();
         v != nullptr; v = m_tab_lst.get_next()) {
        delete v;
    }
    m_tab_lst.destroy();
    m_tab_lst.init();

    m_irc_vec.destroy();
    m_irc_vec.init();
}


void GVN::cleanIRTreeVN(IR const* ir)
{
    if (ir == nullptr) { return; }
    ASSERTN(!ir->is_undef(), ("ir has been freed"));
    for (INT i = 0; i < IR_MAX_KID_NUM(ir); i++) {
        IR * kid = ir->getKid(i);
        if (kid != nullptr) {
            cleanIRTreeVN(kid);
        }
    }
    //Do NOT insert ir into mapping table if it does not have a VN.
    setVNIfFind(ir, nullptr);
}


void GVN::cleanIR2VN()
{
    xcom::TTab<UINT> inlst;
    IR2VNIter it;
    VN const* x;
    for (m_ir2vn.get_first(it, &x); x != nullptr; m_ir2vn.get_next(it, &x)) {
        if (!inlst.find(x->id())) {
            inlst.append(x->id());
            m_free_lst.append_tail(const_cast<VN*>(x));
        }
    }
    m_ir2vn.clean();
    MDPhi2VNIter it2;
    for (m_mdphi2vn.get_first(it2, &x);
         x != nullptr; m_mdphi2vn.get_next(it2, &x)) {
        if (!inlst.find(x->id())) {
            inlst.append(x->id());
            m_free_lst.append_tail(const_cast<VN*>(x));
        }
    }
    m_mdphi2vn.clean();
}


void GVN::clean()
{
    m_vn_count = 1;
    m_zero_vn = nullptr;
    m_mc_zero_vn = nullptr;
    destroyLocalUsed();
    cleanIR2VN();
}


void GVN::dumpVNHash() const
{
    Tab2Iter it2;
    Tab3Iter it3;
    for (VecIdx i = 0; i <= m_irc_vec.get_last_idx(); i++) {
        if (isBinary((IR_CODE)i)) {
            Tab2 * tab2 = (Tab2*)m_irc_vec.get(i);
            if (tab2 == nullptr) { continue; }

            it2.clean();
            Tab1 * tab1;
            for (tab2->get_first(it2, &tab1); tab1 != nullptr;
                 tab2->get_next(it2, &tab1)) {
                if (tab1 == nullptr) { continue; }
            }
            continue;
        }

        if (isUnary((IR_CODE)i)) {
            Tab1 * tab1 = (Tab1*)m_irc_vec.get(i);
            if (tab1 == nullptr) { continue; }
            continue;
        }

        if (isTriple((IR_CODE)i)) {
            Tab3 * tab3 = (Tab3*)m_irc_vec.get(i);
            if (tab3 == nullptr) { continue; }

            it3.clean();
            Tab2 * tab2;
            for (tab3->get_first(it3, &tab2); tab2 != nullptr;
                 tab3->get_next(it3, &tab2)) {
            }
            continue;
        }

        if (isQuad((IR_CODE)i)) {
            continue;
        }

        ASSERT0(m_irc_vec.get(i) == nullptr);
    }
}


VN const* GVN::registerVNviaMD(MD const* md)
{
    if (m_md2vn.get_bucket_size() == 0) {
        m_md2vn.init(10); //to be evaluated
    }
    VN * x = m_md2vn.get(md);
    if (x == nullptr) {
        x = allocVN();
        VN_type(x) = VN_VAR;
        m_md2vn.set(md, x);
    }
    return x;
}


VN const* GVN::registerVNviaINT(LONGLONG v)
{
    if (v == 0) {
        if (m_zero_vn == nullptr) {
            m_zero_vn = allocVN();
            VN_type(m_zero_vn) = VN_INT;
            VN_int_val(m_zero_vn) = 0;
        }
        return m_zero_vn;
    }

    if (m_ll2vn.get_bucket_size() == 0) {
        m_ll2vn.init(10); //To be reevaluated
    }

    VN * vn = m_ll2vn.get(v);
    if (vn != nullptr) {
        return vn;
    }

    vn = allocVN();
    VN_type(vn) = VN_INT;
    VN_int_val(vn) = v;
    m_ll2vn.set(v, vn);
    return vn;
}


VN const* GVN::registerVNviaMC(LONGLONG v)
{
    if (v == 0) {
        if (m_mc_zero_vn == nullptr) {
            m_mc_zero_vn = allocVN();
            VN_type(m_mc_zero_vn) = VN_MC_INT;
            VN_int_val(m_mc_zero_vn) = 0;
        }
        return m_mc_zero_vn;
    }

    if (m_llmc2vn.get_bucket_size() == 0) {
        m_llmc2vn.init(10/*TO reevaluate*/);
    }

    VN * vn = m_llmc2vn.get(v);
    if (vn != nullptr) {
        return vn;
    }

    vn = allocVN();
    VN_type(vn) = VN_MC_INT;
    VN_int_val(vn) = v;
    m_llmc2vn.set(v, vn);
    return vn;
}


VN const* GVN::registerVNviaSTR(Sym const* v)
{
    if (m_str2vn.get_bucket_size() == 0) {
        m_str2vn.init(16/*TO reevaluate*/);
    }

    VN * vn = m_str2vn.get(v);
    if (vn != nullptr) {
        return vn;
    }
    vn = allocVN();
    VN_type(vn) = VN_STR;
    VN_str_val(vn) = v;
    m_str2vn.set(v, vn);
    return vn;
}


VN const* GVN::registerVNviaFP(double v)
{
    if (m_fp2vn.get_bucket_size() == 0) {
        m_fp2vn.init(10/*TO reevaluate*/);
    }
    VN * vn = m_fp2vn.get(v);
    if (vn != nullptr) {
        return vn;
    }
    vn = allocVN();
    VN_type(vn) = VN_FP;
    VN_fp_val(vn) = v;
    m_fp2vn.set(v, vn);
    return vn;
}


VN const* GVN::registerUnaVN(IR_CODE irt, VN const* v0)
{
    ASSERT0(isUnary(irt));
    Tab1 * tab1 = (Tab1*)m_irc_vec.get(irt);
    if (tab1 == nullptr) {
        tab1 = new Tab1();
        m_tab_lst.append_tail(tab1);
        m_irc_vec.set(irt, (Tab2*)tab1);
    }

    VN * res = tab1->get(VN_id(v0));
    if (res == nullptr) {
        res = allocVN();
        VN_type(res) = VN_OP;
        VN_op(res) = irt;
        tab1->set(VN_id(v0), res);
    }
    return res;
}


VN const* GVN::registerBinVN(IR_CODE irt, VN const* v0, VN const* v1)
{
    ASSERT0(v0 && v1);
    ASSERT0(isBinary(irt));
    if (xoc::isCommutative(irt) && (VN_id(v0) > VN_id(v1))) {
        return registerBinVN(irt, v1, v0);
    }
    if (irt == IR_GT) {
        return registerBinVN(IR_LT, v1, v0);
    }
    if (irt == IR_GE) {
        return registerBinVN(IR_LE, v1, v0);
    }

    Tab2 * tab2 = (Tab2*)m_irc_vec.get(irt);
    if (tab2 == nullptr) {
        tab2 = new Tab2();
        m_tab_lst.append_tail((Tab1*)tab2);
        m_irc_vec.set(irt, tab2);
    }

    Tab1 * tab1 = tab2->get(VN_id(v0));
    if (tab1 == nullptr) {
        tab1 = new Tab1();
        m_tab_lst.append_tail((Tab1*)tab1);
        tab2->set(VN_id(v0), tab1);
    }

    VN * res = tab1->get(VN_id(v1));
    if (res == nullptr) {
        res = allocVN();
        VN_type(res) = VN_OP;
        VN_op(res) = irt;
        tab1->set(VN_id(v1), res);
    }
    return res;
}


VN const* GVN::registerTripleVN(IR_CODE irt, VN const* v0, VN const* v1,
                                VN const* v2)
{
    ASSERT0(v0 && v1 && v2);
    ASSERT0(isTriple(irt));
    Tab3 * tab3 = (Tab3*)m_irc_vec.get(irt);
    if (tab3 == nullptr) {
        tab3 = new Tab3();
        m_tab_lst.append_tail((Tab1*)tab3);
        m_irc_vec.set(irt, (Tab2*)tab3);
    }

    Tab2 * tab2 = tab3->get(VN_id(v0));
    if (tab2 == nullptr) {
        tab2 = new Tab2();
        m_tab_lst.append_tail((Tab1*)tab2);
        tab3->set(VN_id(v0), tab2);
    }

    Tab1 * tab1 = tab2->get(VN_id(v1));
    if (tab1 == nullptr) {
        tab1 = new Tab1();
        m_tab_lst.append_tail((Tab1*)tab1);
        tab2->set(VN_id(v1), tab1);
    }

    VN * res = tab1->get(VN_id(v2));
    if (res == nullptr) {
        res = allocVN();
        VN_type(res) = VN_OP;
        VN_op(res) = irt;
        tab1->set(VN_id(v2), res);
    }
    return res;
}


VN const* GVN::registerQuadVN(IR_CODE irt, VN const* v0, VN const* v1,
                              VN const* v2, VN const* v3)
{
    ASSERT0(v0 && v1 && v2 && v3);
    ASSERT0(isQuad(irt));
    Tab4 * tab4 = (Tab4*)m_irc_vec.get(irt);
    if (tab4 == nullptr) {
        tab4 = new Tab4();
        m_tab_lst.append_tail((Tab1*)tab4);
        m_irc_vec.set(irt, (Tab2*)tab4);
    }

    Tab3 * tab3 = tab4->get(VN_id(v0));
    if (tab3 == nullptr) {
        tab3 = new Tab3();
        m_tab_lst.append_tail((Tab1*)tab3);
        tab4->set(VN_id(v0), tab3);
    }

    Tab2 * tab2 = tab3->get(VN_id(v1));
    if (tab2 == nullptr) {
        tab2 = new Tab2();
        m_tab_lst.append_tail((Tab1*)tab2);
        tab3->set(VN_id(v1), tab2);
    }

    Tab1 * tab1 = tab2->get(VN_id(v2));
    if (tab1 == nullptr) {
        tab1 = new Tab1();
        m_tab_lst.append_tail(tab1);
        tab2->set(VN_id(v2), tab1);
    }

    VN * res = tab1->get(VN_id(v3));
    if (res == nullptr) {
        res = allocVN();
        VN_type(res) = VN_OP;
        VN_op(res) = irt;
        tab1->set(VN_id(v3), res);
    }
    return res;
}


//Memory location may be parameter or global variable.
//'emd': exact md
VN const* GVN::allocLiveinVN(IR const* exp, MD const* emd, bool & change)
{
    VN const* x = getVN(exp);
    if (x == nullptr) {
        x = registerVNviaMD(emd);
        change = true;
        setVNOnce(exp, x);
    }
    return x;
}


//Only compute memory operation's vn.
VN const* GVN::computePR(IR const* exp, bool & change)
{
    SSAInfo * ssainfo = PR_ssainfo(exp);
    ASSERT0(exp->isReadPR() && ssainfo);
    IR const* def = ssainfo->getDef();
    if (def == nullptr) {
        ASSERT0(exp->getRefMD());
        if (isAllocLiveinVN() || exp->getRefMD()->is_restrict()) {
            return allocLiveinVN(exp, exp->getRefMD(), change);
        }
        return nullptr;
    }
    VN const* defvn = getVN(def);
    VN const* ux = getVN(exp);
    if (defvn != ux) {
        setVN(exp, defvn);
        change = true;
    }
    return defvn;
}


bool GVN::hasOverlappedDef(IR const* exp) const
{
    MD const* emd = exp->getExactRef();
    ASSERT0(emd);
    ASSERT0(exp && exp->is_exp());
    DUSet const* defset = exp->readDUSet();
    if (defset == nullptr) { return false; }

    //Check if some may-def or overlapped-def disrupts the emd.
    //Skip the DEF which has effect MD but does not overlapped
    //with emd.
    DUSetIter di = nullptr;
    UINT defcount = 0;
    for (BSIdx i = defset->get_first(&di);
         i != BS_UNDEF; i = defset->get_next(i, &di), defcount++) {
        IR const* dir = m_rg->getIR(i);
        ASSERT0(dir->is_stmt());
        MD const* xd = const_cast<IR*>(dir)->getMustRef();
        if (xd == nullptr) {
            MDSet const* xds = const_cast<IR*>(dir)->getMayRef();
            if (xds != nullptr && xds->is_contain(emd, m_rg)) {
                ASSERT0(getVN(exp) == nullptr);
                //exp's value is may defined, here we can not
                //determine if exp have an individual VN.
                return true;
            }
            continue;
        }

        if (xd == emd || xd->is_overlap(emd)) {
            ASSERT0(getVN(exp) == nullptr);
            //exp's value is overlapped with nonkilling DEF,
            //thus we can not determine if exp have an individual VN.
            return true;
        }
    }
    return false;
}


VN const* GVN::inferVNViaPhi(IR const* exp, IR const* phi, bool & change)
{
    VN const* phivn = getVN(phi);
    VN const* expvn = getVN(exp);
    if (phivn != expvn) {
        setVN(exp, phivn);
        change = true;
    }
    return phivn;
}


VN const* GVN::inferVNViaMDPhi(IR const* exp, MDPhi const* phi, bool & change)
{
    VN const* phivn = getVN(phi);
    VN const* expvn = getVN(exp);
    if (phivn != expvn) {
        setVN(exp, phivn);
        change = true;
    }
    return phivn;
}


//Infer the VN of 'exp' via killing def.
//exp: expression.
//kdef: killing-def.
VN const* GVN::inferVNViaKillingDef(IR const* exp, IR const* kdef,
                                    OUT bool & change)
{
    VN const* defvn = getVN(kdef);
    VN const* ux = getVN(exp);
    if (defvn != ux) {
        setVN(exp, defvn);
        change = true;
    }
    return defvn;
}


//Infer the VN of 'exp' via dominated-killing-def.
//exp: expression.
//kdef: killing-def.
VN const* GVN::inferVNViaDomKillingDef(IR const* exp, IR const* kdef,
                                       OUT bool & change)
{
    VN const* defvn = nullptr;
    //Infer exp's VN only if 'kdef' is the dom-def.
    IR const* exp_stmt = exp->getStmt();
    ASSERT0(exp_stmt->is_stmt());
    IRBB * b1 = kdef->getBB();
    IRBB * b2 = exp_stmt->getBB();
    ASSERT0(b1 && b2);
    if ((b1 != b2 && m_cfg->is_dom(b1->id(), b2->id())) ||
        (b1 == b2 && b1->is_dom(kdef, exp_stmt, true))) {
        defvn = getVN(kdef);
    }
    VN const* ux = getVN(exp);
    if (defvn != ux) {
        setVN(exp, defvn);
        change = true;
    }
    return defvn;
}


VN const* GVN::inferVNViaHint(IR const* exp, MD const* md, bool & change)
{
    ASSERT0(exp && md);
    if (isAllocLiveinVN() || md->is_restrict()) {
        return allocLiveinVN(exp, md, change);
    }
    return nullptr;
}


VN const* GVN::inferNonPRVNViaHint(IR const* exp, bool & change)
{
    return inferVNViaHint(exp, exp->getExactRef(), change);
}


VN const* GVN::inferPRVNViaHint(IR const* exp, bool & change)
{
    return inferVNViaHint(exp, exp->getMustRef(), change);
}


VN const* GVN::inferVNThroughCFG(IR const* exp, bool & change)
{
    ASSERT0(exp->getExactRef() || exp->isReadPR());
    //Prefer PRSSA and MDSSA DU.
    if (exp->isReadPR()) {
        if (!usePRSSADU()) { goto CLASSIC_DU; }
        ASSERT0(PR_ssainfo(exp));
        IR * d = PR_ssainfo(exp)->getDef();
        if (d != nullptr && d->is_phi()) {
            return inferVNViaPhi(exp, d, change);
        }
        return inferPRVNViaHint(exp, change);
    }
    if (exp->isMemRefNonPR()) {
        if (!useMDSSADU()) { goto CLASSIC_DU; }
        ASSERT0(m_mdssamgr->getMDSSAInfoIfAny(exp));
        MDDef const* d = m_mdssamgr->findMustMDDef(exp);
        if (d != nullptr && d->is_phi()) {
            return inferVNViaMDPhi(exp, (MDPhi const*)d, change);
        }
        return inferNonPRVNViaHint(exp, change);
    }
CLASSIC_DU:
    if (exp->isReadPR() && useClassicPRDU()) {
        if (hasOverlappedDef(exp)) {
            return nullptr;
        }
        return inferPRVNViaHint(exp, change);
    }
    if (exp->isMemRefNonPR() && useClassicNonPRDU()) {
        if (hasOverlappedDef(exp)) {
            return nullptr;
        }
        return inferNonPRVNViaHint(exp, change);
    }
    return nullptr;
}


//Only compute memory operation's vn.
VN const* GVN::computeExactMemory(IR const* exp, bool & change)
{
    ASSERT0(exp->isMemOpnd());
    MD const* emd = exp->getExactRef();
    if (emd == nullptr) { return nullptr; }
    IR const* kd = xoc::findKillingDef(exp, m_rg);
    if (kd != nullptr) {
        return inferVNViaDomKillingDef(exp, kd, change);
    }
    return inferVNThroughCFG(exp, change);
}


//Compute VN for ild according to anonymous domdef.
VN const* GVN::computeILoadByAnonDomDef(IR const* ild, VN const* mlvn,
                                        IR const* domdef, bool & change)
{
    ASSERT0(ild->is_ild() && m_du->isMayDef(domdef, ild, false));
    ILD_VNE2VN * vnexp_map = m_def2ildtab.get(domdef);
    UINT dtsz = ild->getTypeSize(m_tm);
    VNE_ILD vexp(mlvn->id(), ILD_ofst(ild), dtsz);
    //NOTE:
    //    foo();
    //    ild(v1); //s1
    //    goo();
    //    ild(v1); //s2
    //    vn of s1 should not same as s2.
    if (vnexp_map == nullptr) {
        vnexp_map = new ILD_VNE2VN(m_pool, 16); //bsize to be evaluate.
        m_def2ildtab.set(domdef, vnexp_map);
    }

    VN * ildvn = vnexp_map->get(&vexp);
    if (ildvn == nullptr) {
        ildvn = allocVN();
        VN_type(ildvn) = VN_OP;
        VN_op(ildvn) = IR_ILD;
        vnexp_map->setv((OBJTY)&vexp, ildvn);
    }

    setVN(ild, ildvn);
    change = true;
    return ildvn;
}


VN const* GVN::computeILoad(IR const* exp, bool & change)
{
    ASSERT0(exp->is_ild());
    VN const* mlvn = computeVN(ILD_base(exp), change);
    if (mlvn == nullptr) {
        ASSERT0(getVN(exp) == nullptr);
        ASSERT0(getVN(ILD_base(exp)) == nullptr);
        return nullptr;
    }

    VN const* evn = getVN(exp);
    if (evn != nullptr) { return evn; }

    evn = computeExactMemory(exp, change);
    if (evn != nullptr) { return evn; }

    DUSet const* du = exp->readDUSet();
    if (du == nullptr || du->get_elem_count() == 0) {
        VN const* v = registerTripleVN(IR_ILD, mlvn,
            registerVNviaINT(ILD_ofst(exp)),
            registerVNviaINT(exp->getTypeSize(m_tm)));
        setVNOnce(exp, v);
        return v;
    }

    IR const* exp_stmt = const_cast<IR*>(exp)->getStmt();
    IR const* domdef = m_stmt2domdef.get(exp_stmt);
    if (domdef == nullptr) {
        domdef = m_du->findNearestDomDef(exp, exp_stmt, du);
        if (domdef != nullptr) {
            m_stmt2domdef.set(exp_stmt, domdef);
        }
    }
    if (domdef == nullptr) {
        return nullptr;
    }
    if (domdef->getMustRef() == nullptr || exp->getMustRef() == nullptr ||
        !domdef->getMustRef()->is_exact() || !exp->getMustRef()->is_exact()) {
        return nullptr;
    }

    //ofst will be distinguished in computeILoadByAnonDomDef(), so
    //we do not need to differentiate the various offset of ild and ist.
    //if (domdef->is_ist() && IST_ofst(domdef) != ILD_ofst(exp)) {
    //    return nullptr;
    //}

    if (!domdef->is_ist() || domdef->is_starray() ||
        IST_ofst(domdef) != ILD_ofst(exp)) {
        return computeILoadByAnonDomDef(exp, mlvn, domdef, change);
    }

    //domdef is ist and the offset is matched.
    //Check if IR expression is match.
    VN const* mcvn = getVN(IST_base(domdef));
    if (mcvn == nullptr || mcvn != mlvn) {
        return nullptr;
    }
    VN const* uni_vn = getVN(domdef);
    if (uni_vn == nullptr) {
        uni_vn = registerTripleVN(IR_ILD, mlvn,
                                  registerVNviaINT(ILD_ofst(exp)),
                                  registerVNviaINT(exp->getTypeSize(m_tm)));
        setVNOnce(domdef, uni_vn);
    }
    setVNOnce(exp, uni_vn);
    change = true;
    return uni_vn;
}


void GVN::computeArrayAddrRef(IR const* ir, bool & change)
{
    ASSERT0(ir->is_starray());
    computeVN(ARR_base(ir), change);
    for (IR const* s = ARR_sub_list(ir); s != nullptr; s = s->get_next()) {
        computeVN(s, change);
    }
}


//Compute VN for array according to anonymous domdef.
VN const* GVN::computeArrayByAnonDomDef(IR const* arr, VN const* basevn,
                                        VN const* ofstvn, IR const* domdef,
                                        bool & change)
{
    ASSERT0(arr->is_array() && m_du->isMayDef(domdef, arr, false));
    ARR_VNE2VN * vnexp_map = m_def2arrtab.get(domdef);
    UINT dtsz = arr->getTypeSize(m_tm);
    VNE_ARR vexp(basevn->id(), ofstvn->id(), ARR_ofst(arr), dtsz);
    //NOTE:
    // foo();
    // array(v1); //s1
    // goo();
    // array(v1); //s2
    // vn of s1 should not same as s2.
    if (vnexp_map == nullptr) {
        vnexp_map = new ARR_VNE2VN(m_pool, 16); //bsize to be evaluated.
        m_def2arrtab.set(domdef, vnexp_map);
    }
    VN * vn = vnexp_map->get(&vexp);
    if (vn == nullptr) {
        vn = allocVN();
        VN_type(vn) = VN_OP;
        VN_op(vn) = IR_ARRAY;
        vnexp_map->setv((OBJTY)&vexp, vn);
    }
    setVNOnce(arr, vn);
    change = true;
    return vn;
}


VN const* GVN::computeArray(IR const* exp, bool & change)
{
    ASSERT0(exp->is_array());
    for (IR const* s = ARR_sub_list(exp); s != nullptr; s = s->get_next()) {
        computeVN(s, change);
    }
    VN const* evn = getVN(exp);
    if (evn != nullptr) { return evn; }

    evn = computeExactMemory(exp, change);
    if (evn != nullptr) {
        return evn;
    }

    VN const* abase_vn = computeVN(ARR_base(exp), change);
    VN const* aofst_vn = nullptr;
    if (((CArray*)exp)->getDimNum() == 1) {
        //only handle one dim array.
        if (abase_vn == nullptr) {
            return nullptr;
        }
        aofst_vn = getVN(ARR_sub_list(exp));
        if (aofst_vn == nullptr) {
            return nullptr;
        }
    } else {
        return nullptr;
    }

    DUSet const* du = exp->readDUSet();
    if (du == nullptr || du->get_elem_count() == 0) {
        //Array does not have any DEF.
        VN const* x = registerQuadVN(IR_ARRAY, abase_vn, aofst_vn,
            registerVNviaINT(ARR_ofst(exp)),
            registerVNviaINT(exp->getTypeSize(m_tm)));
        if (getVN(exp) != x) {
            setVN(exp, x);
            change = true;
        }
        return x;
    }

    IR const* exp_stmt = const_cast<IR*>(exp)->getStmt();
    ASSERT0(exp_stmt->is_stmt());
    IR const* domdef = m_du->findNearestDomDef(exp, exp_stmt, du);
    if (domdef == nullptr) {
        return nullptr;
    }
    if (domdef->getMustRef() == nullptr || exp->getMustRef() == nullptr ||
        !domdef->getMustRef()->is_exact() || !exp->getMustRef()->is_exact()) {
        return nullptr;
    }
    if (domdef->is_starray() && ARR_ofst(domdef) != ARR_ofst(exp)) {
        return nullptr;
    }
    if (!domdef->is_starray()) {
        return computeArrayByAnonDomDef(exp, abase_vn, aofst_vn, domdef,
                                        change);
    }

    ASSERT0(domdef->is_starray());
    ASSERTN(((CArray*)domdef)->getDimNum() == 1,
            ("only handle one dim array."));

    //Check if VN expression is match.
    IR const* defbase = ARR_base(domdef);
    VN const* defbase_vn = getVN(defbase);
    if (defbase_vn == nullptr || defbase_vn != abase_vn) {
        return nullptr;
    }
    VN const* o = getVN(ARR_sub_list(domdef));
    if (o == nullptr || o != aofst_vn) {
        return nullptr;
    }

    VN const* def_vn = getVN(domdef);
    if (def_vn == nullptr) {
        def_vn = registerQuadVN(IR_ARRAY, abase_vn, aofst_vn,
                                registerVNviaINT(ARR_ofst(exp)),
                                registerVNviaINT(exp->getTypeSize(m_tm)));
        setVNOnce(domdef, def_vn);
    }
    setVNOnce(exp, def_vn);
    change = true;
    return def_vn;
}


VN const* GVN::computeScalarByAnonDomDef(IR const* exp, IR const* domdef,
                                         bool & change)
{
    ASSERT0((exp->is_ld() || exp->is_pr()) &&
            m_du->isMayDef(domdef, exp, false));
    SCVNE2VN * vnexp_map = m_def2sctab.get(domdef);
    UINT dtsz = exp->getTypeSize(m_tm);
    MD const* md = exp->getExactRef();
    ASSERT0(md);
    VNE_SC vexp(MD_id(md), exp->getOffset(), dtsz);
    //NOTE:
    //    foo();
    //    v1; //s1
    //    goo();
    //    v1; //s2
    //    vn of s1 should not same as s2.
    if (vnexp_map == nullptr) {
        vnexp_map = new SCVNE2VN(m_pool, 16); //bsize to be evaluate.
        m_def2sctab.set(domdef, vnexp_map);
    }
    VN * vn = vnexp_map->get(&vexp);
    if (vn == nullptr) {
        vn = allocVN();
        VN_type(vn) = VN_VAR;
        vnexp_map->setv((OBJTY)&vexp, vn);
    }
    setVNOnce(exp, vn);
    change = true;
    return vn;
}


VN const* GVN::computeScalar(IR const* exp, bool & change)
{
    ASSERT0(exp && exp->is_exp());
    VN const* evn = getVN(exp);
    if (evn != nullptr) { return evn; }

    if (exp->isReadPR() && usePRSSADU()) {
        return computePR(exp, change);
    }

    evn = computeExactMemory(exp, change);
    if (evn != nullptr) { return evn; }

    if (exp->getExactRef() == nullptr) {
        //Can not handle inexact MD.
        return nullptr;
    }

    DUSet const* du = exp->readDUSet();
    if (du == nullptr || du->get_elem_count() == 0) {
        //If exact MD DU is empty, should keep it as unknown status.
        return nullptr;
    }

    IR const* exp_stmt = const_cast<IR*>(exp)->getStmt();
    IR const* domdef = m_du->findNearestDomDef(exp, exp_stmt, du);
    if (domdef == nullptr) {
        return nullptr;
    }
    if (domdef->getMustRef() == nullptr || exp->getMustRef() == nullptr ||
        !domdef->getMustRef()->is_exact() || !exp->getMustRef()->is_exact()) {
        return nullptr;
    }
    if (domdef->is_st() && ST_ofst(domdef) != exp->getOffset()) {
        return nullptr;
    }
    if (!domdef->is_st() && !domdef->is_stpr()) {
        return computeScalarByAnonDomDef(exp, domdef, change);
    }

    switch (exp->getCode()) {
    case IR_LD:
        if (domdef->is_stpr() || (LD_idinfo(exp) != ST_idinfo(domdef))) {
            return nullptr;
        }
        break;
    SWITCH_CASE_READ_PR:
        if (domdef->is_st() || PR_no(exp) != STPR_no(domdef)) {
            return nullptr;
        }
        break;
    default: ASSERTN(0, ("unsupport"));
    }

    VN const* uni_vn = getVN(domdef);
    if (uni_vn == nullptr) {
        VN * t = allocVN();
        VN_type(t) = VN_VAR;
        uni_vn = t;
        setVNOnce(domdef, uni_vn);
    }
    setVNOnce(exp, uni_vn);
    change = true;
    return uni_vn;
}


VN const* GVN::computeSelect(IR const* exp, bool & change)
{
    VN const* vn1 = computeVN(SELECT_det(exp), change);
    VN const* vn2 = computeVN(SELECT_trueexp(exp), change);
    VN const* vn3 = computeVN(SELECT_falseexp(exp), change);
    if (vn1 == nullptr || vn2 == nullptr || vn3 == nullptr) {
        if (getVN(exp) != nullptr) {
            setVN(exp, nullptr);
            change = true;
        }
        return nullptr;
    }
    VN const* x = registerTripleVN(exp->getCode(), vn1, vn2, vn3);
    if (getVN(exp) != x) {
        setVN(exp, x);
        change = true;
    }
    return x;
}


VN const* GVN::computeBin(IR const* exp, bool & change)
{
    VN const* vn1 = computeVN(BIN_opnd0(exp), change);
    VN const* vn2 = computeVN(BIN_opnd1(exp), change);
    if (vn1 == nullptr || vn2 == nullptr) {
        if (getVN(exp) != nullptr) {
            setVN(exp, nullptr);
            change = true;
        }
        return nullptr;
    }
    VN const* x = registerBinVN(exp->getCode(), vn1, vn2);
    if (getVN(exp) != x) {
        setVN(exp, x);
        change = true;
    }
    return x;
}


VN const* GVN::computeConst(IR const* exp, bool & change)
{
    VN const* x = getVN(exp);
    if (x != nullptr) { return x; }

    if (exp->is_int()) {
        x = registerVNviaINT(CONST_int_val(exp));
    } else if (exp->is_ptr()) {
        //Regard PTR as INT because the bit-width of pointer of target machine
        //is always longer than integer.
        x = registerVNviaINT(CONST_int_val(exp));
    } else if (exp->is_mc()) {
        x = registerVNviaMC(CONST_int_val(exp));
    } else if (exp->is_fp()) {
        if (!m_is_vn_fp) {
            return nullptr;
        }
        x = registerVNviaFP(CONST_fp_val(exp));
    } else if (exp->is_str()) {
        x = registerVNviaSTR(CONST_str_val(exp));
    } else if (exp->is_any()) {
        return nullptr;
    } else  {
        ASSERTN(0, ("unsupport const type"));
    }

    ASSERT0(x);
    setVN(exp, x);
    change = true;
    return x;
}


VN const* GVN::computeLda(IR const* exp, bool & change)
{
    Var * v = LDA_idinfo(exp);
    VN const* basevn = nullptr;
    if (v->is_string()) {
        if (m_is_comp_lda_string) {
            MD const* emd = m_rg->getMDMgr()->genMDForVar(v, LDA_ofst(exp));
            ASSERTN(emd && emd->is_effect(),
                    ("string should have effect MD"));
            basevn = registerVNviaMD(emd);
        } else {
            basevn = nullptr;
        }
    } else {
        MD const* emd = m_rg->getMDMgr()->genMDForVar(v, LDA_ofst(exp));
        ASSERTN(emd && emd->is_effect(), ("string should have effect MD"));
        basevn = registerVNviaMD(emd);
    }

    if (basevn == nullptr) {
        if (getVN(exp) != nullptr) {
            setVN(exp, nullptr);
            change = true;
        }
        return nullptr;
    }

    VN const* ofstvn = registerVNviaINT(LDA_ofst(exp));
    VN const* x = registerBinVN(IR_LDA, basevn, ofstvn);
    if (getVN(exp) != x) {
        setVN(exp, x);
        change = true;
    }
    return x;
}


VN const* GVN::computeUna(IR const* exp, bool & change)
{
    VN const* x = computeVN(UNA_opnd(exp), change);
    if (x == nullptr) {
        if (getVN(exp) != nullptr) {
            setVN(exp, nullptr);
            change = true;
        }
        return nullptr;
    }
    x = registerUnaVN(exp->getCode(), x);
    if (getVN(exp) != x) {
        setVN(exp, x);
        change = true;
    }
    return x;
}


VN const* GVN::computeVN(IR const* exp, bool & change)
{
    ASSERT0(exp);
    switch (exp->getCode()) {
    SWITCH_CASE_BIN:
        return computeBin(exp, change);
    SWITCH_CASE_UNA:
        return computeUna(exp, change);
    case IR_LDA:
        return computeLda(exp, change);
    //case IR_ID:
    //Note IR_ID should not participate in GVN analysis because it does not
    //represent a real operation.
    case IR_LD:
    case IR_ID:
    SWITCH_CASE_READ_PR:
        return computeScalar(exp, change);
    case IR_ARRAY:
        return computeArray(exp, change);
    case IR_ILD:
        return computeILoad(exp, change);
    case IR_SELECT:
        return computeSelect(exp, change);
    case IR_CONST:
        return computeConst(exp, change);
    default: break;
    }
    return nullptr;
}


void GVN::processPhi(IR const* ir, bool & change)
{
    VN const* phivn = nullptr;
    IR const* p = PHI_opnd_list(ir);
    if (p != nullptr) {
        phivn = computePhiOpnd(p, change);
        p = p->get_next();
    }
    for (; p != nullptr; p = p->get_next()) {
        VN const* opndvn = computePhiOpnd(p, change);
        if (phivn != nullptr && phivn != opndvn) {
            phivn = nullptr;
        }
    }
    if (getVN(ir) != phivn) {
        setVN(ir, phivn);
        change = true;
    }
}


void GVN::processCall(IR const* ir, bool & change)
{
    for (IR const* p = CALL_param_list(ir); p != nullptr; p = p->get_next()) {
        computeVN(p, change);
    }
    VN const* x = getVN(ir);
    if (x == nullptr) {
        VN * t = allocVN();
        VN_type(t) = VN_VAR;
        change = true;
        x = t;
        setVNOnce(ir, x);
    }
    return;
}


void GVN::processRegion(IR const* ir, bool & change)
{
    //Region effect should be simluated via call-stmt
    //which will be handled.
}


bool GVN::isSameMemLocForIndirectOp(IR const* ir1, IR const* ir2) const
{
    IR const* irbase1 = const_cast<IR*>(ir1)->getBase();
    IR const* irbase2 = const_cast<IR*>(ir2)->getBase();
    ASSERT0(irbase1 && irbase2);
    VN const* base1 = getVN(irbase1);
    VN const* base2 = getVN(irbase2);
    if (base1 == nullptr || base2 == nullptr) {
        return hasSameValueBySSA(irbase1, irbase2);
    }
    return base1 == base2;
}


//Return true if the value of ir1 and ir2 are definitely same, otherwise
//return false to indicate unknown.
bool GVN::hasSameValueByPRSSA(IR const* ir1, IR const* ir2) const
{
    if (!usePRSSADU()) { return false; }
    return PRSSAMgr::hasSameValue(ir1, ir2);
}


bool GVN::hasSameValueByMDSSA(IR const* ir1, IR const* ir2) const
{
    if (!useMDSSADU()) { return false; }
    return MDSSAMgr::hasSameValue(ir1, ir2);
}


bool GVN::hasSameValueBySSA(IR const* ir1, IR const* ir2) const
{
    if (!ir1->isIsomoTo(ir2, true,
                        IsomoFlag(ISOMO_UNDEF|ISOMO_CK_CONST_VAL))) {
        //Only check the IR tree structure and type.
        return false;
    }
    ConstIRIter it1;
    ConstIRIter it2;
    IR const* k1 = iterInitC(ir1, it1, false);
    IR const* k2 = iterInitC(ir2, it2, false);
    for (; k1 != nullptr; k1 = iterNextC(it1, true),
         k2 = iterNextC(it2, true)) {
        ASSERT0(k2);
        if (!k1->isMemRef()) {
            ASSERT0(!k2->isMemRef());
            continue;
        }
        if (k1->isPROp()) {
            ASSERT0(k2->isPROp());
            //Try determining by DU chain.
            if (hasSameValueByPRSSA(k1, k2)) {
                continue;
            }
            //Try determining by VN.
            VN const* k1vn = getVN(k1);
            VN const* k2vn = getVN(k2);
            if (k1vn != nullptr && k2vn != nullptr && k1vn == k2vn) {
                continue;
            }
            //We have no knowledge about k1 and k2.
            return false;
        }
        if (k1->isMemRefNonPR()) {
            ASSERT0(k2->isMemRefNonPR());
            //Try determining by DU chain.
            if (hasSameValueByMDSSA(k1, k2)) {
                continue;
            }
            //Try determining by VN.
            VN const* k1vn = getVN(k1);
            VN const* k2vn = getVN(k2);
            if (k1vn != nullptr && k2vn != nullptr && k1vn == k2vn) {
                continue;
            }
            //We have no knowledge about k1 and k2.
            return false;
        }
        UNREACHABLE();
    }
    ASSERT0(k1 == nullptr && k2 == nullptr);
    return true;
}


static bool isSameMemLocForIRList(IR const* irlst1, IR const* irlst2,
                                  GVN const* gvn)
{
    IR const* s2 = irlst2;
    for (IR const* s1 = irlst1; s1 != nullptr;
         s1 = s1->get_next(), s2 = s2->get_next()) {
        ASSERT0(s2);
        VN const* vs1 = gvn->getVN(s1);
        VN const* vs2 = gvn->getVN(s2);
        if (vs1 == nullptr || vs2 == nullptr) {
            if (!gvn->hasSameValueBySSA(s1, s2)) {
                return false;
            }
        }
        if (vs1 != vs2) {
            return false;
        }
    }
    return true;
}


bool GVN::isSameMemLocForArrayOp(IR const* ir1, IR const* ir2) const
{
    IR const* irbase1 = const_cast<IR*>(ir1)->getBase();
    IR const* irbase2 = const_cast<IR*>(ir2)->getBase();
    ASSERT0(irbase1 && irbase2);
    VN const* base1 = getVN(irbase1);
    VN const* base2 = getVN(irbase2);
    if ((base1 == nullptr || base2 == nullptr) &&
        !hasSameValueBySSA(irbase1, irbase2)) {
        return false;
    }
    if (base1 != base2) {
        return false;
    }
    return isSameMemLocForIRList(ARR_sub_list(ir1), ARR_sub_list(ir2), this);
}


//Return true if ir1 and ir2 represent identical memory location, otherwise
//return false to tell caller we do not know more about these object.
//Note this function does NOT consider data type that ir1 or ir2 referrenced.
bool GVN::isSameMemLoc(IR const* ir1, IR const* ir2) const
{
    ASSERT0(ir1 && ir2);
    if (ir1 == ir2) { return true; }
    if (ir1->getOffset() != ir2->getOffset()) { return false; }
    if ((ir1->is_st() || ir1->is_ld()) && (ir2->is_st() || ir2->is_ld())) {
        return ir1->getIdinfo() == ir2->getIdinfo();
    }
    if (ir1->isIndirectMemOp() && ir2->isIndirectMemOp()) {
        return isSameMemLocForIndirectOp(ir1, ir2);
    }
    if (ir1->isArrayOp() && ir2->isArrayOp() && ir1->isSameArrayStruct(ir2)) {
        return isSameMemLocForArrayOp(ir1, ir2);
    }
    MD const* must1 = ir1->getRefMD();
    MD const* must2 = ir2->getRefMD();
    return must1 == must2 && must1 != nullptr;
}


void GVN::processGETELEM(IR * ir, bool & change)
{
    VN const* v1 = computeVN(GETELEM_base(ir), change);
    VN const* v2 = computeVN(GETELEM_ofst(ir), change);
    if (v1 == nullptr || v2 == nullptr) { return; }
    //Regard getelem as binary operation.
    VN const* x = registerBinVN(IR_GETELEM, v1, v2);
    if (getVN(ir) != x) {
        setVN(ir, x);
        change = true;
    }
}


void GVN::copyVN(IR const* from, IR const* to)
{
    VN const* vn = getVN(from);
    if (vn == nullptr) { return; }
    setVN(to, vn);
}


void GVN::processSETELEM(IR * ir, bool & change)
{
    VN const* v1 = computeVN(SETELEM_base(ir), change);
    VN const* v2 = computeVN(SETELEM_val(ir), change);
    VN const* v3 = computeVN(SETELEM_ofst(ir), change);
    if (v1 == nullptr || v2 == nullptr || v3 == nullptr) { return; }

    //Do NOT clean ir's VN, because its VN may be set by its dominated
    //use-stmt ILD.
    VN const* x = registerTripleVN(ir->getCode(), v1, v2, v3);
    if (getVN(ir) != x) {
        setVN(ir, x);
        change = true;
    }
}


void GVN::processDirectMemOp(IR * ir, bool & change)
{
    ASSERT0(ir->is_stmt() && ir->hasRHS());
    IR * rhs = ir->getRHS();
    if (rhs == nullptr) { return; }
    VN const* x = computeVN(rhs, change);
    if (x == nullptr) {
        //CASE:Any VN indicates the unique value, thus do NOT assign VN to an
        //IR if we can not determine its unique value.
        return;
    }
    //Do NOT clean ir's VN, because its VN may be set by its dominated
    //use-stmt ILD.
    if (getVN(ir) != x) {
        //ir's VN is nullptr only the first iteration computation.
        //ASSERT0(getVN(ir) == nullptr);
        setVN(ir, x);
        change = true;
    }
}


void GVN::processIndirectMemOp(IR * ir, bool & change)
{
    computeVN(ir->getBase(), change);
    processDirectMemOp(ir, change);
}


void GVN::processSTARRAY(IR * ir, bool & change)
{
    computeArrayAddrRef(ir, change);
    processDirectMemOp(ir, change);
}


void GVN::processExtStmt(IR * ir, bool & change)
{
    switch (ir->getCode()) {
    SWITCH_CASE_EXT_STMT:
        if (ir->getCode() == IR_VSTPR) {
            processDirectMemOp(ir, change);
            break;
        }
        UNREACHABLE();
        break;
    default: UNREACHABLE();
    }
}


void GVN::processStmt(IR * ir, bool & change)
{
    ASSERT0(ir);
    switch (ir->getCode()) {
    SWITCH_CASE_DIRECT_MEM_STMT:
    case IR_STPR:
        processDirectMemOp(ir, change);
        return;
    case IR_STARRAY:
        processSTARRAY(ir, change);
        return;
    SWITCH_CASE_INDIRECT_MEM_STMT:
        processIndirectMemOp(ir, change);
        return;
    SWITCH_CASE_CALL:
        processCall(ir, change);
        return;
    SWITCH_CASE_CONDITIONAL_BRANCH_OP:
        computeVN(BR_det(ir), change);
        return;
    SWITCH_CASE_MULTICONDITIONAL_BRANCH_OP:
    case IR_IGOTO:
        computeVN(ir->getValExp(), change);
        return;
    case IR_RETURN:
        if (RET_exp(ir) != nullptr) {
            computeVN(RET_exp(ir), change);
        }
        return;
    case IR_REGION:
        processRegion(ir, change);
        return;
    case IR_PHI:
        processPhi(ir, change);
        return;
    case IR_SETELEM:
        processSETELEM(ir, change);
        return;
    case IR_GETELEM:
        processGETELEM(ir, change);
        return;
    case IR_GOTO:
        return;
    default: processExtStmt(ir, change);
    }
}


VN const* GVN::computePhiPROpnd(IR const* exp, bool & change)
{
    ASSERT0(exp->isReadPR());
    IR const* kd = xoc::findKillingDef(exp, m_rg);
    if (kd != nullptr) {
        return inferVNViaKillingDef(exp, kd, change);
    }
    return inferVNThroughCFG(exp, change);
}


VN const* GVN::computePhiOpnd(IR const* exp, bool & change)
{
    ASSERT0(exp);
    switch (exp->getCode()) {
    SWITCH_CASE_READ_PR:
        return computePhiPROpnd(exp, change);
    case IR_CONST:
        return computeConst(exp, change);
    case IR_LDA:
        return computeLda(exp, change);
    case IR_CVT:
        return computeUna(exp, change);
    default: UNREACHABLE();
    }
    return nullptr;
}


VN const* GVN::computeMDPhiMemOpnd(IR const* exp, bool & change)
{
    ASSERT0(exp->isMemOpnd());
    MD const* emd = exp->getExactRef();
    if (emd == nullptr) { return nullptr; }
    IR const* kd = xoc::findKillingDef(exp, m_rg);
    if (kd != nullptr) {
        return inferVNViaKillingDef(exp, kd, change);
    }
    return inferVNThroughCFG(exp, change);
}


VN const* GVN::computeMDPhiOpnd(IR const* exp, bool & change)
{
    ASSERT0(exp);
    switch (exp->getCode()) {
    case IR_ID:
        return computeMDPhiMemOpnd(exp, change);
    case IR_CONST:
        return computeConst(exp, change);
    case IR_CVT:
        return computeUna(exp, change);
    default: UNREACHABLE();
    }
    return nullptr;
}


void GVN::processMDPhi(MDPhi const* phi, bool & change)
{
    VN const* phivn = nullptr;
    IR const* p = MDPHI_opnd_list(phi);
    if (p != nullptr) {
        phivn = computeMDPhiOpnd(p, change);
        p = p->get_next();
    }
    for (; p != nullptr; p = p->get_next()) {
        VN const* opndvn = computeMDPhiOpnd(p, change);
        if (phivn != nullptr && phivn != opndvn) {
            phivn = nullptr;
        }
    }
    if (getVN(phi) != phivn) {
        ASSERT0(getVN(phi) == nullptr);
        setVN(phi, phivn);
        change = true;
    }
}


void GVN::processMDPhi(IRBB * bb, bool & change)
{
    if (!useMDSSADU()) { return; }
    MDPhiList * philist = m_mdssamgr->getPhiList(bb->id());
    if (philist == nullptr) { return; }
    for (MDPhiListIter it = philist->get_head();
         it != philist->end(); it = philist->get_next(it)) {
        MDPhi * phi = it->val();
        ASSERT0(phi && phi->is_phi());
        processMDPhi(phi, change);
    }
}


void GVN::processBB(IRBB * bb, bool & change)
{
    processMDPhi(bb, change);
    IRListIter ct;
    for (BB_irlist(bb).get_head(&ct);
         ct != BB_irlist(bb).end(); ct = BB_irlist(bb).get_next(ct)) {
        processStmt(ct->val(), change);
    }
}


void GVN::assignRHSVN()
{
    BBList const* bbl = m_rg->getBBList();
    BBList::Iter it;
    for (IRBB * bb = bbl->get_head(&it);
         bb != nullptr; bb = bbl->get_next(&it)) {
        for (IR const* ir = BB_first_ir(bb);
             ir != nullptr; ir = BB_next_ir(bb)) {
            if (!ir->hasRHS()) { continue; }
            VN const* resvn = getVN(ir);
            if (resvn == nullptr) {
                ASSERT0(getVN(ir->getRHS()) == nullptr);
                continue;
            }
            IR const* rhs = ir->getRHS();
            ASSERT0(getVN(rhs) == nullptr || getVN(rhs) == resvn);
            setVN(rhs, resvn);
        }
    }
}


void GVN::processBBListInRPO()
{
    RPOVexList * vlst = m_cfg->getRPOVexList();
    ASSERT0(vlst);
    ASSERT0(vlst->get_elem_count() == m_rg->getBBList()->get_elem_count());
    UINT count = 0;
    bool change = true;
    while (change && count < 100) {
        change = false;
        for (Vertex const* v = vlst->get_head();
             v != nullptr; v = vlst->get_next()) {
            processBB(m_cfg->getBB(v->id()), change);
        }
        count++;
    }
    ASSERT0(!change);
}


void GVN::dumpIR2VN() const
{
    if (!m_rg->isLogMgrInit()) { return; }
    IR2VNIter it;
    VN const* x;
    for (UINT i = m_ir2vn.get_first(it, &x); x != nullptr;
         i = m_ir2vn.get_next(it, &x)) {
        note(getRegion(), "\nIR%d:vn%d, %s", i, x->id(),
             VNTypeDesc::getVTName(x->getType()));
    }
}


void GVN::dump_h1(IR const* k, VN const* x) const
{
    ASSERT0(k);
    if (k->is_pr()) {
        note(getRegion(), "\n\t$%d", PR_no(k));
    } else {
        note(getRegion(), "\n\t%s", IRCNAME(k->getCode()));
    }
    prt(getRegion(), " id:%d ", k->id());
    if (x != nullptr) {
        prt(getRegion(), "vn%d", x->id());
    } else {
        prt(getRegion(), "--");
    }
}


void GVN::dumpBB(UINT bbid) const
{
    if (!m_rg->isLogMgrInit()) { return; }
    IRBB * bb = m_cfg->getBB(bbid);
    ASSERT0(bb);

    ConstIRIter ii;
    note(getRegion(), "\n-- BB%d ", bb->id());
    dumpBBLabel(bb->getLabelList(), getRegion());
    note(getRegion(), "\n");
    for (IR * ir = BB_first_ir(bb); ir != nullptr; ir = BB_next_ir(bb)) {
        dumpIR(ir, m_rg);
        note(getRegion(), "\n");
        VN const* x = getVN(ir);
        if (x != nullptr) {
            prt(getRegion(), "vn%u", x->id());
        }

        prt(getRegion(), " <- {");
        ii.clean();
        bool dumped = false;
        for (IR const* k = iterExpInitC(ir, ii);
             k != nullptr; k = iterNextC(ii)) {
            dumped = true;
            VN const* vn = getVN(k);
            dump_h1(k, vn);
        }
        if (dumped) {
            note(getRegion(), "\n");
        }
        prt(getRegion(), " }");
    }
}


bool GVN::dump() const
{
    if (!m_rg->isLogMgrInit()) { return false; }
    START_TIMER_FMT(t, ("DUMP %s", getPassName()));
    note(getRegion(), "\n==---- DUMP %s '%s' ----==",
         getPassName(), m_rg->getRegionName());
    getRegion()->getLogMgr()->incIndent(2);
    BBList * bbl = m_rg->getBBList();
    for (IRBB * bb = bbl->get_head(); bb != nullptr; bb = bbl->get_next()) {
        dumpBB(bb->id());
    }
    Pass::dump();
    getRegion()->getLogMgr()->decIndent(2);
    END_TIMER_FMT(t, ("DUMP %s", getPassName()));
    return true;
}


//Return true if GVN is able to determine the result of 'ir', otherwise
//return false that GVN know nothing about ir.
bool GVN::calcCondMustVal(IR const* ir, bool & must_true,
                          bool & must_false) const
{
    must_true = false;
    must_false = false;
    ASSERT0(ir->is_judge());
    if (ir->is_lnot()) {
        VN const* v = getVN(UNA_opnd(ir));
        if (v == nullptr) { return false; }

        if (VN_type(v) == VN_INT) {
            if (!VN_int_val(v)) {
                must_true = true;
                must_false = false;
            } else {
                must_true = false;
                must_false = true;
            }
            return true;
        }

        if (VN_type(v) == VN_FP) {
            if (VN_fp_val(v) == 0.0) {
                must_true = true;
                must_false = false;
                return true;
            }

            must_true = false;
            must_false = true;
            return true;
        }

        if (VN_type(v) == VN_STR) {
            must_true = false;
            must_false = true;
            return true;
        }

        return false;
    }

    VN const* v1 = getVN(BIN_opnd0(ir));
    VN const* v2 = getVN(BIN_opnd1(ir));
    if (v1 == nullptr || v2 == nullptr) { return false; }

    switch (ir->getCode()) {
    case IR_LAND:
    case IR_LOR:
        if (VN_type(v1) == VN_INT && VN_type(v2) == VN_INT) {
            if (VN_int_val(v1) && VN_int_val(v2)) {
                must_true = true;
                must_false = false;
            } else {
                must_true = false;
                must_false = true;
            }
            return true;
        }
        break;
    case IR_LT:
    case IR_GT:
        if (v1 == v2) {
            must_true = false;
            must_false = true;
            return true;
        }

        if (VN_type(v1) == VN_INT && VN_type(v2) == VN_INT) {
            if (ir->is_lt()) {
                if (VN_int_val(v1) < VN_int_val(v2)) {
                    must_true = true;
                    must_false = false;
                } else {
                    must_true = false;
                    must_false = true;
                }
                return true;
            } else if (ir->is_gt()) {
                if (VN_int_val(v1) > VN_int_val(v2)) {
                    must_true = true;
                    must_false = false;
                } else {
                    must_true = false;
                    must_false = true;
                }
                return true;
            }
        }
        break;
    case IR_LE:
    case IR_GE:
        if (v1 == v2) {
            must_true = true;
            must_false = false;
            return true;
        }

        if (VN_type(v1) == VN_INT && VN_type(v2) == VN_INT) {
            if (ir->is_le()) {
                if (VN_int_val(v1) <= VN_int_val(v2)) {
                    must_true = true;
                    must_false = false;
                } else {
                    must_true = false;
                    must_false = true;
                }
                return true;
            } else if (ir->is_ge()) {
                if (VN_int_val(v1) >= VN_int_val(v2)) {
                    must_true = true;
                    must_false = false;
                } else {
                    must_true = false;
                    must_false = true;
                }
                return true;
            }
        }
        break;
    case IR_NE:
        if (v1 != v2) {
            must_true = true;
            must_false = false;
            return true;
        }

        must_true = false;
        must_false = true;
        return true;
    case IR_EQ:
        if (v1 == v2) {
            must_true = true;
            must_false = false;
            return true;
        }

        must_true = false;
        must_false = true;
        return true;
    default: UNREACHABLE();
    }
    return false;
}


//GVN try to assign a value numbers to expressions.
bool GVN::perform(OptCtx & oc)
{
    if (m_rg->getBBList()->get_elem_count() == 0) { return false; }
    if (!oc.is_ref_valid()) { return false; }
    m_mdssamgr = m_rg->getMDSSAMgr();
    m_prssamgr = m_rg->getPRSSAMgr();
    m_oc = &oc;
    if (!oc.is_pr_du_chain_valid() && !usePRSSADU()) {
        //GVN use either classic PR DU chain or PRSSA.
        //At least one kind of DU chain should be avaiable.
        return false;
    }
    if (!oc.is_nonpr_du_chain_valid() && !useMDSSADU()) {
        //GVN use either classic MD DU chain or MDSSA.
        //At least one kind of DU chain should be avaiable.
        return false;
    }

    START_TIMER(t, getPassName());
    clean();
    m_rg->getPassMgr()->checkValidAndRecompute(&oc, PASS_RPO, PASS_DOM,
                                               PASS_UNDEF);
    processBBListInRPO();
    assignRHSVN();
    destroyLocalUsed();
    END_TIMER(t, getPassName());

    if (g_dump_opt.isDumpAfterPass() && g_dump_opt.isDumpGVN()) {
       dump();
    }
    set_valid(true);
    return true;
}
//END GVN

} //namespace xoc
