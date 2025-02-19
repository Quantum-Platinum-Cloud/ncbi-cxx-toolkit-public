/*  $Id$
 * ===========================================================================
 *
 *                            PUBLIC DOMAIN NOTICE
 *               National Center for Biotechnology Information
 *
 *  This software/database is a "United States Government Work" under the
 *  terms of the United States Copyright Act.  It was written as part of
 *  the author's official duties as a United States Government employee and
 *  thus cannot be copyrighted.  This software/database is freely available
 *  to the public for use. The National Library of Medicine and the U.S.
 *  Government have not placed any restriction on its use or reproduction.
 *
 *  Although all reasonable efforts have been taken to ensure the accuracy
 *  and reliability of the software and data, the NLM and the U.S.
 *  Government do not and cannot warrant the performance or results that
 *  may be obtained by using this software or data. The NLM and the U.S.
 *  Government disclaim all warranties, express or implied, including
 *  warranties of performance, merchantability or fitness for any particular
 *  purpose.
 *
 *  Please cite the author in any work or product based on this material.
 *
 * ===========================================================================
 *
 * Authors: Dmitri Dmitrienko
 *
 * File Description:
 *
 */
#include <ncbi_pch.hpp>

#include "pubseq_gateway.hpp"
#include "http_request.hpp"


CPubseqGatewayApp::SRequestParameter
CPubseqGatewayApp::x_GetParam(CHttpRequest &  req, const string &  name) const
{
    SRequestParameter       param;
    const char *            value;
    size_t                  value_size;

    param.m_Found = req.GetParam(name.data(), name.size(),
                                 true, &value, &value_size);
    if (param.m_Found)
        param.m_Value.assign(value, value_size);
    return param;
}


bool CPubseqGatewayApp::x_IsBoolParamValid(const string &  param_name,
                                           const CTempString &  param_value,
                                           string &  err_msg) const
{
    static string   yes = "yes";
    static string   no = "no";

    if (param_value != yes && param_value != no) {
        err_msg = "Malformed '" + param_name + "' parameter. "
                  "Acceptable values are '" + yes + "' and '" + no + "'.";
        return false;
    }
    return true;
}


bool CPubseqGatewayApp::x_ConvertIntParameter(const string &  param_name,
                                              const CTempString &  param_value,
                                              int &  converted,
                                              string &  err_msg) const
{
    try {
        converted = NStr::StringToInt(param_value);
    } catch (...) {
        err_msg = "Error converting '" + param_name + "' parameter "
                  "to integer (received value: '" + string(param_value) + "')";
        return false;
    }
    return true;
}


bool CPubseqGatewayApp::x_ConvertIntParameter(const string &  param_name,
                                              const CTempString &  param_value,
                                              int64_t &  converted,
                                              string &  err_msg) const
{
    try {
        converted = NStr::StringToLong(param_value);
    } catch (...) {
        err_msg = "Error converting '" + param_name + "' parameter "
                  "to integer (received value: '" + string(param_value) + "')";
        return false;
    }
    return true;
}


bool CPubseqGatewayApp::x_ConvertDoubleParameter(const string &  param_name,
                                                 const CTempString &  param_value,
                                                 double &  converted,
                                                 string &  err_msg) const
{
    try {
        converted = NStr::StringToDouble(param_value);
    } catch (...) {
        err_msg = "Error converting '" + param_name + "' parameter "
                  "to double (received value: '" + string(param_value) + "')";
        return false;
    }
    return true;
}


void CPubseqGatewayApp::x_MalformedArguments(
                                shared_ptr<CPSGS_Reply>  reply,
                                const psg_time_point_t &  now,
                                const string &  err_msg)
{
    m_Counters.Increment(CPSGSCounters::ePSGS_MalformedArgs);
    x_SendMessageAndCompletionChunks(reply, now, err_msg,
                                     CRequestStatus::e400_BadRequest,
                                     ePSGS_MalformedParameter, eDiag_Error);
    PSG_WARNING(err_msg);
}


void CPubseqGatewayApp::x_InsufficientArguments(
                                shared_ptr<CPSGS_Reply>  reply,
                                const psg_time_point_t &  now,
                                const string &  err_msg)
{
    m_Counters.Increment(CPSGSCounters::ePSGS_InsufficientArgs);
    x_SendMessageAndCompletionChunks(reply, now, err_msg,
                                     CRequestStatus::e400_BadRequest,
                                     ePSGS_InsufficientArguments, eDiag_Error);
    PSG_WARNING(err_msg);
}


// Used in case of exceptions while handling requests
void CPubseqGatewayApp::x_Finish500(shared_ptr<CPSGS_Reply>  reply,
                                    const psg_time_point_t &  now,
                                    EPSGS_PubseqGatewayErrorCode  code,
                                    const string &  err_msg)
{
   x_SendMessageAndCompletionChunks(reply, now, err_msg,
                                    CRequestStatus::e500_InternalServerError,
                                    code, eDiag_Error);
    PSG_ERROR(err_msg);
}


bool
CPubseqGatewayApp::x_GetTraceParameter(CHttpRequest &  req,
                                       shared_ptr<CPSGS_Reply>  reply,
                                       const psg_time_point_t &  now,
                                       SPSGS_RequestBase::EPSGS_Trace &  trace)
{
    static string  kTraceParam = "trace";

    trace = SPSGS_RequestBase::ePSGS_NoTracing;     // default
    SRequestParameter   trace_protocol_param = x_GetParam(req, kTraceParam);

    if (trace_protocol_param.m_Found) {
        string      err_msg;

        if (!x_IsBoolParamValid(kTraceParam,
                                trace_protocol_param.m_Value, err_msg)) {
            x_MalformedArguments(reply, now, err_msg);
            return false;
        }
        if (trace_protocol_param.m_Value == "yes")
            trace = SPSGS_RequestBase::ePSGS_WithTracing;
        else
            trace = SPSGS_RequestBase::ePSGS_NoTracing;
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetProcessorEventsParameter(CHttpRequest &  req,
                                                 shared_ptr<CPSGS_Reply>  reply,
                                                 const psg_time_point_t &  now,
                                                 bool &  processor_events)
{
    static string  kProcessorEventsParam = "processor_events";

    processor_events = false;   // default
    SRequestParameter   processor_events_param = x_GetParam(req, kProcessorEventsParam);

    if (processor_events_param.m_Found) {
        string      err_msg;

        if (!x_IsBoolParamValid(kProcessorEventsParam,
                                processor_events_param.m_Value, err_msg)) {
            x_MalformedArguments(reply, now, err_msg);
            return false;
        }
        processor_events = processor_events_param.m_Value == "yes";
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetResendTimeout(CHttpRequest &  req,
                                      shared_ptr<CPSGS_Reply>  reply,
                                      const psg_time_point_t &  now,
                                      double &  resend_timeout)
{
    static string  kResendTimeoutParam = "resend_timeout";

    resend_timeout = m_Settings.m_ResendTimeoutSec;
    SRequestParameter   resend_timeout_param = x_GetParam(req, kResendTimeoutParam);

    if (resend_timeout_param.m_Found) {
        string      err_msg;
        if (!x_ConvertDoubleParameter(kResendTimeoutParam,
                                      resend_timeout_param.m_Value,
                                      resend_timeout, err_msg)) {
            x_MalformedArguments(reply, now, err_msg);
            return false;
        }

        if (resend_timeout < 0.0) {
            err_msg = "Invalid '" + kResendTimeoutParam + "' value " +
                      to_string(resend_timeout) + ". It must be >= 0.0";
            x_MalformedArguments(reply, now, err_msg);
            return false;
        }
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetSeqIdResolveParameter(CHttpRequest &  req,
                                              shared_ptr<CPSGS_Reply>  reply,
                                              const psg_time_point_t &  now,
                                              bool &  seq_id_resolve)
{
    static string  kSeqIdResolveParam = "seq_id_resolve";

    seq_id_resolve = true;      // default
    SRequestParameter   seq_id_resolve_param = x_GetParam(req, kSeqIdResolveParam);

    if (seq_id_resolve_param.m_Found) {
        string      err_msg;
        if (!x_IsBoolParamValid(kSeqIdResolveParam,
                                seq_id_resolve_param.m_Value, err_msg)) {
            x_MalformedArguments(reply, now, err_msg);
            return false;
        }
        seq_id_resolve = seq_id_resolve_param.m_Value == "yes";
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetHops(CHttpRequest &  req,
                             shared_ptr<CPSGS_Reply>  reply,
                             const psg_time_point_t &  now,
                             int &  hops)
{
    static string   kHopsParam = "hops";

    hops = 0;   // Default value
    SRequestParameter   hops_param = x_GetParam(req, kHopsParam);

    if (hops_param.m_Found) {
        string      err_msg;
        if (!x_ConvertIntParameter(kHopsParam, hops_param.m_Value,
                                   hops, err_msg)) {
            x_MalformedArguments(reply, now, err_msg);
            return false;
        }

        if (hops < 0) {
            err_msg = "Invalid '" + kHopsParam + "' value " + to_string(hops) +
                      ". It must be > 0.";
            x_MalformedArguments(reply, now, err_msg);
            return false;
        }

        if (hops > m_Settings.m_MaxHops) {
            err_msg = "The '" + kHopsParam + "' value " + to_string(hops) +
                      " exceeds the server configured value " +
                      to_string(m_Settings.m_MaxHops) + ".";
            m_Counters.Increment(CPSGSCounters::ePSGS_MaxHopsExceededError);
            x_SendMessageAndCompletionChunks(reply, now, err_msg,
                                             CRequestStatus::e400_BadRequest,
                                             ePSGS_MalformedParameter,
                                             eDiag_Error);
            PSG_WARNING(err_msg);
            return false;
        }
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetLastModified(CHttpRequest &  req,
                                     shared_ptr<CPSGS_Reply>  reply,
                                     const psg_time_point_t &  now,
                                     int64_t &  last_modified)
{
    static string   kLastModifiedParam = "last_modified";

    last_modified = INT64_MIN;

    SRequestParameter   last_modified_param = x_GetParam(req, kLastModifiedParam);
    if (last_modified_param.m_Found) {
        try {
            last_modified = NStr::StringToLong(last_modified_param.m_Value);
        } catch (...) {
            x_MalformedArguments(reply, now,
                                 "Malformed '" + kLastModifiedParam +
                                 "' parameter. Expected an integer");
            return false;
        }
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetBlobId(CHttpRequest &  req,
                               shared_ptr<CPSGS_Reply>  reply,
                               const psg_time_point_t &  now,
                               SPSGS_BlobId &  blob_id)
{
    static string   kBlobIdParam = "blob_id";

    SRequestParameter   blob_id_param = x_GetParam(req, kBlobIdParam);
    if (!blob_id_param.m_Found) {
        x_InsufficientArguments(reply, now,
                                "Mandatory parameter "
                                "'" + kBlobIdParam + "' is not found.");
        return false;
    }

    blob_id.SetId(blob_id_param.m_Value);
    if (blob_id.GetId().empty()) {
        x_MalformedArguments(reply, now,
                             "The '" + kBlobIdParam +
                             "' parameter value has not been supplied");
        return false;
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetResolveFlags(CHttpRequest &  req,
                                     shared_ptr<CPSGS_Reply>  reply,
                                     const psg_time_point_t &  now,
                                     SPSGS_ResolveRequest::TPSGS_BioseqIncludeData &  include_data_flags)
{
    static vector<pair<string, SPSGS_ResolveRequest::EPSGS_BioseqIncludeData>>
        kResolveFlagParams =
    {
        make_pair("all_info", SPSGS_ResolveRequest::fPSGS_AllBioseqFields),   // must be first

        make_pair("canon_id", SPSGS_ResolveRequest::fPSGS_CanonicalId),
        make_pair("seq_ids", SPSGS_ResolveRequest::fPSGS_SeqIds),
        make_pair("mol_type", SPSGS_ResolveRequest::fPSGS_MoleculeType),
        make_pair("length", SPSGS_ResolveRequest::fPSGS_Length),
        make_pair("state", SPSGS_ResolveRequest::fPSGS_State),
        make_pair("blob_id", SPSGS_ResolveRequest::fPSGS_BlobId),
        make_pair("tax_id", SPSGS_ResolveRequest::fPSGS_TaxId),
        make_pair("hash", SPSGS_ResolveRequest::fPSGS_Hash),
        make_pair("date_changed", SPSGS_ResolveRequest::fPSGS_DateChanged),
        make_pair("gi", SPSGS_ResolveRequest::fPSGS_Gi),
        make_pair("name", SPSGS_ResolveRequest::fPSGS_Name),
        make_pair("seq_state", SPSGS_ResolveRequest::fPSGS_SeqState)
    };

    include_data_flags = 0;     // default

    SRequestParameter       request_param;
    for (const auto &  flag_param: kResolveFlagParams) {
        request_param = x_GetParam(req, flag_param.first);
        if (request_param.m_Found) {
            string  err_msg;
            if (!x_IsBoolParamValid(flag_param.first,
                                    request_param.m_Value, err_msg)) {
                x_MalformedArguments(reply, now, err_msg);
                return false;
            }
            if (request_param.m_Value == "yes") {
                include_data_flags |= flag_param.second;
            } else {
                include_data_flags &= ~flag_param.second;
            }
        }
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetId2Chunk(CHttpRequest &  req,
                                 shared_ptr<CPSGS_Reply>  reply,
                                 const psg_time_point_t &  now,
                                 int64_t &  id2_chunk)
{
    static string   kId2ChunkParam = "id2_chunk";

    id2_chunk = INT64_MIN;
    SRequestParameter   id2_chunk_param = x_GetParam(req, kId2ChunkParam);
    if (!id2_chunk_param.m_Found) {
        x_InsufficientArguments(reply, now, "Mandatory parameter "
                                "'" + kId2ChunkParam + "' is not found.");
        return false;
    }

    try {
        id2_chunk = NStr::StringToLong(id2_chunk_param.m_Value);
        if (id2_chunk < 0) {
            x_MalformedArguments(reply, now,
                                 "Invalid '" + kId2ChunkParam +
                                 "' parameter. Expected >= 0");
            return false;
        }
    } catch (...) {
        x_MalformedArguments(reply, now,
                             "Malformed '" + kId2ChunkParam + "' parameter. "
                             "Expected an integer");
        return false;
    }
    return true;
}


vector<string>
CPubseqGatewayApp::x_GetExcludeBlobs(CHttpRequest &  req) const
{
    static string   kExcludeBlobsParam = "exclude_blobs";

    vector<string>      result;
    SRequestParameter   exclude_blobs_param = x_GetParam(req, kExcludeBlobsParam);
    if (exclude_blobs_param.m_Found) {
        vector<string>      blob_ids;
        NStr::Split(exclude_blobs_param.m_Value, ",", blob_ids);

        size_t      empty_count = 0;
        for (const auto &  item : blob_ids) {
            if (item.empty())
                ++empty_count;
            else
                result.push_back(item);
        }

        if (empty_count > 0)
            PSG_WARNING("Found " << empty_count << " empty blob id(s) in the '" <<
                        kExcludeBlobsParam << "' list (empty blob ids are ignored)");
    }

    return result;
}


bool
CPubseqGatewayApp::x_GetEnabledAndDisabledProcessors(
                                        CHttpRequest &  req,
                                        shared_ptr<CPSGS_Reply>  reply,
                                        const psg_time_point_t &  now,
                                        vector<string> &  enabled_processors,
                                        vector<string> &  disabled_processors)
{
    static string   kEnableProcessor = "enable_processor";
    static string   kDisableProcessor = "disable_processor";

    req.GetMultipleValuesParam(kEnableProcessor.data(),
                               kEnableProcessor.size(),
                               enabled_processors);
    req.GetMultipleValuesParam(kDisableProcessor.data(),
                               kDisableProcessor.size(),
                               disabled_processors);

    enabled_processors.erase(
        remove_if(enabled_processors.begin(), enabled_processors.end(),
                  [](string const & s) { return s.empty(); }),
        enabled_processors.end());
    disabled_processors.erase(
        remove_if(disabled_processors.begin(), disabled_processors.end(),
                  [](string const & s) { return s.empty(); }),
        disabled_processors.end());

    for (const auto & en_processor : enabled_processors) {
        for (const auto &  dis_processor : disabled_processors) {
            if (NStr::CompareNocase(en_processor, dis_processor) == 0) {
                string      err_msg = "The same processor name is found "
                    "in both '" + kEnableProcessor + "' (has it as " + en_processor + ") and '" +
                    kDisableProcessor + "' (has it as " + dis_processor + ") lists";
                m_Counters.Increment(CPSGSCounters::ePSGS_MalformedArgs);
                x_SendMessageAndCompletionChunks(reply, now, err_msg,
                                                 CRequestStatus::e400_BadRequest,
                                                 ePSGS_MalformedParameter,
                                                 eDiag_Error);
                PSG_WARNING(err_msg);
                return false;
            }
        }
    }

    return true;
}


bool CPubseqGatewayApp::x_GetTSEOption(CHttpRequest &  req,
                                       shared_ptr<CPSGS_Reply>  reply,
                                       const psg_time_point_t &  now,
                                       SPSGS_BlobRequestBase::EPSGS_TSEOption &  tse_option)
{
    static string   kTSEParam = "tse";
    static string   none = "none";
    static string   whole = "whole";
    static string   orig = "orig";
    static string   smart = "smart";
    static string   slim = "slim";

    SRequestParameter       tse_param = x_GetParam(req, kTSEParam);
    if (tse_param.m_Found) {
        if (tse_param.m_Value == none) {
            tse_option = SPSGS_BlobRequestBase::ePSGS_NoneTSE;
            return true;
        }
        if (tse_param.m_Value == whole) {
            tse_option = SPSGS_BlobRequestBase::ePSGS_WholeTSE;
            return true;
        }
        if (tse_param.m_Value == orig) {
            tse_option = SPSGS_BlobRequestBase::ePSGS_OrigTSE;
            return true;
        }
        if (tse_param.m_Value == smart) {
            tse_option = SPSGS_BlobRequestBase::ePSGS_SmartTSE;
            return true;
        }
        if (tse_param.m_Value == slim) {
            tse_option = SPSGS_BlobRequestBase::ePSGS_SlimTSE;
            return true;
        }

        x_MalformedArguments(reply, now,
                             "Malformed '" + kTSEParam + "' parameter. "
                             "Acceptable values are '" +
                             none + "', '" +
                             whole + "', '" +
                             orig + "', '" +
                             smart + "' and '" +
                             slim + "'.");
        return false;
    }

    return true;
}


bool
CPubseqGatewayApp::x_GetAccessionSubstitutionOption(
                                        CHttpRequest &  req,
                                        shared_ptr<CPSGS_Reply>  reply,
                                        const psg_time_point_t &  now,
                                        SPSGS_RequestBase::EPSGS_AccSubstitutioOption & acc_subst_option)
{
    static string       kAccSubstitutionParam = "acc_substitution";
    static string       default_option = "default";
    static string       limited_option = "limited";
    static string       never_option = "never";

    SRequestParameter   subst_param = x_GetParam(req, kAccSubstitutionParam);
    if (subst_param.m_Found) {
        if (subst_param.m_Value == default_option) {
            acc_subst_option = SPSGS_RequestBase::ePSGS_DefaultAccSubstitution;
            return true;
        }
        if (subst_param.m_Value == limited_option) {
            acc_subst_option = SPSGS_RequestBase::ePSGS_LimitedAccSubstitution;
            return true;
        }
        if (subst_param.m_Value == never_option) {
            acc_subst_option = SPSGS_RequestBase::ePSGS_NeverAccSubstitute;
            return true;
        }

        x_MalformedArguments(reply, now,
                             "Malformed '" + kAccSubstitutionParam + "' parameter. "
                             "Acceptable values are '" +
                             default_option + "', '" +
                             limited_option + "', '" +
                             never_option + "'.");
        return false;
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetIntrospectionFormat(CHttpRequest &  req,
                                            string &  fmt,
                                            string &  err_msg)
{
    static string   kFmtParam = "fmt";
    static string   html = "html";
    static string   json = "json";


    SRequestParameter   fmt_param = x_GetParam(req, kFmtParam);
    if (fmt_param.m_Found) {
        if (fmt_param.m_Value == html) {
            fmt = html;
            return true;
        }
        if (fmt_param.m_Value == json) {
            fmt = json;
            return true;
        }

        err_msg = "Malformed '" + kFmtParam + "' parameter. "
                  "Acceptable values are '" + html + "' and '" + json + "'";
        return false;
    }

    fmt = html;     // default
    return true;
}



bool
CPubseqGatewayApp::x_GetOutputFormat(CHttpRequest &  req,
                                     shared_ptr<CPSGS_Reply>  reply,
                                     const psg_time_point_t &  now,
                                     SPSGS_ResolveRequest::EPSGS_OutputFormat &  output_format)
{
    static string   kFmtParam = "fmt";
    static string   protobuf = "protobuf";
    static string   json = "json";
    static string   native = "native";

    SRequestParameter   fmt_param = x_GetParam(req, kFmtParam);
    if (fmt_param.m_Found) {
        if (fmt_param.m_Value == protobuf) {
            output_format = SPSGS_ResolveRequest::ePSGS_ProtobufFormat;
            return true;
        }
        if (fmt_param.m_Value == json) {
            output_format = SPSGS_ResolveRequest::ePSGS_JsonFormat;
            return true;
        }
        if (fmt_param.m_Value == native) {
            output_format = SPSGS_ResolveRequest::ePSGS_NativeFormat;
            return true;
        }

        x_MalformedArguments(reply, now,
                             "Malformed '" + kFmtParam + "' parameter. "
                             "Acceptable values are '" +
                             protobuf + "' and '" +
                             json + "' and '" +
                             native + "'.");
        return false;
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetUseCacheParameter(CHttpRequest &  req,
                                          shared_ptr<CPSGS_Reply>  reply,
                                          const psg_time_point_t &  now,
                                          SPSGS_RequestBase::EPSGS_CacheAndDbUse &  use_cache)
{
    static string   kUseCacheParam = "use_cache";

    use_cache = SPSGS_RequestBase::ePSGS_CacheAndDb;    // default

    SRequestParameter   use_cache_param = x_GetParam(req, kUseCacheParam);
    if (use_cache_param.m_Found) {
        string      err_msg;
        if (!x_IsBoolParamValid(kUseCacheParam,
                                use_cache_param.m_Value, err_msg)) {
            x_MalformedArguments(reply, now, err_msg);
            return false;
        }

        if (use_cache_param.m_Value == "yes") {
            use_cache = SPSGS_RequestBase::ePSGS_CacheOnly;
        } else {
            use_cache = SPSGS_RequestBase::ePSGS_DbOnly;
        }
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetSendBlobIfSmallParameter(CHttpRequest &  req,
                                                 shared_ptr<CPSGS_Reply>  reply,
                                                 const psg_time_point_t &  now,
                                                 int &  send_blob_if_small)
{
    static string   kSendBlobIfSmallParam = "send_blob_if_small";

    send_blob_if_small = 0;   // default

    SRequestParameter   send_blob_if_small_param = x_GetParam(req, kSendBlobIfSmallParam);
    if (send_blob_if_small_param.m_Found) {
        string      err_msg;
        if (!x_ConvertIntParameter(kSendBlobIfSmallParam,
                                   send_blob_if_small_param.m_Value,
                                   send_blob_if_small, err_msg)) {
            x_MalformedArguments(reply, now, err_msg);
            return false;
        }

        if (send_blob_if_small < 0) {
            x_MalformedArguments(reply, now,
                                 "Invalid " + kSendBlobIfSmallParam +
                                 " value. It must be an integer >= 0");
            return false;
        }
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetNames(CHttpRequest &  req,
                              shared_ptr<CPSGS_Reply>  reply,
                              const psg_time_point_t &  now,
                              vector<string> &  names)
{
    static string   kNamesParam = "names";

    SRequestParameter   names_param = x_GetParam(req, kNamesParam);
    if (!names_param.m_Found) {
        x_MalformedArguments(reply, now,
                             "The mandatory '" + kNamesParam +
                             "' parameter is not found");
        return false;
    }

    names.clear();
    NStr::Split(names_param.m_Value, ",", names);
    if (names.empty()) {
        x_MalformedArguments(reply, now,
                             "Named annotation names are not found in the request");
        return false;
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetCommonIDRequestParams(
                                        CHttpRequest &  req,
                                        shared_ptr<CPSGS_Reply>  reply,
                                        const psg_time_point_t &  now,
                                        SPSGS_RequestBase::EPSGS_Trace &  trace,
                                        int &  hops,
                                        vector<string> &  enabled_processors,
                                        vector<string> &  disabled_processors,
                                        bool &  processor_events)
{
    trace = SPSGS_RequestBase::ePSGS_NoTracing;     // default
    if (!x_GetTraceParameter(req, reply, now, trace)) {
        m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
        return false;
    }

    hops = 0;   // default
    if (!x_GetHops(req, reply, now, hops)) {
        m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
        return false;
    }

    enabled_processors.clear();     // default
    disabled_processors.clear();    // default
    if (!x_GetEnabledAndDisabledProcessors(req, reply, now, enabled_processors,
                                           disabled_processors)) {
        m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
        return false;
    }

    processor_events = false;   // default
    if (!x_GetProcessorEventsParameter(req, reply, now, processor_events)) {
        m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
        return false;
    }
    return true;
}


bool
CPubseqGatewayApp::x_ProcessCommonGetAndResolveParams(CHttpRequest &  req,
                                                      shared_ptr<CPSGS_Reply>  reply,
                                                      const psg_time_point_t &  now,
                                                      CTempString &  seq_id,
                                                      int &  seq_id_type,
                                                      SPSGS_RequestBase::EPSGS_CacheAndDbUse &  use_cache,
                                                      bool  seq_id_is_optional)
{
    static string       kSeqIdParam = "seq_id";
    static string       kSeqIdTypeParam = "seq_id_type";

    SRequestParameter   seq_id_type_param;
    string              err_msg;

    // Check the mandatory parameter presence
    SRequestParameter   seq_id_param = x_GetParam(req, kSeqIdParam);
    if (!seq_id_param.m_Found) {
        if (!seq_id_is_optional) {
            x_InsufficientArguments(reply, now,
                                    "Missing the '" + kSeqIdParam + "' parameter");
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return false;
        }
    }
    else if (seq_id_param.m_Value.empty()) {
        if (!seq_id_is_optional) {
            x_MalformedArguments(reply, now,
                                 "Missing value of the '" + kSeqIdParam + "' parameter");
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return false;
        }
    }

    if (seq_id_param.m_Found) {
        if (!seq_id_param.m_Value.empty()) {
            seq_id = seq_id_param.m_Value;
        }
    }

    use_cache = SPSGS_RequestBase::ePSGS_CacheAndDb;
    if (!x_GetUseCacheParameter(req, reply, now, use_cache)) {
        m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
        return false;
    }

    seq_id_type_param = x_GetParam(req, kSeqIdTypeParam);
    if (seq_id_type_param.m_Found) {
        if (!x_ConvertIntParameter(kSeqIdTypeParam, seq_id_type_param.m_Value,
                                   seq_id_type, err_msg)) {
            x_MalformedArguments(reply, now, err_msg);
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return false;
        }

        if (seq_id_type < 0 || seq_id_type >= CSeq_id::e_MaxChoice) {
            err_msg = "The '" + kSeqIdTypeParam +
                      "' value must be >= 0 and less than " +
                      to_string(CSeq_id::e_MaxChoice);
            x_MalformedArguments(reply, now, err_msg);
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return false;
        }
    } else {
        seq_id_type = -1;
    }

    return true;
}


bool
CPubseqGatewayApp::x_GetProtein(CHttpRequest &  req,
                                shared_ptr<CPSGS_Reply>  reply,
                                const psg_time_point_t &  now,
                                optional<string> &  protein)
{
    static string       kProteinParam = "protein";

    SRequestParameter   protein_param = x_GetParam(req, kProteinParam);
    if (protein_param.m_Found) {
        // Note: it is necessary to distinguish if the url value is "" or not
        //       provided. So here there is an assignment even if the parameter
        //       has zero length
        protein = protein_param.m_Value;
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetIPG(CHttpRequest &  req,
                            shared_ptr<CPSGS_Reply>  reply,
                            const psg_time_point_t &  now,
                            int64_t &  ipg)
{
    static string       kIPGParam = "ipg";

    SRequestParameter   ipg_param = x_GetParam(req, kIPGParam);
    if (ipg_param.m_Found) {
        string              err_msg;
        if (!x_ConvertIntParameter(kIPGParam, ipg_param.m_Value,
                                   ipg, err_msg)) {
            x_MalformedArguments(reply, now, err_msg);
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return false;
        }

        if (ipg <= 0) {
            err_msg = "The '" + kIPGParam +
                      "' value must be > 0";
            x_MalformedArguments(reply, now, err_msg);
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return false;
        }
    } else {
        ipg = -1;
    }

    return true;
}


bool
CPubseqGatewayApp::x_GetNucleotide(CHttpRequest &  req,
                                   shared_ptr<CPSGS_Reply>  reply,
                                   const psg_time_point_t &  now,
                                   optional<string> &  nucleotide)
{
    static string       kNucleotideParam = "nucleotide";

    SRequestParameter   nucleotide_param = x_GetParam(req, kNucleotideParam);
    if (nucleotide_param.m_Found) {
        // Note: it is necessary to distinguish if the url value is "" or not
        //       provided. So here there is an assignment even if the parameter
        //       has zero length
        nucleotide = nucleotide_param.m_Value;
    }
    return true;
}


bool
CPubseqGatewayApp::x_GetSNPScaleLimit(CHttpRequest &  req,
                                      shared_ptr<CPSGS_Reply>  reply,
                                      const psg_time_point_t &  now,
                                      optional<CSeq_id::ESNPScaleLimit> &  snp_scale_limit)
{
    static string       kSNPScaleLimitParam = "snp_scale_limit";
    static string       kChromosome = "chromosome";
    static string       kContig = "contig";
    static string       kSupercontig = "supercontig";
    static string       kUnit = "unit";

    SRequestParameter   snp_scale_limit_param = x_GetParam(req, kSNPScaleLimitParam);
    if (snp_scale_limit_param.m_Found) {
        if (snp_scale_limit_param.m_Value.empty()) {
            // Empty string is OK. There is no assignment so the processor will
            // see it as 'not set'
            return true;
        }
        if (snp_scale_limit_param.m_Value == kChromosome) {
            snp_scale_limit = CSeq_id::eSNPScaleLimit_Chromosome;
            return true;
        }
        if (snp_scale_limit_param.m_Value == kContig) {
            snp_scale_limit = CSeq_id::eSNPScaleLimit_Contig;
            return true;
        }
        if (snp_scale_limit_param.m_Value == kSupercontig) {
            snp_scale_limit = CSeq_id::eSNPScaleLimit_Supercontig;
            return true;
        }
        if (snp_scale_limit_param.m_Value == kUnit) {
            snp_scale_limit = CSeq_id::eSNPScaleLimit_Unit;
            return true;
        }

        x_MalformedArguments(reply, now,
                             "Malformed '" + kSNPScaleLimitParam + "' parameter. "
                             "Acceptable values are '" +
                             kChromosome + "' and '" +
                             kContig + "' and '" +
                             kSupercontig + "' and '" +
                             kUnit + "'.");
        return false;
    }

    // This is the case when there was no parameter in the incoming URL
    // There is no assignment so the processors will see it as 'not set'
    return true;
}


bool
CPubseqGatewayApp::x_GetTimeSeries(CHttpRequest &  req,
                                   shared_ptr<CPSGS_Reply>  reply,
                                   const psg_time_point_t &  now,
                                   vector<pair<int, int>> &  time_series)
{
    // It is like "1:59 5:1439 60:"
    // Should return {{1, 59}, {5, 1439}, {60, numeric_limits<int>::max()}}
    static string                   kTimeSeriesParam = "time_series";
    static vector<pair<int, int>>   kDefaultTimeSeries = {{1, 59}, {5, 1439},
                                                          {60, numeric_limits<int>::max()}};

    SRequestParameter   time_series_param = x_GetParam(req, kTimeSeriesParam);
    if (time_series_param.m_Found) {
        time_series.clear();

        if (time_series_param.m_Value.empty()) {
            x_MalformedArguments(reply, now, "The '" + kTimeSeriesParam +
                                 "' parameter is empty. Expected at leas one "
                                 "space separated pair of integers "
                                 "<aggregation mins>:<last minute> or 'no'");
            return false;
        }

        if (time_series_param.m_Value == "no") {
            // Special value: 'no' means there will be no time series
            // The caller knows that it is a special value by an empty
            // time_series container
            return true;
        }

        bool                last = false;
        int                 previous = -1;
        vector<string>      parts;
        NStr::Split(time_series_param.m_Value, " ", parts);
        for (auto &  item : parts) {
            if (last) {
                x_MalformedArguments(reply, now, "The '" + kTimeSeriesParam +
                                     "' is malformed. Another item is found "
                                     "after the one which describes the rest "
                                     "of the time series.");

                return false;
            }

            vector<string>  vals;
            NStr::Split(item, ":", vals);
            if (vals.size() != 2) {
                x_MalformedArguments(reply, now, "The '" + kTimeSeriesParam +
                                     "' is malformed. One or more items "
                                     "do not have a second value.");
                return false;
            }

            int     aggregation;
            try {
                aggregation = NStr::StringToInt(vals[0]);
            } catch (...) {
                x_MalformedArguments(reply, now, "The '" + kTimeSeriesParam +
                                     "' is malformed. Cannot convert one or more "
                                     "aggregation mins into an integer");
                return false;
            }
            if (aggregation <= 0) {
                x_MalformedArguments(reply, now, "The '" + kTimeSeriesParam +
                                     "' is malformed. One or more "
                                     "aggregation mins is <= 0 while it must be > 0.");
                return false;
            }

            int     last_minute;
            if (vals[1].empty()) {
                last_minute = numeric_limits<int>::max();
                last = true;
            } else {
                try {
                    last_minute = NStr::StringToInt(vals[1]);
                } catch (...) {
                    x_MalformedArguments(reply, now, "The '" + kTimeSeriesParam +
                                         "' is malformed. Cannot convert one or more "
                                         "last minute into an integer");
                    return false;
                }

                if (last_minute <= previous) {
                    x_MalformedArguments(reply, now, "The '" + kTimeSeriesParam +
                                         "' is malformed. One or more last minute "
                                         "<= than the previous one");
                    return false;
                }

                // Check divisibility
                int     start = 0;
                if (previous != -1) {
                    start = previous + 1;
                }
                if ((last_minute - start + 1) % aggregation != 0) {
                   x_MalformedArguments(reply, now, "The '" + kTimeSeriesParam +
                                         "' is malformed. The range " +
                                         to_string(start) + "-" +
                                         to_string(last_minute) +
                                         " is not divisable by aggregation of " +
                                         to_string(aggregation));
                    return false;
                }

                previous = last_minute;
            }

            time_series.push_back(make_pair(aggregation, last_minute));
        }

        if (!last) {
            x_MalformedArguments(reply, now, "The '" + kTimeSeriesParam +
                                 "' is malformed. The item which descibes the "
                                 "rest of the series is not found.");
            return false;
        }

        return true;
    }

    // The user did not provide the parameter
    time_series = kDefaultTimeSeries;
    return true;
}

