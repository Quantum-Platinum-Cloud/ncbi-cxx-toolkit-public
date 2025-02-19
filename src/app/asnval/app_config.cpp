/* $Id$
* ========================================================================== =
*
*PUBLIC DOMAIN NOTICE
* National Center for Biotechnology Information
*
* This software / database is a "United States Government Work" under the
* terms of the United States Copyright Act.It was written as part of
* the author's official duties as a United States Government employee and
* thus cannot be copyrighted.This software / database is freely available
* to the public for use.The National Library of Medicineand the U.S.
* Government have not placed any restriction on its use or reproduction.
*
*Although all reasonable efforts have been taken to ensure the accuracy
* andreliability of the software and data, the NLMand the U.S.
* Government do not and cannot warrant the performance or results that
* may be obtained by using this software or data.The NLM and the U.S.
* Government disclaim all warranties, express or implied, including
* warranties of performance, merchantability or fitness for any particular
* purpose.
*
* Please cite the author in any work or product based on this material.
*
*========================================================================== =
*
*Author:  Frank Ludwig
*
* File Description :
*validator
*
*/

#include <ncbi_pch.hpp>
#include "app_config.hpp"
#include <objtools/validator/valid_cmdargs.hpp>

using namespace ncbi;

CAppConfig::CAppConfig(const CArgs& args)
{
    mQuiet = args["quiet"] && args["quiet"].AsBoolean();
    mDoCleanup = args["cleanup"] && args["cleanup"].AsBoolean();
    mVerbosity = static_cast<CAppConfig::EVerbosity>(args["v"].AsInteger());
    mLowCutoff = static_cast<EDiagSev>(args["Q"].AsInteger() - 1);
    mHighCutoff = static_cast<EDiagSev>(args["P"].AsInteger() - 1);
    mReportLevel = static_cast<EDiagSev>(args["R"].AsInteger() - 1);

    mBatch = args["batch"];
    string objectType = args["a"].AsString();
    if (!objectType.empty()) {
        if (objectType == "t" || objectType == "u") {
            mBatch = true;
            cerr << "Warning: -a t and -a u are deprecated; use -batch instead." << endl;
        }
        else {
            cerr << "Warning: -a is deprecated; ASN.1 type is now autodetected." << endl;
        }
    }

    if (args["E"]) {
        mOnlyError = args["E"].AsString();
    }
    mOnlyAnnots = args["annot"];
    mHugeFile = args["huge"];
    mContinue = false;
    // Set validator options
    m_Options = objects::validator::CValidatorArgUtil::ArgsToValidatorOptions(args);
}
