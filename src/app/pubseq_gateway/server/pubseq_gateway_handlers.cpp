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

#include <corelib/ncbithr.hpp>
#include <corelib/ncbidiag.hpp>
#include <corelib/request_ctx.hpp>

#include <objtools/pubseq_gateway/impl/cassandra/blob_storage.hpp>

#include "http_request.hpp"
#include "http_reply.hpp"
#include "http_connection.hpp"
#include "pubseq_gateway.hpp"
#include "pubseq_gateway_exception.hpp"
#include "resolve_processor.hpp"

#include "shutdown_data.hpp"
extern SShutdownData    g_ShutdownData;


USING_NCBI_SCOPE;


static string  kTSELastModifiedParam = "tse_last_modified";
static string  kSeqIdsParam = "seq_ids";
static string  kClientIdParam = "client_id";
static string  kAuthTokenParam = "auth_token";
static string  kTimeoutParam = "timeout";
static string  kDataSizeParam = "return_data_size";
static string  kLogParam = "log";
static string  kUsernameParam = "username";
static string  kAlertParam = "alert";
static string  kResetParam = "reset";
static string  kId2InfoParam = "id2_info";
static string  kMostRecentTimeParam = "most_recent_time";
static string  kMostAncientTimeParam = "most_ancient_time";
static string  kHistogramNamesParam = "histogram_names";
static string  kNA = "n/a";

static string  kBadUrlMessage = "Unknown request, the provided URL "
                                "is not recognized: ";


int CPubseqGatewayApp::OnBadURL(CHttpRequest &  req,
                                shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context,
                           CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    if (req.GetPath() == "/") {
        // Special case: no path at all so provide a help message
        try {
            string      fmt;
            string      err_msg;
            if (!x_GetIntrospectionFormat(req, fmt, err_msg)) {
                // Basically an incorrect format parameter
                reply->Send400(err_msg.c_str());
                PSG_ERROR(err_msg);
                x_PrintRequestStop(context,
                                   CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e400_BadRequest,
                                   reply->GetBytesSent());
                return 0;
            }

            if (fmt == "json") {
                // format is json
                reply->SetContentType(ePSGS_JsonMime);
                reply->SetContentLength(m_HelpMessageJson.size());
                // true => persistent
                reply->SendOk(m_HelpMessageJson.data(),
                              m_HelpMessageJson.size(), true);
                x_PrintRequestStop(context,
                                   CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e200_Ok,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
            } else {
                // format is html
                reply->SetContentType(ePSGS_HtmlMime);
                reply->SetContentLength(m_HelpMessageHtml.size());
                // true => persistent
                reply->SendOk(m_HelpMessageHtml.data(),
                              m_HelpMessageHtml.size(), true);
                x_PrintRequestStop(context,
                                   CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e200_Ok,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
            }
        } catch (const exception &  exc) {
            x_Finish500(reply, now, ePSGS_BadURL,
                        "Exception when handling no path URL event: " +
                        string(exc.what()));
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e500_InternalServerError,
                               reply->GetBytesSent());
        } catch (...) {
            x_Finish500(reply, now, ePSGS_BadURL,
                        "Unknown exception when handling no path URL event");
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e500_InternalServerError,
                               reply->GetBytesSent());
        }
    } else {
        try {
            string      bad_url = req.GetPath();
            x_SendMessageAndCompletionChunks(reply, now,
                                             kBadUrlMessage + NStr::PrintableString(bad_url),
                                             CRequestStatus::e400_BadRequest,
                                             ePSGS_BadURL,
                                             eDiag_Error);
            auto    bad_url_size = bad_url.size();
            if (bad_url_size > 128) {
                bad_url.resize(128);
                bad_url += "... (truncated; original length: " +
                           to_string(bad_url_size) + ")";
            }

            PSG_WARNING(kBadUrlMessage + bad_url);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_BadUrlPath);
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
        } catch (const exception &  exc) {
            x_Finish500(reply, now, ePSGS_BadURL,
                        "Exception when handling a bad URL event: " +
                        string(exc.what()));
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
        } catch (...) {
            x_Finish500(reply, now, ePSGS_BadURL,
                        "Unknown exception when handling a bad URL event");
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
        }
    }
    return 0;
}


int CPubseqGatewayApp::OnGet(CHttpRequest &  req,
                             shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    try {
        SPSGS_RequestBase::EPSGS_Trace  trace = SPSGS_RequestBase::ePSGS_NoTracing;
        int                             hops = 0;
        vector<string>                  enabled_processors;
        vector<string>                  disabled_processors;
        bool                            processor_events = false;

        if (!x_GetCommonIDRequestParams(req, reply, now, trace, hops,
                                        enabled_processors, disabled_processors,
                                        processor_events)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        CTempString                             seq_id;
        int                                     seq_id_type;
        SPSGS_RequestBase::EPSGS_CacheAndDbUse  use_cache = SPSGS_RequestBase::ePSGS_CacheAndDb;

        if (!x_ProcessCommonGetAndResolveParams(req, reply, now, seq_id,
                                                seq_id_type, use_cache)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        SPSGS_BlobRequestBase::EPSGS_TSEOption  tse_option = SPSGS_BlobRequestBase::ePSGS_OrigTSE;
        if (!x_GetTSEOption(req, reply, now, tse_option)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        vector<string>      exclude_blobs = x_GetExcludeBlobs(req);

        SPSGS_RequestBase::EPSGS_AccSubstitutioOption
                                subst_option = SPSGS_RequestBase::ePSGS_DefaultAccSubstitution;
        if (!x_GetAccessionSubstitutionOption(req, reply, now, subst_option)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        SRequestParameter   client_id_param = x_GetParam(req, kClientIdParam);

        double  resend_timeout;
        if (!x_GetResendTimeout(req, reply, now, resend_timeout)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        int     send_blob_if_small = 0;
        if (!x_GetSendBlobIfSmallParameter(req, reply, now, send_blob_if_small)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        bool    seq_id_resolve = true;  // default
        if (!x_GetSeqIdResolveParameter(req, reply, now, seq_id_resolve)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        unique_ptr<SPSGS_RequestBase>
            req(new SPSGS_BlobBySeqIdRequest(
                        string(seq_id.data(), seq_id.size()),
                        seq_id_type, exclude_blobs,
                        tse_option, use_cache, subst_option,
                        resend_timeout,
                        string(client_id_param.m_Value.data(),
                               client_id_param.m_Value.size()),
                        send_blob_if_small, seq_id_resolve,
                        hops, trace,
                        processor_events,
                        enabled_processors, disabled_processors,
                        now));
        shared_ptr<CPSGS_Request>
            request(new CPSGS_Request(move(req), context));

        bool    have_proc = x_DispatchRequest(context, request, reply);
        if (!have_proc) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                               CRequestStatus::e404_NotFound,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NoProcessorInstantiated);
        }
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Exception when handling a get request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Unknown exception when handling a get request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySeqIdRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnGetBlob(CHttpRequest &  req,
                                 shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    try {
        SPSGS_RequestBase::EPSGS_Trace  trace = SPSGS_RequestBase::ePSGS_NoTracing;
        int                             hops = 0;
        vector<string>                  enabled_processors;
        vector<string>                  disabled_processors;
        bool                            processor_events = false;

        if (!x_GetCommonIDRequestParams(req, reply, now, trace, hops,
                                        enabled_processors, disabled_processors,
                                        processor_events)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        SPSGS_BlobRequestBase::EPSGS_TSEOption
                            tse_option = SPSGS_BlobRequestBase::ePSGS_OrigTSE;
        if (!x_GetTSEOption(req, reply, now, tse_option)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        int64_t             last_modified = INT64_MIN;
        if (!x_GetLastModified(req, reply, now, last_modified)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        SPSGS_RequestBase::EPSGS_CacheAndDbUse  use_cache = SPSGS_RequestBase::ePSGS_CacheAndDb;
        if (!x_GetUseCacheParameter(req, reply, now, use_cache)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        int     send_blob_if_small = 0;
        if (!x_GetSendBlobIfSmallParameter(req, reply, now, send_blob_if_small)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        SRequestParameter   client_id_param = x_GetParam(req, kClientIdParam);

        SPSGS_BlobId        blob_id;
        if (!x_GetBlobId(req, reply, now, blob_id)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        unique_ptr<SPSGS_RequestBase>
                req(new SPSGS_BlobBySatSatKeyRequest(
                            blob_id, last_modified,
                            tse_option, use_cache,
                            string(client_id_param.m_Value.data(),
                                   client_id_param.m_Value.size()),
                            send_blob_if_small, hops, trace,
                            processor_events,
                            enabled_processors, disabled_processors, now));
        shared_ptr<CPSGS_Request>
                request(new CPSGS_Request(move(req), context));

        bool    have_proc = x_DispatchRequest(context, request, reply);
        if (!have_proc) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                               CRequestStatus::e404_NotFound,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NoProcessorInstantiated);
        }
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Exception when handling a getblob request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Unknown exception when handling a getblob request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_BlobBySatSatKeyRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnResolve(CHttpRequest &  req,
                                 shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    try {
        SPSGS_RequestBase::EPSGS_Trace  trace = SPSGS_RequestBase::ePSGS_NoTracing;
        int                             hops = 0;
        vector<string>                  enabled_processors;
        vector<string>                  disabled_processors;
        bool                            processor_events = false;

        if (!x_GetCommonIDRequestParams(req, reply, now, trace, hops,
                                        enabled_processors, disabled_processors,
                                        processor_events)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        CTempString                             seq_id;
        int                                     seq_id_type;
        SPSGS_RequestBase::EPSGS_CacheAndDbUse  use_cache = SPSGS_RequestBase::ePSGS_CacheAndDb;

        if (!x_ProcessCommonGetAndResolveParams(req, reply, now, seq_id,
                                                seq_id_type, use_cache)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        SPSGS_ResolveRequest::EPSGS_OutputFormat
                            output_format = SPSGS_ResolveRequest::ePSGS_NativeFormat;
        if (!x_GetOutputFormat(req, reply, now, output_format)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        SPSGS_ResolveRequest::TPSGS_BioseqIncludeData   include_data_flags = 0;
        if (!x_GetResolveFlags(req, reply, now, include_data_flags)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        SPSGS_RequestBase::EPSGS_AccSubstitutioOption
                                subst_option = SPSGS_RequestBase::ePSGS_DefaultAccSubstitution;
        if (!x_GetAccessionSubstitutionOption(req, reply, now, subst_option)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        bool                seq_id_resolve = true;  // default
        if (!x_GetSeqIdResolveParameter(req, reply, now, seq_id_resolve)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        // Parameters processing has finished
        unique_ptr<SPSGS_RequestBase>
            req(new SPSGS_ResolveRequest(
                        string(seq_id.data(), seq_id.size()),
                        seq_id_type, include_data_flags, output_format,
                        use_cache, subst_option, seq_id_resolve, hops, trace,
                        processor_events,
                        enabled_processors, disabled_processors, now));
        shared_ptr<CPSGS_Request>
            request(new CPSGS_Request(move(req), context));

        bool    have_proc = x_DispatchRequest(context, request, reply);
        if (!have_proc) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                               CRequestStatus::e404_NotFound,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NoProcessorInstantiated);
        }
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Exception when handling a resolve request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Unknown exception when handling a resolve request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnGetTSEChunk(CHttpRequest &  req,
                                     shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_TSEChunkRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    try {
        SPSGS_RequestBase::EPSGS_Trace  trace = SPSGS_RequestBase::ePSGS_NoTracing;
        int                             hops = 0;
        vector<string>                  enabled_processors;
        vector<string>                  disabled_processors;
        bool                            processor_events = false;

        if (!x_GetCommonIDRequestParams(req, reply, now, trace, hops,
                                        enabled_processors, disabled_processors,
                                        processor_events)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_TSEChunkRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        // Mandatory parameter id2_chunk
        int64_t             id2_chunk_value = INT64_MIN;
        if (!x_GetId2Chunk(req, reply, now, id2_chunk_value)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_TSEChunkRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        SRequestParameter   id2_info_param = x_GetParam(req, kId2InfoParam);
        if (!id2_info_param.m_Found)
        {
            x_InsufficientArguments(reply, now, "Mandatory parameter '" +
                                    kId2InfoParam + "' is not found.");
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_TSEChunkRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        SPSGS_RequestBase::EPSGS_CacheAndDbUse  use_cache = SPSGS_RequestBase::ePSGS_CacheAndDb;
        if (!x_GetUseCacheParameter(req, reply, now, use_cache)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_TSEChunkRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        // All parameters are good
        unique_ptr<SPSGS_RequestBase>
            req(new SPSGS_TSEChunkRequest(
                        id2_chunk_value, id2_info_param.m_Value,
                        use_cache, hops, trace, processor_events,
                        enabled_processors, disabled_processors, now));
        shared_ptr<CPSGS_Request>
            request(new CPSGS_Request(move(req), context));

        bool    have_proc = x_DispatchRequest(context, request, reply);
        if (!have_proc) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_TSEChunkRequest,
                               CRequestStatus::e404_NotFound,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NoProcessorInstantiated);
        }
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Exception when handling a get_tse_chunk request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_TSEChunkRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Unknown exception when handling a get_tse_chunk request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_TSEChunkRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnGetNA(CHttpRequest &  req,
                               shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    try {
        SPSGS_RequestBase::EPSGS_Trace  trace = SPSGS_RequestBase::ePSGS_NoTracing;
        int                             hops = 0;
        vector<string>                  enabled_processors;
        vector<string>                  disabled_processors;
        bool                            processor_events = false;

        if (!x_GetCommonIDRequestParams(req, reply, now, trace, hops,
                                        enabled_processors, disabled_processors,
                                        processor_events)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        CTempString                             seq_id;
        int                                     seq_id_type;
        SPSGS_RequestBase::EPSGS_CacheAndDbUse  use_cache = SPSGS_RequestBase::ePSGS_CacheAndDb;

        // true => seq_id is optional
        if (!x_ProcessCommonGetAndResolveParams(req, reply, now, seq_id,
                                                seq_id_type, use_cache, true)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        SPSGS_ResolveRequest::EPSGS_OutputFormat
                        output_format = SPSGS_ResolveRequest::ePSGS_JsonFormat;
        if (!x_GetOutputFormat(req, reply, now, output_format)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        // At the moment the only json format is supported (native translates
        // to json). See CXX-10258
        if (output_format != SPSGS_ResolveRequest::ePSGS_JsonFormat &&
            output_format != SPSGS_ResolveRequest::ePSGS_NativeFormat) {
            x_MalformedArguments(
                    reply, now,
                    "Invalid 'fmt' parameter value. The 'get_na' "
                    "request supports 'json' and 'native' values");
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }


        // Get the annotation names
        vector<string>          names;
        if (!x_GetNames(req, reply, now, names)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        // Get other seq ids
        SRequestParameter       seq_ids_param = x_GetParam(req, kSeqIdsParam);
        vector<string>          seq_ids;
        if (seq_ids_param.m_Found) {
            if (!seq_ids_param.m_Value.empty()) {
                NStr::Split(seq_ids_param.m_Value, " ", seq_ids);
            }
        }

        if (seq_id.empty() && seq_ids.empty()) {
            x_MalformedArguments(reply, now,
                                 "Neither 'seq_id' nor '" + kSeqIdsParam +
                                 "' are found in the request");
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        // Sanity for seq_id and other seq ids:
        // - remove duplicates from the other seq ids
        // - check that seq_id is not in other seq ids
        if (!seq_ids.empty()) {
            sort(seq_ids.begin(), seq_ids.end());
            auto    last = unique(seq_ids.begin(), seq_ids.end());
            seq_ids.erase(last, seq_ids.end());
        }

        if (!seq_id.empty()) {
            auto    it = find(seq_ids.begin(), seq_ids.end(), seq_id);
            if (it != seq_ids.end()) {
                seq_ids.erase(it);
            }
        }

        SPSGS_BlobRequestBase::EPSGS_TSEOption
                            tse_option = SPSGS_BlobRequestBase::ePSGS_NoneTSE;
        if (!x_GetTSEOption(req, reply, now, tse_option)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        SRequestParameter   client_id_param = x_GetParam(req, kClientIdParam);

        int     send_blob_if_small = 0;
        if (!x_GetSendBlobIfSmallParameter(req, reply, now, send_blob_if_small)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        double              resend_timeout;
        if (!x_GetResendTimeout(req, reply, now, resend_timeout)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        bool                seq_id_resolve = true;  // default
        if (!x_GetSeqIdResolveParameter(req, reply, now, seq_id_resolve)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        optional<CSeq_id::ESNPScaleLimit>    snp_scale_limit;
        if (!x_GetSNPScaleLimit(req, reply, now, snp_scale_limit)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        // Parameters processing has finished
        unique_ptr<SPSGS_RequestBase>
            req(new SPSGS_AnnotRequest(
                        string(seq_id.data(), seq_id.size()),
                        seq_id_type, names, use_cache,
                        resend_timeout,
                        seq_ids,
                        string(client_id_param.m_Value.data(),
                               client_id_param.m_Value.size()),
                        tse_option, send_blob_if_small, seq_id_resolve,
                        snp_scale_limit,
                        hops, trace, processor_events,
                        enabled_processors, disabled_processors,
                        now));
        shared_ptr<CPSGS_Request>
            request(new CPSGS_Request(move(req), context));

        bool    have_proc = x_DispatchRequest(context, request, reply);
        if (!have_proc) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                               CRequestStatus::e404_NotFound,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NoProcessorInstantiated);
        }
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Exception when handling a get_na request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Unknown exception when handling a get_na request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_AnnotationRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnAccessionVersionHistory(CHttpRequest &  req,
                                                 shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context,
                           CPSGS_Request::ePSGS_AccessionVersionHistoryRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    try {
        SPSGS_RequestBase::EPSGS_Trace  trace = SPSGS_RequestBase::ePSGS_NoTracing;
        int                             hops = 0;
        vector<string>                  enabled_processors;
        vector<string>                  disabled_processors;
        bool                            processor_events = false;

        if (!x_GetCommonIDRequestParams(req, reply, now, trace, hops,
                                        enabled_processors, disabled_processors,
                                        processor_events)) {
            x_PrintRequestStop(context,
                               CPSGS_Request::ePSGS_AccessionVersionHistoryRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        CTempString                             seq_id;
        int                                     seq_id_type;
        SPSGS_RequestBase::EPSGS_CacheAndDbUse  use_cache = SPSGS_RequestBase::ePSGS_CacheAndDb;

        if (!x_ProcessCommonGetAndResolveParams(req, reply, now, seq_id,
                                                seq_id_type, use_cache)) {
            x_PrintRequestStop(context,
                               CPSGS_Request::ePSGS_AccessionVersionHistoryRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        // Parameters processing has finished
        unique_ptr<SPSGS_RequestBase>
            req(new SPSGS_AccessionVersionHistoryRequest(
                        string(seq_id.data(), seq_id.size()),
                        seq_id_type, use_cache, hops, trace, processor_events,
                        enabled_processors, disabled_processors,
                        now));
        shared_ptr<CPSGS_Request>
            request(new CPSGS_Request(move(req), context));

        bool    have_proc = x_DispatchRequest(context, request, reply);
        if (!have_proc) {
            x_PrintRequestStop(context,
                               CPSGS_Request::ePSGS_AccessionVersionHistoryRequest,
                               CRequestStatus::e404_NotFound,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NoProcessorInstantiated);
        }
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Exception when handling an accession_version_history request: " +
                    string(exc.what()));
        x_PrintRequestStop(context,
                           CPSGS_Request::ePSGS_AccessionVersionHistoryRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Unknown exception when handling an accession_version_history request");
        x_PrintRequestStop(context,
                           CPSGS_Request::ePSGS_AccessionVersionHistoryRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }

    return 0;
}


int CPubseqGatewayApp::OnIPGResolve(CHttpRequest &  req,
                                    shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    try {
        vector<string>                  enabled_processors;
        vector<string>                  disabled_processors;
        bool                            processor_events = false;

        if (!x_GetEnabledAndDisabledProcessors(req, reply, now, enabled_processors,
                                               disabled_processors)) {
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        processor_events = false;   // default
        if (!x_GetProcessorEventsParameter(req, reply, now, processor_events)) {
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        optional<string>        protein;
        if (!x_GetProtein(req, reply, now, protein)) {
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        optional<string>        nucleotide;
        if (!x_GetNucleotide(req, reply, now, nucleotide)) {
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        int64_t                         ipg = -1;
        if (!x_GetIPG(req, reply, now, ipg)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        // Prohibit protein less requests
        if (nucleotide.has_value()) {
            if (!protein.has_value()) {
                x_InsufficientArguments(reply, now,
                                        "If a 'nucleotide' parameter is provided "
                                        "then a 'protein' parameter"
                                        " must be provided as well");
                m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                                   CRequestStatus::e400_BadRequest,
                                   reply->GetBytesSent());
                return 0;
            }
        }

        // Check that at least one of the mandatory parameters are provided
        if (ipg == -1 && !protein.has_value()) {
            x_InsufficientArguments(reply, now,
                                    "At least one of the 'protein' and 'ipg' "
                                    "parameters must be provided");
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        SPSGS_RequestBase::EPSGS_Trace  trace = SPSGS_RequestBase::ePSGS_NoTracing;
        if (!x_GetTraceParameter(req, reply, now, trace)) {
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            return 0;
        }

        // Parameters processing has finished
        unique_ptr<SPSGS_RequestBase>
            req(new SPSGS_IPGResolveRequest(
                        protein,
                        ipg,
                        nucleotide,
                        trace, processor_events,
                        enabled_processors, disabled_processors,
                        now));
        shared_ptr<CPSGS_Request>
            request(new CPSGS_Request(move(req), context));

        bool    have_proc = x_DispatchRequest(context, request, reply);
        if (!have_proc) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                               CRequestStatus::e404_NotFound,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NoProcessorInstantiated);
        }
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Exception when handling an IPG resolve request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_UnknownError,
                    "Unknown exception when handling an IPG resolve request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_IPGResolveRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }

    return 0;
}


int CPubseqGatewayApp::OnHealth(CHttpRequest &  req,
                                shared_ptr<CPSGS_Reply>  reply)
{
    static string   separator = "==============================================";
    static string   prefix = "PSG_HEALTH_ERROR: ";

    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    auto    startup_data_state = GetStartupDataState();
    if (startup_data_state != ePSGS_StartupDataOK) {
        // Something is wrong with access to Cassandra
        // Check if there are any active alerts
        auto        active_alerts = m_Alerts.SerializeActive();
        string      msg = separator + "\n" +
                          prefix + "CASSANDRA" "\n" +
                          GetCassStartupDataStateMessage(startup_data_state) + "\n" +
                          separator + "\n" +
                          prefix + "ALERTS" "\n";
        if (active_alerts.GetSize() == 0) {
            msg += "There are no active alerts";
        } else {
            msg += "Active alerts are:" "\n" +
                   active_alerts.Repr(CJsonNode::fStandardJson);
        }

        reply->SetContentType(ePSGS_PlainTextMime);
        reply->Send500(msg.c_str());
        PSG_WARNING("Cassandra is not available or is in non-working state");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
        return 0;
    }

    if (m_Settings.m_TestSeqId.empty()) {
        // seq_id for a health test is not configured so skip the test
        reply->SetContentType(ePSGS_PlainTextMime);
        reply->SetContentLength(0);
        reply->SendOk(nullptr, 0, true);
        PSG_WARNING("Test seq_id resolution skipped (configured as an empty string)");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e200_Ok, reply->GetBytesSent());
        m_Counters.Increment(CPSGSCounters::ePSGS_HealthRequest);
        return 0;
    }

    if (m_Settings.m_Si2csiDbFile.empty() ||
        m_Settings.m_BioseqInfoDbFile.empty()) {
        // Cache is not configured so skip the test
        reply->SetContentType(ePSGS_PlainTextMime);
        reply->SetContentLength(0);
        reply->SendOk(nullptr, 0, true);
        PSG_WARNING("Test seq_id resolution skipped (cache is not configured)");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e200_Ok, reply->GetBytesSent());
        m_Counters.Increment(CPSGSCounters::ePSGS_HealthRequest);
        return 0;
    }

    // If cache is configured then we can give it a try to resolve the
    // configured seq_id in cache
    try {
        vector<string>      enabled_processors;
        vector<string>      disabled_processors;

        unique_ptr<SPSGS_RequestBase>
            req(new SPSGS_ResolveRequest(m_Settings.m_TestSeqId, -1,
                                         SPSGS_ResolveRequest::fPSGS_CanonicalId,
                                         SPSGS_ResolveRequest::ePSGS_JsonFormat,
                                         SPSGS_RequestBase::ePSGS_CacheOnly,
                                         SPSGS_RequestBase::ePSGS_NeverAccSubstitute,
                                         true,
                                         0, SPSGS_RequestBase::ePSGS_NoTracing,
                                         false,     // no processor messages
                                         enabled_processors, disabled_processors,
                                         now));
        shared_ptr<CPSGS_Request>
            request(new CPSGS_Request(move(req), CRef<CRequestContext>()));


        CPSGS_ResolveProcessor  resolve_processor(request, reply, 0);
        auto    resolution = resolve_processor.ResolveTestInputSeqId();

        if (!resolution.IsValid()) {
            if (!m_Settings.m_TestSeqIdIgnoreError) {
                string  msg = separator + "\n" +
                              prefix + "RESOLUTION" "\n";
                if (resolution.m_Error.HasError()) {
                    msg += resolution.m_Error.m_ErrorMessage;
                } else {
                    msg += "Cannot resolve '" + m_Settings.m_TestSeqId + "' seq_id";
                }
                reply->SetContentType(ePSGS_PlainTextMime);
                reply->Send500(msg.c_str());
                PSG_WARNING("Cannot resolve test seq_id '" + m_Settings.m_TestSeqId + "'");
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e500_InternalServerError,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_HealthRequest);
                return 0;
            }
            PSG_WARNING("Cannot resolve test seq_id '" + m_Settings.m_TestSeqId +
                        "', however the configuration is to ignore test errors");
        }
    } catch (const exception &  exc) {
        if (!m_Settings.m_TestSeqIdIgnoreError) {
            string  msg = separator + "\n" +
                          prefix + "RESOLUTION" "\n" +
                          exc.what();
            reply->SetContentType(ePSGS_PlainTextMime);
            reply->Send500(msg.c_str());
            PSG_WARNING("Cannot resolve test seq_id '" + m_Settings.m_TestSeqId + "'");
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e500_InternalServerError,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_HealthRequest);
            return 0;
        }
        PSG_WARNING("Cannot resolve test seq_id '" + m_Settings.m_TestSeqId +
                    "', however the configuration is to ignore test errors");
    } catch (...) {
        if (!m_Settings.m_TestSeqIdIgnoreError) {
            string  msg = separator + "\n" +
                          prefix + "RESOLUTION" "\n"
                          "Unknown '" + m_Settings.m_TestSeqId + "' resolution error";
            reply->SetContentType(ePSGS_PlainTextMime);
            reply->Send500(msg.c_str());
            PSG_WARNING("Cannot resolve test seq_id '" + m_Settings.m_TestSeqId + "'");
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e500_InternalServerError,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_HealthRequest);
            return 0;
        }
        PSG_WARNING("Cannot resolve test seq_id '" + m_Settings.m_TestSeqId +
                    "', however the configuration is to ignore test errors");
    }

    // Here: all OK or errors are ignored
    reply->SetContentType(ePSGS_PlainTextMime);
    reply->SetContentLength(0);
    reply->SendOk(nullptr, 0, true);
    x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                       CRequestStatus::e200_Ok, reply->GetBytesSent());
    m_Counters.Increment(CPSGSCounters::ePSGS_HealthRequest);
    return 0;
}


int CPubseqGatewayApp::OnConfig(CHttpRequest &  req,
                                shared_ptr<CPSGS_Reply>  reply)
{
    static string   kConfigurationFilePath = "ConfigurationFilePath";
    static string   kConfiguration = "Configuration";

    // NOTE: expected to work regardless of the shutdown request

    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    try {
        CNcbiOstrstream             conf;
        CNcbiOstrstreamToString     converter(conf);

        GetConfig().Write(conf);

        CJsonNode   conf_info(CJsonNode::NewObjectNode());
        conf_info.SetString(kConfigurationFilePath, GetConfigPath());
        conf_info.SetString(kConfiguration, string(converter));
        string      content = conf_info.Repr(CJsonNode::fStandardJson);

        reply->SetContentType(ePSGS_JsonMime);
        reply->SetContentLength(content.size());
        reply->SendOk(content.data(), content.size(), false);

        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e200_Ok, reply->GetBytesSent());
        m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_ConfigError,
                    "Exception when handling a config request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_ConfigError,
                    "Unknown exception when handling a config request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnInfo(CHttpRequest &  req,
                              shared_ptr<CPSGS_Reply>  reply)
{
    static string   kPID = "PID";
    static string   kExecutablePath = "ExecutablePath";
    static string   kCommandLineArguments = "CommandLineArguments";
    static string   kStartupDataState = "StartupDataState";
    static string   kRealTime = "RealTime";
    static string   kUserTime = "UserTime";
    static string   kSystemTime = "SystemTime";
    static string   kPhysicalMemory = "PhysicalMemory";
    static string   kMemoryUsedTotal = "MemoryUsedTotal";
    static string   kMemoryUsedTotalPeak = "MemoryUsedTotalPeak";
    static string   kMemoryUsedResident = "MemoryUsedResident";
    static string   kMemoryUsedResidentPeak = "MemoryUsedResidentPeak";
    static string   kMemoryUsedShared = "MemoryUsedShared";
    static string   kMemoryUsedData = "MemoryUsedData";
    static string   kMemoryUsedStack = "MemoryUsedStack";
    static string   kMemoryUsedText = "MemoryUsedText";
    static string   kMemoryUsedLib = "MemoryUsedLib";
    static string   kMemoryUsedSwap = "MemoryUsedSwap";
    static string   kProcFDSoftLimit = "ProcFDSoftLimit";
    static string   kProcFDHardLimit = "ProcFDHardLimit";
    static string   kProcFDUsed = "ProcFDUsed";
    static string   kCPUCount = "CPUCount";
    static string   kProcThreadCount = "ProcThreadCount";
    static string   kVersion = "Version";
    static string   kBuildDate = "BuildDate";
    static string   kStartedAt = "StartedAt";
    static string   kExcludeBlobCacheUserCount = "ExcludeBlobCacheUserCount";
    static string   kConcurrentPrefix = "ConcurrentProcCount_";

    // NOTE: expected to work regardless of the shutdown request

    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    auto                    app = CPubseqGatewayApp::GetInstance();
    try {
        CJsonNode   info(CJsonNode::NewObjectNode());

        info.SetInteger(kPID, CDiagContext::GetPID());
        info.SetString(kExecutablePath, GetProgramExecutablePath());
        info.SetString(kCommandLineArguments, x_GetCmdLineArguments());
        info.SetString(kStartupDataState,
                       GetCassStartupDataStateMessage(app->GetStartupDataState()));


        double      real_time;
        double      user_time;
        double      system_time;
        bool        process_time_result = CCurrentProcess::GetTimes(&real_time,
                                                                    &user_time,
                                                                    &system_time);
        if (process_time_result) {
            info.SetDouble(kRealTime, real_time);
            info.SetDouble(kUserTime, user_time);
            info.SetDouble(kSystemTime, system_time);
        } else {
            info.SetString(kRealTime, kNA);
            info.SetString(kUserTime, kNA);
            info.SetString(kSystemTime, kNA);
        }

        Uint8       physical_memory = CSystemInfo::GetTotalPhysicalMemorySize();
        if (physical_memory > 0)
            info.SetInteger(kPhysicalMemory, physical_memory);
        else
            info.SetString(kPhysicalMemory, kNA);

        CProcessBase::SMemoryUsage      mem_usage;
        bool                            mem_used_result = CCurrentProcess::GetMemoryUsage(mem_usage);
        if (mem_used_result) {
            if (mem_usage.total > 0)
                info.SetInteger(kMemoryUsedTotal, mem_usage.total);
            else
                info.SetString(kMemoryUsedTotal, kNA);

            if (mem_usage.total_peak > 0)
                info.SetInteger(kMemoryUsedTotalPeak, mem_usage.total_peak);
            else
                info.SetString(kMemoryUsedTotalPeak, kNA);

            if (mem_usage.resident > 0)
                info.SetInteger(kMemoryUsedResident, mem_usage.resident);
            else
                info.SetString(kMemoryUsedResident, kNA);

            if (mem_usage.resident_peak > 0)
                info.SetInteger(kMemoryUsedResidentPeak, mem_usage.resident_peak);
            else
                info.SetString(kMemoryUsedResidentPeak, kNA);

            if (mem_usage.shared > 0)
                info.SetInteger(kMemoryUsedShared, mem_usage.shared);
            else
                info.SetString(kMemoryUsedShared, kNA);

            if (mem_usage.data > 0)
                info.SetInteger(kMemoryUsedData, mem_usage.data);
            else
                info.SetString(kMemoryUsedData, kNA);

            if (mem_usage.stack > 0)
                info.SetInteger(kMemoryUsedStack, mem_usage.stack);
            else
                info.SetString(kMemoryUsedStack, kNA);

            if (mem_usage.text > 0)
                info.SetInteger(kMemoryUsedText, mem_usage.text);
            else
                info.SetString(kMemoryUsedText, kNA);

            if (mem_usage.lib > 0)
                info.SetInteger(kMemoryUsedLib, mem_usage.lib);
            else
                info.SetString(kMemoryUsedLib, kNA);

            if (mem_usage.swap > 0)
                info.SetInteger(kMemoryUsedSwap, mem_usage.swap);
            else
                info.SetString(kMemoryUsedSwap, kNA);
        } else {
            info.SetString(kMemoryUsedTotal, kNA);
            info.SetString(kMemoryUsedTotalPeak, kNA);
            info.SetString(kMemoryUsedResident, kNA);
            info.SetString(kMemoryUsedResidentPeak, kNA);
            info.SetString(kMemoryUsedShared, kNA);
            info.SetString(kMemoryUsedData, kNA);
            info.SetString(kMemoryUsedStack, kNA);
            info.SetString(kMemoryUsedText, kNA);
            info.SetString(kMemoryUsedLib, kNA);
            info.SetString(kMemoryUsedSwap, kNA);
        }

        int         proc_fd_soft_limit;
        int         proc_fd_hard_limit;
        int         proc_fd_used =
                CCurrentProcess::GetFileDescriptorsCount(&proc_fd_soft_limit,
                                                         &proc_fd_hard_limit);

        if (proc_fd_soft_limit >= 0)
            info.SetInteger(kProcFDSoftLimit, proc_fd_soft_limit);
        else
            info.SetString(kProcFDSoftLimit, kNA);

        if (proc_fd_hard_limit >= 0)
            info.SetInteger(kProcFDHardLimit, proc_fd_hard_limit);
        else
            info.SetString(kProcFDHardLimit, kNA);

        if (proc_fd_used >= 0)
            info.SetInteger(kProcFDUsed, proc_fd_used);
        else
            info.SetString(kProcFDUsed, kNA);

        info.SetInteger(kCPUCount, CSystemInfo::GetCpuCount());

        int         proc_thread_count = CCurrentProcess::GetThreadCount();
        if (proc_thread_count >= 1)
            info.SetInteger(kProcThreadCount, proc_thread_count);
        else
            info.SetString(kProcThreadCount, kNA);


        info.SetString(kVersion, PUBSEQ_GATEWAY_VERSION);
        info.SetString(kBuildDate, PUBSEQ_GATEWAY_BUILD_DATE);
        info.SetString(kStartedAt, m_StartTime.AsString());

        info.SetInteger(kExcludeBlobCacheUserCount,
                        app->GetExcludeBlobCache()->Size());

        map<string, size_t>     concurrent_procs =
                                    m_RequestDispatcher->GetConcurrentCounters();
        for (auto item: concurrent_procs) {
            info.SetInteger(kConcurrentPrefix + item.first,
                            item.second);
        }

        string      content = info.Repr(CJsonNode::fStandardJson);

        reply->SetContentType(ePSGS_JsonMime);
        reply->SetContentLength(content.size());
        reply->SendOk(content.data(), content.size(), false);

        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e200_Ok,
                           reply->GetBytesSent());
        m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_InfoError,
                    "Exception when handling an info request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_InfoError,
                    "Unknown exception when handling an info request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnStatus(CHttpRequest &  req,
                                shared_ptr<CPSGS_Reply>  reply)
{
    // NOTE: expected to work regardless of the shutdown request

    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    try {
        CJsonNode                       status(CJsonNode::NewObjectNode());

        if (m_CassConnection) {
            m_Counters.AppendValueNode(
                status, CPSGSCounters::ePSGS_CassandraActiveStatements,
                static_cast<uint64_t>(m_CassConnection->GetActiveStatements()));
        } else {
            // No cassandra connection
            m_Counters.AppendValueNode(
                status, CPSGSCounters::ePSGS_CassandraActiveStatements,
                static_cast<uint64_t>(0));
        }
        m_Counters.AppendValueNode(
            status, CPSGSCounters::ePSGS_NumberOfConnections,
            static_cast<uint64_t>(m_TcpDaemon->NumOfConnections()));
        m_Counters.AppendValueNode(
            status, CPSGSCounters::ePSGS_SplitInfoCacheSize,
            static_cast<uint64_t>(m_SplitInfoCache->Size()));
        m_Counters.AppendValueNode(
            status, CPSGSCounters::ePSGS_ShutdownRequested,
            g_ShutdownData.m_ShutdownRequested);
        m_Counters.AppendValueNode(
            status, CPSGSCounters::ePSGS_ActiveProcessorGroups,
            static_cast<uint64_t>(GetProcessorDispatcher()->GetActiveProcessorGroups()));

        if (g_ShutdownData.m_ShutdownRequested) {
            auto        now = psg_clock_t::now();
            uint64_t    sec = chrono::duration_cast<chrono::seconds>
                                (g_ShutdownData.m_Expired - now).count();
            m_Counters.AppendValueNode(
                status, CPSGSCounters::ePSGS_GracefulShutdownExpiredInSec, sec);
        } else {
            m_Counters.AppendValueNode(
                status, CPSGSCounters::ePSGS_GracefulShutdownExpiredInSec, kNA);
        }

        m_Counters.PopulateDictionary(status);

        string      content = status.Repr(CJsonNode::fStandardJson);

        reply->SetContentType(ePSGS_JsonMime);
        reply->SetContentLength(content.size());
        reply->SendOk(content.data(), content.size(), false);

        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e200_Ok, reply->GetBytesSent());
        m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_StatusError,
                    "Exception when handling a status request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_StatusError,
                    "Unknown exception when handling a status request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnShutdown(CHttpRequest &  req,
                                  shared_ptr<CPSGS_Reply>  reply)
{
    // NOTE: expected to work regardless of the shutdown request

    static const char *     s_Shutdown = "Shutdown request accepted";
    static size_t           s_ShutdownSize = strlen(s_Shutdown);

    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    try {
        string              msg;
        string              username;
        SRequestParameter   username_param = x_GetParam(req, kUsernameParam);
        if (username_param.m_Found) {
            username = string(username_param.m_Value.data(),
                              username_param.m_Value.size());
        }

        if (!m_Settings.m_AuthToken.empty()) {
            SRequestParameter   auth_token_param = x_GetParam(req, kAuthTokenParam);

            bool    auth_good = false;
            if (auth_token_param.m_Found) {
                auth_good = m_Settings.m_AuthToken == string(auth_token_param.m_Value.data(),
                                                             auth_token_param.m_Value.size());
            }

            if (!auth_good) {
                msg = "Unauthorized shutdown request: invalid authorization token. ";
                if (username.empty())
                    msg += "Unknown user";
                else
                    msg += "User: " + username;
                PSG_MESSAGE(msg);

                x_SendMessageAndCompletionChunks(
                        reply, now, "Invalid authorization token",
                        CRequestStatus::e401_Unauthorized,
                        ePSGS_Unauthorised, eDiag_Error);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e401_Unauthorized,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
                return 0;
            }
        }

        int                 timeout = 10; // Default: 10 sec
        SRequestParameter   timeout_param = x_GetParam(req, kTimeoutParam);
        if (timeout_param.m_Found) {
            try {
                timeout = stoi(string(timeout_param.m_Value.data(),
                                      timeout_param.m_Value.size()));
            } catch (...) {
                msg = "Invalid shutdown request: cannot convert timeout to an integer. ";
                if (username.empty())
                    msg += "Unknown user";
                else
                    msg += "User: " + username;
                PSG_MESSAGE(msg);

                x_SendMessageAndCompletionChunks(
                        reply, now, "Invalid timeout (must be a positive integer)",
                        CRequestStatus::e400_BadRequest,
                        ePSGS_MalformedParameter, eDiag_Error);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e400_BadRequest,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
                return 0;
            }

            if (timeout < 0) {
                msg = "Invalid shutdown request: timeout must be >= 0. ";
                if (username.empty())
                    msg += "Unknown user";
                else
                    msg += "User: " + username;
                PSG_MESSAGE(msg);

                x_SendMessageAndCompletionChunks(
                        reply, now, "Invalid timeout (must be a positive integer)",
                        CRequestStatus::e400_BadRequest,
                        ePSGS_MalformedParameter, eDiag_Error);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e400_BadRequest,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
                return 0;
            }
        }

        reply->SetContentType(ePSGS_PlainTextMime);

        msg = "Shutdown request received from ";
        if (username.empty())
            msg += "an unknown user";
        else
            msg += "user " + username;

        auto        now = psg_clock_t::now();
        auto        expiration = now;
        if (timeout > 0)
           expiration += chrono::seconds(timeout);

        if (g_ShutdownData.m_ShutdownRequested) {
            // Consequent shutdown request
            if (expiration >= g_ShutdownData.m_Expired) {
                msg += ". The previous shutdown expiration is shorter "
                       "than this one. Ignored.";
                PSG_MESSAGE(msg);
                reply->Send409(msg.c_str());
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e409_Conflict,
                                   reply->GetBytesSent());
                return 0;
            }
        }

        // New shutdown request or a shorter expiration request
        PSG_MESSAGE(msg);

        reply->Send202(s_Shutdown, s_ShutdownSize);
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e202_Accepted,
                           reply->GetBytesSent());

        // The actual shutdown processing is in s_OnWatchDog() method
        g_ShutdownData.m_Expired = expiration;
        g_ShutdownData.m_ShutdownRequested = true;
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_ShutdownError,
                    "Exception when handling a shutdown request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_ShutdownError,
                    "Unknown exception when handling a shutdown request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnGetAlerts(CHttpRequest &  req,
                                   shared_ptr<CPSGS_Reply>  reply)
{
    // NOTE: expected to work regardless of the shutdown request

    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    try {
        string      content = m_Alerts.Serialize().Repr(CJsonNode::fStandardJson);

        reply->SetContentType(ePSGS_JsonMime);
        reply->SetContentLength(content.size());
        reply->SendOk(content.data(), content.size(), false);

        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e200_Ok, reply->GetBytesSent());
        m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_GetAlertsError,
                    "Exception when handling a get alerts request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_GetAlertsError,
                    "Unknown exception when handling a get alerts request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnAckAlert(CHttpRequest &  req,
                                  shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    string                  msg;

    try {
        SRequestParameter   alert_param = x_GetParam(req, kAlertParam);
        if (!alert_param.m_Found) {
            msg = "Missing " + kAlertParam + " parameter";
            x_SendMessageAndCompletionChunks(
                    reply, now, msg, CRequestStatus::e400_BadRequest,
                    ePSGS_InsufficientArguments, eDiag_Error);
            PSG_ERROR(msg);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        SRequestParameter   username_param = x_GetParam(req, kUsernameParam);
        if (!username_param.m_Found) {
            msg = "Missing " + kUsernameParam + " parameter";
            x_SendMessageAndCompletionChunks(
                    reply, now, msg, CRequestStatus::e400_BadRequest,
                    ePSGS_InsufficientArguments, eDiag_Error);
            PSG_ERROR(msg);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        string  alert(alert_param.m_Value.data(), alert_param.m_Value.size());
        string  username(username_param.m_Value.data(), username_param.m_Value.size());

        switch (m_Alerts.Acknowledge(alert, username)) {
            case ePSGS_AlertNotFound:
                msg = "Alert " + alert + " is not found";
                x_SendMessageAndCompletionChunks(
                        reply, now, msg,
                        CRequestStatus::e404_NotFound,
                        ePSGS_MalformedParameter, eDiag_Error);
                PSG_ERROR(msg);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e404_NotFound,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
                break;
            case ePSGS_AlertAlreadyAcknowledged:
                reply->SetContentType(ePSGS_PlainTextMime);
                msg = "Alert " + alert + " has already been acknowledged";
                reply->SendOk(msg.data(), msg.size(), false);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e200_Ok,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
                break;
            case ePSGS_AlertAcknowledged:
                reply->SetContentType(ePSGS_PlainTextMime);
                reply->SendOk(nullptr, 0, true);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e200_Ok,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
                break;
        }
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_AckAlertError,
                    "Exception when handling an acknowledge alert request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_AckAlertError,
                    "Unknown exception when handling an acknowledge alert request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}


int CPubseqGatewayApp::OnStatistics(CHttpRequest &  req,
                                    shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    try {
        // /ADMIN/statistics[?reset=yes(dflt=no)][&most_recent_time=<time>][&most_ancient_time=<time>][histogram_names=name1,name2,...]
        // /ADMIN/statistics[?reset=yes(dflt=no)][&most_recent_time=<time>][&most_ancient_time=<time>]


        bool                    reset = false;
        SRequestParameter       reset_param = x_GetParam(req, kResetParam);
        if (reset_param.m_Found) {
            string      err_msg;
            if (!x_IsBoolParamValid(kResetParam, reset_param.m_Value, err_msg)) {
                x_SendMessageAndCompletionChunks(
                        reply, now, err_msg, CRequestStatus::e400_BadRequest,
                        ePSGS_MalformedParameter, eDiag_Error);
                PSG_WARNING(err_msg);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e400_BadRequest,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
                return 0;
            }
            reset = reset_param.m_Value == "yes";
        }

        if (reset) {
            m_Timing->Reset();
            m_Counters.Reset();

            reply->SetContentType(ePSGS_PlainTextMime);
            reply->SendOk(nullptr, 0, true);
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e200_Ok,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
            return 0;
        }

        int                 most_recent_time = INT_MIN;
        SRequestParameter   most_recent_time_param = x_GetParam(req, kMostRecentTimeParam);
        if (most_recent_time_param.m_Found) {
            string          err_msg;
            try {
                most_recent_time = NStr::StringToInt8(most_recent_time_param.m_Value);
                if (most_recent_time < 0)
                    err_msg = "Invalid " + kMostRecentTimeParam +
                              " value (" + most_recent_time_param.m_Value +
                              "). It must be >= 0.";
            } catch (...) {
                err_msg = "Invalid " + kMostRecentTimeParam +
                          " value (" + most_recent_time_param.m_Value +
                          "). It must be an integer >= 0.";
            }

            if (!err_msg.empty()) {
                x_SendMessageAndCompletionChunks(
                        reply, now, err_msg, CRequestStatus::e400_BadRequest,
                        ePSGS_MalformedParameter, eDiag_Error);
                PSG_ERROR(err_msg);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e400_BadRequest,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
                return 0;
            }
        }

        int                 most_ancient_time = INT_MIN;
        SRequestParameter   most_ancient_time_param = x_GetParam(req, kMostAncientTimeParam);
        if (most_ancient_time_param.m_Found) {
            string          err_msg;
            try {
                most_ancient_time = NStr::StringToInt8(most_ancient_time_param.m_Value);
                if (most_ancient_time < 0)
                    err_msg = "Invalid " + kMostAncientTimeParam +
                              " value (" + most_ancient_time_param.m_Value +
                              "). It must be >= 0.";
            } catch (...) {
                err_msg = "Invalid " + kMostAncientTimeParam +
                          " value (" + most_ancient_time_param.m_Value +
                          "). It must be an integer >= 0.";
            }

            if (!err_msg.empty()) {
                x_SendMessageAndCompletionChunks(
                        reply, now, err_msg, CRequestStatus::e400_BadRequest,
                        ePSGS_MalformedParameter, eDiag_Error);
                PSG_ERROR(err_msg);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e400_BadRequest,
                                   reply->GetBytesSent());
                m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
                return 0;
            }
        }

        if (most_ancient_time >= 0 && most_recent_time >= 0) {
            // auto reorder the time if needed
            if (most_recent_time > most_ancient_time) {
                swap(most_recent_time, most_ancient_time);
            }
        }

        vector<CTempString> histogram_names;
        SRequestParameter   histogram_names_param = x_GetParam(req, kHistogramNamesParam);
        if (histogram_names_param.m_Found) {
            NStr::Split(histogram_names_param.m_Value, ",", histogram_names);
        }

        vector<pair<int, int>>      time_series;
        if (!x_GetTimeSeries(req, reply, now, time_series)) {
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_ResolveRequest,
                               CRequestStatus::e400_BadRequest,
                               reply->GetBytesSent());
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        CJsonNode   timing(m_Timing->Serialize(most_ancient_time,
                                               most_recent_time,
                                               histogram_names,
                                               time_series,
                                               m_Settings.m_TickSpan));
        string      content = timing.Repr(CJsonNode::fStandardJson);

        reply->SetContentType(ePSGS_JsonMime);
        reply->SetContentLength(content.size());
        reply->SendOk(content.data(), content.size(), false);

        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e200_Ok, reply->GetBytesSent());
        m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_StatisticsError,
                    "Exception when handling a statistics request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_StatisticsError,
                    "Unknown exception when handling a statistics request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}

int CPubseqGatewayApp::OnDispatcherStatus(CHttpRequest &  req,
                                          shared_ptr<CPSGS_Reply>  reply)
{
    // NOTE: expected to work regardless of the shutdown request

    auto                    now = psg_clock_t::now();
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context = x_CreateRequestContext(req);

    try {
        CJsonNode  dispatcher_status(CJsonNode::NewArrayNode());

        m_RequestDispatcher->PopulateStatus(dispatcher_status);
        string      content = dispatcher_status.Repr(CJsonNode::fStandardJson);

        reply->SetContentType(ePSGS_JsonMime);
        reply->SetContentLength(content.size());
        reply->SendOk(content.data(), content.size(), false);

        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e200_Ok,
                           reply->GetBytesSent());
        m_Counters.Increment(CPSGSCounters::ePSGS_AdminRequest);
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_StatusError,
                    "Exception when handling a dispatcher_status request: " +
                    string(exc.what()));
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_StatusError,
                    "Unknown exception when handling a dispatcher_status request");
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e500_InternalServerError,
                           reply->GetBytesSent());
    }
    return 0;
}



int CPubseqGatewayApp::OnTestIO(CHttpRequest &  req,
                                shared_ptr<CPSGS_Reply>  reply)
{
    auto                    now = psg_clock_t::now();
    bool                    need_log = false;   // default
    CRequestContextResetter context_resetter;
    CRef<CRequestContext>   context;

    if (x_IsShuttingDown(reply, now)) {
        x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                           CRequestStatus::e503_ServiceUnavailable,
                           reply->GetBytesSent());
        return 0;
    }

    try {
        string                  err_msg;

        SRequestParameter       log_param = x_GetParam(req, kLogParam);
        if (log_param.m_Found) {
            if (!x_IsBoolParamValid(kLogParam, log_param.m_Value, err_msg)) {
                x_SendMessageAndCompletionChunks(
                        reply, now, err_msg, CRequestStatus::e400_BadRequest,
                        ePSGS_MalformedParameter, eDiag_Error);
                PSG_WARNING(err_msg);
                m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
                return 0;
            }
            need_log = log_param.m_Value == "yes";
        }

        if (need_log)
            context = x_CreateRequestContext(req);

        // Read the return data size
        SRequestParameter   data_size_param = x_GetParam(req, kDataSizeParam);
        long                data_size = 0;
        if (data_size_param.m_Found) {
            data_size = NStr::StringToLong(data_size_param.m_Value);
            if (data_size < 0 || data_size > kMaxTestIOSize) {
                err_msg = "Invalid range of the '" + kDataSizeParam +
                          "' parameter. Accepted values are 0..." +
                          to_string(kMaxTestIOSize);
                x_SendMessageAndCompletionChunks(
                        reply, now, err_msg, CRequestStatus::e400_BadRequest,
                        ePSGS_MalformedParameter, eDiag_Error);
                if (need_log) {
                    PSG_WARNING(err_msg);
                    x_PrintRequestStop(context,
                                       CPSGS_Request::ePSGS_UnknownRequest,
                                       CRequestStatus::e400_BadRequest,
                                       reply->GetBytesSent());
                }
                m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
                return 0;
            }
        } else {
            err_msg = "The '" + kDataSizeParam + "' must be provided";
            x_SendMessageAndCompletionChunks(
                    reply, now, err_msg, CRequestStatus::e400_BadRequest,
                    ePSGS_InsufficientArguments, eDiag_Error);
            if (need_log) {
                PSG_WARNING(err_msg);
                x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                                   CRequestStatus::e400_BadRequest,
                                   reply->GetBytesSent());
            }
            m_Counters.Increment(CPSGSCounters::ePSGS_NonProtocolRequests);
            return 0;
        }

        reply->SetContentType(ePSGS_BinaryMime);
        reply->SetContentLength(data_size);

        // true: persistent
        reply->SendOk(m_IOTestBuffer.get(), data_size, true);

        if (need_log)
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e200_Ok,
                               reply->GetBytesSent());
        m_Counters.Increment(CPSGSCounters::ePSGS_TestIORequest);
    } catch (const exception &  exc) {
        x_Finish500(reply, now, ePSGS_TestIOError,
                    "Exception when handling a test io request: " +
                    string(exc.what()));
        if (need_log)
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e500_InternalServerError,
                               reply->GetBytesSent());
    } catch (...) {
        x_Finish500(reply, now, ePSGS_TestIOError,
                    "Unknown exception when handling a test io request");
        if (need_log)
            x_PrintRequestStop(context, CPSGS_Request::ePSGS_UnknownRequest,
                               CRequestStatus::e500_InternalServerError,
                               reply->GetBytesSent());
    }
    return 0;
}



bool CPubseqGatewayApp::x_IsShuttingDown(shared_ptr<CPSGS_Reply>  reply,
                                         const psg_time_point_t &  create_timestamp)
{
    if (g_ShutdownData.m_ShutdownRequested) {
        string      msg = "The server is in process of shutting down";
        x_SendMessageAndCompletionChunks(reply, create_timestamp, msg,
                                         CRequestStatus::e503_ServiceUnavailable,
                                         ePSGS_ShuttingDown,
                                         eDiag_Error);
        PSG_WARNING(msg);
        return true;
    }
    return false;
}


// true => some processors were instantiated
// false => no suitable processor
bool CPubseqGatewayApp::x_DispatchRequest(CRef<CRequestContext>   context,
                                          shared_ptr<CPSGS_Request>  request,
                                          shared_ptr<CPSGS_Reply>  reply)
{
    list<string>    processor_names =
            m_RequestDispatcher->PreliminaryDispatchRequest(request, reply);
    if (processor_names.empty())
        return false;

    if (context.NotNull()) {
        CDiagContext::SetRequestContext(context);
        GetDiagContext().Extra().Print("psg_request_id", request->GetRequestId());
    }

    reply->SetRequestId(request->GetRequestId());

    auto    http_conn = reply->GetHttpReply()->GetHttpConnection();
    http_conn->Postpone(request, reply, move(processor_names));
    return true;
}

