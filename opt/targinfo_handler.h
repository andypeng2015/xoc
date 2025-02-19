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
#ifndef _TARGINFO_HANDLER_H_
#define _TARGINFO_HANDLER_H_

namespace xoc {

class LinearScanRA;

//The class processes target machine dependent if REF_TARGMACH_INFO enabled.
class TargInfoHandler : public Pass {
    COPY_CONSTRUCTOR(TargInfoHandler);
protected:
    LinearScanRA * m_lsra;
    PassMgr * m_passmgr;
protected:
    LinearScanRA * queryLSRA();
public:
    explicit TargInfoHandler(Region * rg);
    virtual ~TargInfoHandler();

    virtual CHAR const* getPassName() const { return "TargInfo Handler"; }
    PASS_TYPE getPassType() const { return PASS_TARGINFO_HANDLER; }

    virtual bool perform(OptCtx & oc) { return false; }

    //The function try to copy physical register from 'src' to 'tgt' if 'src'
    //has been assigned a physical register.
    //e.g: src's PRNO is $1(with r1), the 'tgt' PRNO should be $2(with r1 too).
    virtual void tryCopyPhyRegIfAny(PRNO src, PRNO tgt);
};

} //namespace xoc
#endif
