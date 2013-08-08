/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// AuthProxy - An authorization plugin for Apache Traffic Server that delegates
// the authorization decision to a separate web service. The web service
// (which we refer to here as the Authorization Proxy) is expected to authorize
// the request (or not) by consulting some authoritative source.
//
// This plugin follows the pattern of the basic-auth sample code. We use the
// TS_HTTP_OS_DNS_HOOK to perform the initial authorization, and the
// TS_HTTP_SEND_RESPONSE_HDR_HOOK to send an error response if necessary.

#include "utils.h"
#include <string>
#include <memory> // placement new
#include <limits>
#include <cstring>
#include <cstdlib>
#include <cstdio>

#include <getopt.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <ts/remap.h>
#include <ink_config.h>

using std::strlen;

struct AuthRequestContext;

typedef bool (*AuthRequestTransform)(AuthRequestContext * auth, const sockaddr * saddr);

// We can operate in global plugin mode or remap plugin mode. If we are in
// global mode, then we will authorize every request. In remap mode, we will
// only authorize tagged requests.
static bool         AuthTaggedRequestOnly = false;
static int          AuthTaggedRequestArg = -1;

static TSCont       AuthOsDnsContinuation;

struct AuthOptions
{
    char *  hostname;
    int     hostport;
    bool    force;
    AuthRequestTransform transform;

    AuthOptions() : hostname(NULL), hostport(8080), force(false), transform(NULL) {
    }

    ~AuthOptions() {
        TSfree(hostname);
    }
};

// Global options; used when we are in global authorization mode.
static AuthOptions * AuthGlobalOptions;

// Generic state handler callback. This should handle the event, and return a
// new event. The return value controls the subsequent state transition:
//      TS_EVENT_CONTINUE   Continue the state machine, returning to the ATS event loop
//      TS_EVENT_NONE       Stop processing (because a nested dispatch occurred)
//      Anything else       Continue the state machine with this event
typedef TSEvent (*StateHandler)(struct AuthRequestContext *, void * edata);

struct StateTransition {
    TSEvent                 event;
    StateHandler            handler;
    const StateTransition * next;
};

static TSEvent StateAuthProxyConnect(AuthRequestContext *, void *);
static TSEvent StateAuthProxyResolve(AuthRequestContext *, void *);
static TSEvent StateAuthProxyWriteComplete(AuthRequestContext *, void *);
static TSEvent StateUnauthorized(AuthRequestContext *, void *);
static TSEvent StateAuthorized(AuthRequestContext *, void *);

static TSEvent StateAuthProxyReadHeaders(AuthRequestContext *, void *);
static TSEvent StateAuthProxyCompleteHeaders(AuthRequestContext *, void *);
static TSEvent StateAuthProxyReadContent(AuthRequestContext *, void *);
static TSEvent StateAuthProxyCompleteContent(AuthRequestContext *, void *);

static TSEvent StateAuthProxySendResponse(AuthRequestContext *, void *);

// Trampoline state that just returns TS_EVENT_CONTINUE. We need this to be
// able to transition between state tables when we are in a loop.
static TSEvent StateContinue(AuthRequestContext *, void *)
{
    return TS_EVENT_CONTINUE;
}

// State table for sending the auth proxy response to the client.
static const StateTransition StateTableSendResponse[] =
{
    { TS_EVENT_HTTP_SEND_RESPONSE_HDR, StateAuthProxySendResponse, NULL },
    { TS_EVENT_NONE, NULL, NULL }
};

// State table for reading the proxy response body content.
static const StateTransition StateTableProxyReadContent[] =
{
    { TS_EVENT_VCONN_READ_READY, StateAuthProxyReadContent, StateTableProxyReadContent },
    { TS_EVENT_VCONN_READ_COMPLETE, StateAuthProxyReadContent, StateTableProxyReadContent },
    { TS_EVENT_VCONN_EOS, StateAuthProxyCompleteContent, StateTableProxyReadContent  },
    { TS_EVENT_HTTP_SEND_RESPONSE_HDR, StateContinue, StateTableSendResponse },
    { TS_EVENT_ERROR, StateUnauthorized, NULL },
    { TS_EVENT_IMMEDIATE, StateAuthorized, NULL },
    { TS_EVENT_NONE, NULL, NULL }
};

// State table for reading the auth proxy response header.
static const StateTransition StateTableProxyReadHeader[] =
{
    { TS_EVENT_VCONN_READ_READY, StateAuthProxyReadHeaders, StateTableProxyReadHeader },
    { TS_EVENT_VCONN_READ_COMPLETE, StateAuthProxyReadHeaders, StateTableProxyReadHeader },
    { TS_EVENT_HTTP_READ_REQUEST_HDR, StateAuthProxyCompleteHeaders, StateTableProxyReadHeader },
    { TS_EVENT_HTTP_SEND_RESPONSE_HDR, StateContinue, StateTableSendResponse },
    { TS_EVENT_HTTP_CONTINUE, StateAuthProxyReadContent, StateTableProxyReadContent },
    { TS_EVENT_VCONN_EOS, StateUnauthorized, NULL }, // XXX Should we check headers on EOS?
    { TS_EVENT_ERROR, StateUnauthorized, NULL },
    { TS_EVENT_IMMEDIATE, StateAuthorized, NULL },
    { TS_EVENT_NONE, NULL, NULL }
};

// State table for sending the request to the auth proxy.
static const StateTransition StateTableProxyRequest[] =
{
    { TS_EVENT_HOST_LOOKUP, StateAuthProxyConnect, StateTableProxyRequest },
    { TS_EVENT_VCONN_WRITE_COMPLETE, StateAuthProxyWriteComplete, StateTableProxyReadHeader },
    { TS_EVENT_ERROR, StateUnauthorized, NULL },
    { TS_EVENT_NONE, NULL, NULL }
};

// Initial state table.
static const StateTransition StateTableInit[] =
{
    { TS_EVENT_HTTP_OS_DNS, StateAuthProxyResolve, StateTableProxyRequest },
    { TS_EVENT_ERROR, StateUnauthorized, NULL },
    { TS_EVENT_NONE, NULL, NULL }
};

struct AuthRequestContext
{
    TSHttpTxn       txn;    // Original client transaction we are authorizing.
    TSCont          cont;   // Continuation for this state machine.
    TSVConn         vconn;  // Virtual connection to the auth proxy.
    TSHttpParser    hparser;// HTTP response header parser.
    HttpHeader      rheader;// HTTP response header.
    HttpIoBuffer    iobuf;
    bool            is_head;// This is a HEAD request
    bool            read_body;

    const StateTransition * state;

    AuthRequestContext()
            : txn(NULL), cont(NULL), vconn(NULL), hparser(TSHttpParserCreate()),
                rheader(), iobuf(TS_IOBUFFER_SIZE_INDEX_4K), is_head(false), read_body(true), state(NULL) {
        this->cont = TSContCreate(dispatch, TSMutexCreate());
        TSContDataSet(this->cont, this);
    }

    ~AuthRequestContext() {
        TSContDataSet(this->cont, NULL);
        TSContDestroy(this->cont);
        TSHttpParserDestroy(this->hparser);
        if (this->vconn) {
            TSVConnClose(this->vconn);
        }
    }

    const AuthOptions * options() const {
        AuthOptions * opt;

        opt = (AuthOptions *)TSHttpTxnArgGet(this->txn, AuthTaggedRequestArg);
        return opt ? opt : AuthGlobalOptions;
    }

    static AuthRequestContext * allocate();
    static void destroy(AuthRequestContext *);
    static int dispatch(TSCont, TSEvent, void *);
};

AuthRequestContext *
AuthRequestContext::allocate()
{
    void * ptr = TSmalloc(sizeof(AuthRequestContext));
    return new(ptr) AuthRequestContext();
}

void
AuthRequestContext::destroy(AuthRequestContext * auth)
{
    if (auth) {
        auth->~AuthRequestContext();
        TSfree(auth);
    }
}

int
AuthRequestContext::dispatch(TSCont cont, TSEvent event, void * edata)
{
    AuthRequestContext * auth = (AuthRequestContext *)TSContDataGet(cont);
    const StateTransition * s;

pump:
    for (s = auth->state; s && s->event; ++s) {
        if (s->event == event) {
            break;
        }
    }

    // If we don't have a handler, the state machine is borked.
    TSReleaseAssert(s->handler != NULL);

    // Move to the next state. We have to set this *before* invoking the
    // handler because the handler itself can invoke the next handler.
    auth->state = s->next;
    event = s->handler(auth, edata);

    // If the handler returns TS_EVENT_NONE, it means that a re-entrant event
    // was dispatched. In this case, the state machine continues from the
    // nested call to dispatch.
    if (event == TS_EVENT_NONE) {
        return TS_EVENT_NONE;
    }

    // If there are no more states, the state machine has terminated.
    if (auth->state == NULL) {
        AuthRequestContext::destroy(auth);
    }

    // If the handler gave us an event, pump the it back into the current state
    // table, otherwise we will return back to the ATS event loop.
    if (event != TS_EVENT_CONTINUE) {
        goto pump;
    }

    return TS_EVENT_NONE;
}

// Return whether the client request was a HEAD request.
static bool
AuthRequestIsHead(TSHttpTxn txn)
{
    TSMBuffer   mbuf;
    TSMLoc      mhdr;
    int         len;
    bool        is_head;

    TSReleaseAssert(
        TSHttpTxnClientReqGet(txn, &mbuf, &mhdr) == TS_SUCCESS
    );

    is_head = (TSHttpHdrMethodGet(mbuf, mhdr, &len) == TS_HTTP_METHOD_HEAD);

    TSHandleMLocRelease(mbuf, TS_NULL_MLOC, mhdr);
    return is_head;
}

// Chain the response header hook to send the proxy's authorization response.
static void
AuthChainAuthorizationResponse(AuthRequestContext * auth)
{
    if (auth->vconn) {
        TSVConnClose(auth->vconn);
        auth->vconn = NULL;
    }

    TSHttpTxnHookAdd(auth->txn, TS_HTTP_SEND_RESPONSE_HDR_HOOK, auth->cont);
    TSHttpTxnReenable(auth->txn, TS_EVENT_HTTP_ERROR);
}

// Transform the client request into a HEAD request and write it out.
static bool
AuthWriteHeadRequest(AuthRequestContext * auth, const sockaddr * /* saddr ATS_UNUSED */)
{
    HttpHeader  rq;
    TSMBuffer   mbuf;
    TSMLoc      mhdr;

    TSReleaseAssert(
        TSHttpTxnClientReqGet(auth->txn, &mbuf, &mhdr) == TS_SUCCESS
    );

    // First, copy the whole client request to our new auth proxy request.
    TSReleaseAssert(
        TSHttpHdrCopy(rq.buffer, rq.header, mbuf, mhdr) == TS_SUCCESS
    );

    // Next, we need to rewrite the client request URL to be a HEAD request.
    TSReleaseAssert(
        TSHttpHdrMethodSet(rq.buffer, rq.header, TS_HTTP_METHOD_HEAD, -1) == TS_SUCCESS
    );

    HttpSetMimeHeader(rq.buffer, rq.header, TS_MIME_FIELD_CONTENT_LENGTH, 0u);
    HttpSetMimeHeader(rq.buffer, rq.header, TS_MIME_FIELD_CACHE_CONTROL, "no-cache");

    HttpDebugHeader(rq.buffer, rq.header);

    // Serialize the HTTP request to the write IO buffer.
    TSHttpHdrPrint(rq.buffer, rq.header, auth->iobuf.buffer);

    // We have to tell the auth context not to try to ready the response
    // body (since HEAD can have a content-length but must not have any
    // content).
    auth->read_body = false;

    TSHandleMLocRelease(mbuf, TS_NULL_MLOC, mhdr);
    return true;
}

// Transform the client request into a form that the auth proxy can consume and
// write it out.
static bool
AuthWriteRedirectedRequest(AuthRequestContext * auth, const sockaddr * saddr)
{
    HttpHeader  rq;
    TSMBuffer   mbuf;
    TSMLoc      mhdr;
    TSMLoc      murl;

    char addrbuf[INET6_ADDRSTRLEN];
    char hostbuf[INET6_ADDRSTRLEN + sizeof("[]") + sizeof(":65536")];
    uint16_t hostport;

    TSReleaseAssert(
        TSHttpTxnClientReqGet(auth->txn, &mbuf, &mhdr) == TS_SUCCESS
    );

    // First, copy the whole client request to our new auth proxy request.
    TSReleaseAssert(
        TSHttpHdrCopy(rq.buffer, rq.header, mbuf, mhdr) == TS_SUCCESS
    );

    hostport = SockaddrGetPort(saddr);
    inet_ntop(saddr->sa_family, SockaddrGetAddress(saddr), addrbuf, sizeof(addrbuf));
    if (saddr->sa_family == PF_INET6) {
        snprintf(hostbuf, sizeof(hostbuf), "[%s]:%d", addrbuf, hostport);
    } else {
        snprintf(hostbuf, sizeof(hostbuf), "%s:%d", addrbuf, hostport);
    }

    // Next, we need to rewrite the client request URL so that the request goes to
    // the auth proxy instead of the original request.
    TSReleaseAssert(
        TSHttpHdrUrlGet(rq.buffer, rq.header, &murl) == TS_SUCCESS
    );

    // XXX Possibly we should rewrite the URL to remove the host, port and
    // scheme, forcing ATS to go to the Host header. I wonder how HTTPS would
    // work in that case. At any rate, we should add a new header containing
    // the original host so that the auth proxy can examine it.
    TSUrlHostSet(rq.buffer, murl, addrbuf, -1);
    TSUrlPortSet(rq.buffer, murl, hostport);
    TSHandleMLocRelease(rq.buffer,rq.header, murl);

    HttpSetMimeHeader(rq.buffer, rq.header, TS_MIME_FIELD_HOST, hostbuf);
    HttpSetMimeHeader(rq.buffer, rq.header, TS_MIME_FIELD_CONTENT_LENGTH, 0u);
    HttpSetMimeHeader(rq.buffer, rq.header, TS_MIME_FIELD_CACHE_CONTROL, "no-cache");

    HttpDebugHeader(rq.buffer, rq.header);

    // Serialize the HTTP request to the write IO buffer.
    TSHttpHdrPrint(rq.buffer, rq.header, auth->iobuf.buffer);

    TSHandleMLocRelease(mbuf, TS_NULL_MLOC, mhdr);
    TSHandleMLocRelease(rq.buffer, rq.header, murl);
    return true;
}

static TSEvent
StateAuthProxyResolve(AuthRequestContext * auth, void *)
{
    TSAction            lookup;
    const AuthOptions * options = auth->options();

    // If we are authorizing with a HEAD request we want to send that to the
    // origin; other requests we want to send to the authorization proxy.
    if (options->transform == AuthWriteHeadRequest) {
        char        hostname[TS_MAX_HOST_NAME_LEN * 2];
        TSMBuffer   mbuf;
        TSMLoc      mhdr;

        TSReleaseAssert(
            TSHttpTxnClientReqGet(auth->txn, &mbuf, &mhdr) == TS_SUCCESS
        );

        if (HttpGetOriginHost(mbuf, mhdr, hostname, sizeof(hostname))) {
            TSLogDebug("resolving authorization host %s", hostname);
            lookup = TSHostLookup(auth->cont, hostname, strlen(hostname));
            TSHandleMLocRelease(mbuf, TS_NULL_MLOC, mhdr);
        } else {
            TSLogError("failed to extract origin host name from client request");
            TSHandleMLocRelease(mbuf, TS_NULL_MLOC, mhdr);
            return TS_EVENT_ERROR;
        }

    } else {
        TSLogDebug("resolving authorization proxy host %s", options->hostname);
        lookup = TSHostLookup(auth->cont, options->hostname, strlen(options->hostname));
    }

    if (TSActionDone(lookup)) {
        TSLogDebug("host lookup was executed in line");
        return TS_EVENT_NONE;
    }

    return TS_EVENT_CONTINUE;
}

static TSEvent
StateAuthProxyConnect(AuthRequestContext * auth, void * edata)
{
    const AuthOptions * options = auth->options();
    TSHostLookupResult  dns;
    const sockaddr *    saddr;

    union {
        sockaddr            sa;
        sockaddr_in         sin;
        sockaddr_in6        sin6;
        sockaddr_storage    storage;
    } addr;

    dns = (TSHostLookupResult)edata;
    if (dns == NULL) {
        TSLogError("failed to resolve authorization proxy at %s", options->hostname);
        return TS_EVENT_ERROR;
    }

    // Copy the resolved address and add the port.
    saddr = TSHostLookupResultAddrGet(dns);
    switch (saddr->sa_family) {
    case PF_INET:
        memcpy(&addr.sin, saddr, sizeof(sockaddr_in));
        addr.sin.sin_port = options->hostport;
        break;
    case PF_INET6:
        memcpy(&addr.sin6, saddr, sizeof(sockaddr_in6));
        addr.sin6.sin6_port = options->hostport;
        break;
    }

    auth->is_head = AuthRequestIsHead(auth->txn);
    TSLogDebug("client request %s a HEAD request", auth->is_head ? "is" : "is not");

    auth->vconn = TSHttpConnect(&addr.sa);
    if (auth->vconn == NULL) {
        return TS_EVENT_ERROR;
    }

    // Transform the client request into an auth proxy request and write it
    // out to the auth proxy vconn.
    if (!options->transform(auth, &addr.sa)) {
        return TS_EVENT_ERROR;
    }

    // Start a write and transition to WriteAuthProxyState.
    TSVConnWrite(auth->vconn, auth->cont, auth->iobuf.reader, TSIOBufferReaderAvail(auth->iobuf.reader));
    return TS_EVENT_CONTINUE;
}

static TSEvent
StateAuthProxyCompleteHeaders(AuthRequestContext * auth, void * /* edata ATS_UNUSED */)
{
    TSHttpStatus status;
    unsigned nbytes;

    HttpDebugHeader(auth->rheader.buffer, auth->rheader.header);

    status = TSHttpHdrStatusGet(auth->rheader.buffer, auth->rheader.header);
    TSLogDebug("authorization proxy returned status %d", (int)status);

    // Authorize the original request on a 2xx response.
    if (status >= 200 && status < 300) {
        return TS_EVENT_IMMEDIATE;
    }

    if (auth->read_body) {
        // We can't support sending the auth proxy response back to the client
        // without writing a transform. Since that's more trouble than I want to
        // deal with right now, let's just fail fast ...
        if (HttpIsChunkedEncoding(auth->rheader.buffer, auth->rheader.header)) {
            TSLogDebug("ignoring chunked authorization proxy response");
        } else {
            // OK, we have a non-chunked response. If there's any content, let's go and
            // buffer it so that we can send it on to the client.
            nbytes = HttpGetContentLength(auth->rheader.buffer, auth->rheader.header);
            if (nbytes > 0) {
                TSLogDebug("content length is %u", nbytes);
                return TS_EVENT_HTTP_CONTINUE;
            }
        }
    }

    // We are going to reply with the auth proxy's response. The response body
    // is empty in this case.
    AuthChainAuthorizationResponse(auth);
    return TS_EVENT_HTTP_SEND_RESPONSE_HDR;
}

static TSEvent
StateAuthProxySendResponse(AuthRequestContext * auth, void * /* edata ATS_UNUSED */)
{
    TSMBuffer mbuf;
    TSMLoc mhdr;
    TSHttpStatus status;
    char msg[128];

    // The auth proxy denied this request. We need to copy the auth proxy
    // response header to the client response header, then read any available
    // body data and copy that as well.

    // There's only a client response if the auth proxy sent one. There
    TSReleaseAssert(
        TSHttpTxnClientRespGet(auth->txn, &mbuf, &mhdr) == TS_SUCCESS
    );

    TSReleaseAssert(
        TSHttpHdrCopy(mbuf, mhdr, auth->rheader.buffer, auth->rheader.header) == TS_SUCCESS
    );

    status = TSHttpHdrStatusGet(mbuf, mhdr),
    snprintf(msg, sizeof(msg), "%d %s\n", status, TSHttpHdrReasonLookup(status));

    TSHttpTxnErrorBodySet(auth->txn, TSstrdup(msg), strlen(msg), NULL);

    // We must not whack the content length for HEAD responses, since the
    // client already knows that there is no body. Forcing content length to
    // zero breaks hdiutil(1) on Mac OS X.
    if (!auth->is_head) {
        HttpSetMimeHeader(mbuf, mhdr, TS_MIME_FIELD_CONTENT_LENGTH, 0u);
    }

    TSLogDebug("sending auth proxy response for status %d", status);

    TSHttpTxnReenable(auth->txn, TS_EVENT_HTTP_CONTINUE);
    TSHandleMLocRelease(mbuf, TS_NULL_MLOC, mhdr);
    return TS_EVENT_CONTINUE;
}

static TSEvent
StateAuthProxyReadHeaders(AuthRequestContext * auth, void * /* edata ATS_UNUSED */)
{
    TSIOBufferBlock blk;
    ssize_t         consumed = 0;
    bool            complete = false;

    TSLogDebug("reading header data, %u bytes available", (unsigned)TSIOBufferReaderAvail(auth->iobuf.reader));

    for (blk = TSIOBufferReaderStart(auth->iobuf.reader); blk; blk = TSIOBufferBlockNext(blk)) {
        const char *    ptr;
        const char *    end;
        int64_t         nbytes;
        TSParseResult   result;

        ptr = TSIOBufferBlockReadStart(blk, auth->iobuf.reader, &nbytes);
        if (ptr == NULL || nbytes == 0) {
            continue;
        }

        end = ptr + nbytes;
        result = TSHttpHdrParseResp(auth->hparser, auth->rheader.buffer, auth->rheader.header, &ptr, end);
        switch (result) {
        case TS_PARSE_ERROR:
            return TS_EVENT_ERROR;
        case TS_PARSE_DONE:
        case TS_PARSE_OK:
            // We consumed the buffer we got minus the remainder.
            consumed += (nbytes - std::distance(ptr, end));
            complete = true;
            break;
        case TS_PARSE_CONT:
            consumed += (nbytes - std::distance(ptr, end));
            break;
        }

        if (complete) {
            break;
        }
    }

    TSLogDebug("consuming %u bytes, %u remain",
            (unsigned)consumed, (unsigned)TSIOBufferReaderAvail(auth->iobuf.reader));
    TSIOBufferReaderConsume(auth->iobuf.reader, consumed);

    // If the headers are complete, send a completion event.
    return complete ? TS_EVENT_HTTP_READ_REQUEST_HDR : TS_EVENT_CONTINUE;
}

static TSEvent
StateAuthProxyWriteComplete(AuthRequestContext * auth, void * /* edata ATS_UNUSED */)
{
    // We finished writing the auth proxy request. Kick off a read to get the response.
    auth->iobuf.reset();

    TSVConnRead(auth->vconn, auth->cont, auth->iobuf.buffer, std::numeric_limits<int64_t>::max());

    // XXX Do we need to keep the read and write VIOs and close them?

    return TS_EVENT_CONTINUE;
}

static TSEvent
StateAuthProxyReadContent(AuthRequestContext * auth, void * /* edata ATS_UNUSED */)
{
    unsigned        needed;
    int64_t         avail = 0;

    avail = TSIOBufferReaderAvail(auth->iobuf.reader);
    needed = HttpGetContentLength(auth->rheader.buffer, auth->rheader.header);

    TSLogDebug("we have %u of %u needed bytes", (unsigned)avail, needed);

    if (avail >= needed) {
        // OK, we have what we need. Let's respond to the client request.
        AuthChainAuthorizationResponse(auth);
        return TS_EVENT_HTTP_SEND_RESPONSE_HDR;
    }

    return TS_EVENT_CONTINUE;
}

static TSEvent
StateAuthProxyCompleteContent(AuthRequestContext * auth, void * /* edata ATS_UNUSED */)
{
    unsigned        needed;
    int64_t         avail;

    avail = TSIOBufferReaderAvail(auth->iobuf.reader);
    needed = HttpGetContentLength(auth->rheader.buffer, auth->rheader.header);

    TSLogDebug("we have %u of %u needed bytes", (unsigned)avail, needed);

    if (avail >= needed) {
        // OK, we have what we need. Let's respond to the client request.
        AuthChainAuthorizationResponse(auth);
        return TS_EVENT_HTTP_SEND_RESPONSE_HDR;
    }

    // We got EOS before reading all the content we expected.
    return TS_EVENT_ERROR;
}

// Terminal state. Force a 403 Forbidden response.
static TSEvent
StateUnauthorized(AuthRequestContext * auth, void *)
{
    static const char msg[] = "authorization denied\n";

    TSHttpTxnSetHttpRetStatus(auth->txn, TS_HTTP_STATUS_FORBIDDEN);
    TSHttpTxnErrorBodySet(auth->txn, TSstrdup(msg), sizeof(msg) - 1, NULL);

    TSHttpTxnReenable(auth->txn, TS_EVENT_HTTP_ERROR);
    return TS_EVENT_CONTINUE;
}

// Terminal state. Allow the original request to proceed.
static TSEvent
StateAuthorized(AuthRequestContext * auth, void *)
{
    const AuthOptions * options = auth->options();

    TSLogDebug("request authorized");

    // Since the original request might have authentication headers, we may
    // need to force ATS to ignore those in order to make it cacheable.
    if (options->force) {
        TSHttpTxnConfigIntSet(auth->txn, TS_CONFIG_HTTP_CACHE_IGNORE_AUTHENTICATION, 1);
    }

    TSHttpTxnReenable(auth->txn, TS_EVENT_HTTP_CONTINUE);
    return TS_EVENT_CONTINUE;
}

// Return true if the given request was tagged by a remap rule as needing
// authorization.
static bool
AuthRequestIsTagged(TSHttpTxn txn)
{
    return AuthTaggedRequestArg != -1 &&
        TSHttpTxnArgGet(txn, AuthTaggedRequestArg) != NULL;
}

static int
AuthProxyGlobalHook(TSCont /* cont ATS_UNUSED */, TSEvent event, void * edata)
{
    AuthRequestContext * auth;
    union {
        TSHostLookupResult  dns;
        TSHttpTxn           txn;
        void *              edata;
    } ptr;

    ptr.edata = edata;
    TSLogDebug("handling event=%d edata=%p", (int)event, edata);

    switch (event) {
    case TS_EVENT_HTTP_OS_DNS:
        // Ignore internal requests since we generated them.
        if (TSHttpIsInternalRequest(ptr.txn) == TS_SUCCESS) {
            // All our internal requests *must* hit the origin since it is the
            // agent that needs to make the authorization decision. We can't
            // allow that to be cached.
            TSHttpTxnReqCacheableSet(ptr.txn, 0);

            TSLogDebug("re-enabling internal transaction");
            TSHttpTxnReenable(ptr.txn, TS_EVENT_HTTP_CONTINUE);
            return TS_EVENT_NONE;
        }

        // Hook this request if we are in global authorization mode or if a
        // remap rule tagged it.
        if (AuthGlobalOptions != NULL || AuthRequestIsTagged(ptr.txn)) {
            auth = AuthRequestContext::allocate();
            auth->state = StateTableInit;
            auth->txn = ptr.txn;
            return AuthRequestContext::dispatch(auth->cont, event, edata);
        }

        // fallthru

    default:
        return TS_EVENT_NONE;
    }

}

static AuthOptions *
AuthParseOptions(int argc, const char ** argv)
{
  // The const_cast<> here is magic to work around a flaw in the definition of struct option
  // on some platforms (e.g. Solaris / Illumos). On sane platforms (e.g. linux), it'll get
  // automatically casted back to the const char*, as the struct is defined in <getopt.h>.
    static const struct option longopt[] =
    {
        { const_cast<char *>("auth-host"), required_argument, 0, 'h' },
        { const_cast<char *>("auth-port"), required_argument, 0, 'p' },
        { const_cast<char *>("auth-transform"), required_argument, 0, 't' },
        { const_cast<char *>("force-cacheability"), no_argument, 0, 'c' },
        {0, 0, 0, 0 }
    };

    AuthOptions * options = AuthNew<AuthOptions>();

    options->transform = AuthWriteRedirectedRequest;
    options->hostname = TSstrdup("127.0.0.1");

    // We might parse arguments multiple times if we are loaded as a global
    // plugin and a remap plugin. Reset optind so that getopt_long() does the
    // right thing (ie. work instead of crash).
    optind = 0;

    for (;;) {
        int opt;

        opt = getopt_long(argc, (char * const *)argv, "", longopt, NULL);
        switch (opt) {
        case 'h':
            TSfree(options->hostname);
            options->hostname = TSstrdup(optarg);
            break;
        case 'p':
            options->hostport = std::atoi(optarg);
            break;
        case 'c':
            options->force = true;
            break;
        case 't':
            if (strcasecmp(optarg, "redirect") == 0) {
                options->transform = AuthWriteRedirectedRequest;
            } else if (strcasecmp(optarg, "head") == 0) {
                options->transform = AuthWriteHeadRequest;
            } else {
                TSLogError("invalid authorization transform '%s'", optarg);
                // XXX make this a fatal error?
            }

            break;
        }

        if (opt == -1) {
            break;
        }
    }

    return options;
}
#undef LONGOPT_OPTION_CAST

void
TSPluginInit(int argc, const char *argv[])
{
    TSPluginRegistrationInfo info;

    info.plugin_name = (char *)"AuthProxy";
    info.vendor_name = (char *)"James Peach";
    info.support_email = (char *)"jamespeach@me.com";

    if (TSPluginRegister(TS_SDK_VERSION_3_0, &info) != TS_SUCCESS) {
        TSLogError("plugin registration failed");
    }

    TSReleaseAssert(
        TSHttpArgIndexReserve("AuthProxy", "AuthProxy authorization tag", &AuthTaggedRequestArg) == TS_SUCCESS
    );

    // We are in global mode. Authorize all requests.
    AuthTaggedRequestOnly = false;

    AuthOsDnsContinuation = TSContCreate(AuthProxyGlobalHook, NULL);
    AuthGlobalOptions = AuthParseOptions(argc, argv);
    TSLogDebug("using authorization proxy at %s:%d", AuthGlobalOptions->hostname, AuthGlobalOptions->hostport);

    // Catch the DNS hook. This triggers after reading the headers and
    // resolving the requested host, but before performing any cache lookups.
    TSHttpHookAdd(TS_HTTP_OS_DNS_HOOK, AuthOsDnsContinuation);
}

TSReturnCode
TSRemapInit(TSRemapInterface * api, char * err, int errsz)
{
    (void)api;
    (void)err;
    (void)errsz;

    TSReleaseAssert(
        TSHttpArgIndexReserve("AuthProxy", "AuthProxy authorization tag", &AuthTaggedRequestArg) == TS_SUCCESS
    );

    // We are in remap mode. Only authorize tagged requests.
    AuthTaggedRequestOnly = true;
    AuthOsDnsContinuation = TSContCreate(AuthProxyGlobalHook, NULL);
    return TS_SUCCESS;
}

TSReturnCode
TSRemapNewInstance(int argc, char * argv[], void ** instance, char * /* err ATS_UNUSED */, int /* errsz ATS_UNUSED */)
{
    AuthOptions * options;

    TSLogDebug("using authorization proxy for remapping %s -> %s", argv[0], argv[1]);

    // The first two arguments are the "from" and "to" URL string. We need to
    // skip them, but we also require that there be an option to masquerade as
    // argv[0], so we increment the argument indexes by 1 rather than by 2.
    argc--;
    argv++;
    options = AuthParseOptions(argc, (const char **)argv);

    *instance = options;
    return TS_SUCCESS;
}

void
TSRemapDeleteInstance(void * instance)
{
    AuthOptions * options = (AuthOptions *)instance;
    AuthDelete(options);
}

TSRemapStatus
TSRemapDoRemap(void * instance, TSHttpTxn txn, TSRemapRequestInfo * /* rri ATS_UNUSED */)
{
    AuthOptions * options = (AuthOptions *)instance;

    TSHttpTxnArgSet(txn, AuthTaggedRequestArg, options);
    TSHttpTxnHookAdd(txn, TS_HTTP_OS_DNS_HOOK, AuthOsDnsContinuation);
    return TSREMAP_NO_REMAP;
}

// vim: set ts=4 sw=4 et :
