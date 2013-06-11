/*
    pipeline.c -- HTTP pipeline processing. 
    Copyright (c) All Rights Reserved. See details at the end of the file.
 */

/********************************* Includes ***********************************/

#include    "http.h"

/********************************** Forward ***********************************/

static bool matchFilter(HttpConn *conn, HttpStage *filter, HttpRoute *route, int dir);
static void openQueues(HttpConn *conn);
static void pairQueues(HttpConn *conn);
static void httpStartHandler(HttpConn *conn);

/*********************************** Code *************************************/
/*
    Called after routing the request (httpRouteRequest)
 */
PUBLIC void httpCreatePipeline(HttpConn *conn)
{
    HttpRx      *rx;
    
    rx = conn->rx;
    assert(conn->endpoint);

    if (conn->endpoint) {
        assert(rx->route);
        httpCreateRxPipeline(conn, rx->route);
        httpCreateTxPipeline(conn, rx->route);
    }
}


PUBLIC void httpCreateTxPipeline(HttpConn *conn, HttpRoute *route)
{
    Http        *http;
    HttpTx      *tx;
    HttpRx      *rx;
    HttpQueue   *q;
    HttpStage   *stage, *filter;
    int         next, hasOutputFilters;

    assert(conn);
    assert(route);

    http = conn->http;
    rx = conn->rx;
    tx = conn->tx;

    tx->outputPipeline = mprCreateList(-1, 0);
    if (conn->endpoint) {
        if (tx->handler == 0 || tx->finalized) {
            tx->handler = http->passHandler;
        }
        mprAddItem(tx->outputPipeline, tx->handler);
    }
    hasOutputFilters = 0;
    if (route->outputStages) {
        for (next = 0; (filter = mprGetNextItem(route->outputStages, &next)) != 0; ) {
            if (matchFilter(conn, filter, route, HTTP_STAGE_TX) == HTTP_ROUTE_OK) {
                mprAddItem(tx->outputPipeline, filter);
                if (rx->traceLevel >= 0) {
                    mprLog(rx->traceLevel, "Select output filter: \"%s\"", filter->name);
                }
                hasOutputFilters = 1;
            }
        }
    }
    if (tx->connector == 0) {
#if !BIT_ROM
        if (tx->handler == http->fileHandler && (rx->flags & HTTP_GET) && !hasOutputFilters && 
                !conn->secure && httpShouldTrace(conn, HTTP_TRACE_TX, HTTP_TRACE_BODY, tx->ext) < 0) {
            tx->connector = http->sendConnector;
        } else 
#endif
        if (route && route->connector) {
            tx->connector = route->connector;
        } else {
            tx->connector = http->netConnector;
        }
    }
    mprAddItem(tx->outputPipeline, tx->connector);
    if (rx->traceLevel >= 0) {
        mprLog(rx->traceLevel + 1, "Select connector: \"%s\"", tx->connector->name);
    }
    /*  Create the outgoing queue heads and open the queues */
    q = tx->queue[HTTP_QUEUE_TX];
    for (next = 0; (stage = mprGetNextItem(tx->outputPipeline, &next)) != 0; ) {
        q = httpCreateQueue(conn, stage, HTTP_QUEUE_TX, q);
    }
    conn->connectorq = tx->queue[HTTP_QUEUE_TX]->prevQ;

    /*
        Double the connector max hi-water mark. This optimization permits connectors to accept packets without 
        unnecesary flow control.
     */
    conn->connectorq->max *= 2;

    pairQueues(conn);

    /*
        Put the header before opening the queues incase an open routine actually services and completes the request
     */
    httpPutForService(conn->writeq, httpCreateHeaderPacket(), HTTP_DELAY_SERVICE);

    /*
        Open the pipelien stages. This calls the open entrypoints on all stages
     */
    openQueues(conn);
}


PUBLIC void httpCreateRxPipeline(HttpConn *conn, HttpRoute *route)
{
    HttpTx      *tx;
    HttpRx      *rx;
    HttpQueue   *q;
    HttpStage   *stage, *filter;
    int         next;

    assert(conn);
    assert(route);

    rx = conn->rx;
    tx = conn->tx;
    rx->inputPipeline = mprCreateList(-1, 0);
    if (route) {
        for (next = 0; (filter = mprGetNextItem(route->inputStages, &next)) != 0; ) {
            if (matchFilter(conn, filter, route, HTTP_STAGE_RX) == HTTP_ROUTE_OK) {
                mprAddItem(rx->inputPipeline, filter);
            }
        }
    }
    mprAddItem(rx->inputPipeline, tx->handler ? tx->handler : conn->http->clientHandler);
    /*  Create the incoming queue heads and open the queues.  */
    q = tx->queue[HTTP_QUEUE_RX];
    for (next = 0; (stage = mprGetNextItem(rx->inputPipeline, &next)) != 0; ) {
        q = httpCreateQueue(conn, stage, HTTP_QUEUE_RX, q);
    }
    if (!conn->endpoint) {
        pairQueues(conn);
        openQueues(conn);
    }
}


static void pairQueues(HttpConn *conn)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead, *rq, *rqhead;

    tx = conn->tx;
    qhead = tx->queue[HTTP_QUEUE_TX];
    rqhead = tx->queue[HTTP_QUEUE_RX];
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        if (q->pair == 0) {
            for (rq = rqhead->nextQ; rq != rqhead; rq = rq->nextQ) {
                if (q->stage == rq->stage) {
                    q->pair = rq;
                    rq->pair = q;
                }
            }
        }
    }
}


static int openQueue(HttpQueue *q, ssize chunkSize)
{
    Http        *http;
    HttpConn    *conn;
    HttpStage   *stage;
    MprModule   *module;

    stage = q->stage;
    conn = q->conn;
    http = q->conn->http;

    if (chunkSize > 0) {
        q->packetSize = min(q->packetSize, chunkSize);
    }
    if (stage->flags & HTTP_STAGE_UNLOADED && stage->module) {
        module = stage->module;
        module = mprCreateModule(module->name, module->path, module->entry, http);
        if (mprLoadModule(module) < 0) {
            httpError(conn, HTTP_CODE_INTERNAL_SERVER_ERROR, "Cannot load module %s", module->name);
            return MPR_ERR_CANT_READ;
        }
        stage->module = module;
    }
    if (stage->module) {
        stage->module->lastActivity = http->now;
    }
    return 0;
}


static void openQueues(HttpConn *conn)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead;
    int         i;

    tx = conn->tx;
    for (i = 0; i < HTTP_MAX_QUEUE; i++) {
        qhead = tx->queue[i];
        for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
            if (q->open && !(q->flags & (HTTP_QUEUE_OPEN))) {
                if (q->pair == 0 || !(q->pair->flags & HTTP_QUEUE_OPEN)) {
                    openQueue(q, tx->chunkSize);
                    if (q->open && !tx->finalized) {
                        q->flags |= HTTP_QUEUE_OPEN;
                        q->stage->open(q);
                    }
                }
            }
        }
    }
}


PUBLIC void httpSetSendConnector(HttpConn *conn, cchar *path)
{
#if !BIT_ROM
    HttpTx      *tx;

    tx = conn->tx;
    tx->flags |= HTTP_TX_SENDFILE;
    tx->filename = sclone(path);
#else
    mprError("Send connector not available if ROMFS enabled");
#endif
}


PUBLIC void httpDestroyPipeline(HttpConn *conn)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead;
    int         i;

    tx = conn->tx;
    if (tx) {
        for (i = 0; i < HTTP_MAX_QUEUE; i++) {
            qhead = tx->queue[i];
            for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
                if (q->close && q->flags & HTTP_QUEUE_OPEN) {
                    q->flags &= ~HTTP_QUEUE_OPEN;
                    q->stage->close(q);
                }
            }
        }
    }
}


PUBLIC void httpStartPipeline(HttpConn *conn)
{
    HttpQueue   *qhead, *q, *prevQ, *nextQ;
    HttpTx      *tx;
    HttpRx      *rx;

    tx = conn->tx;
    rx = conn->rx;
    assert(conn->endpoint);

    if (rx->needInputPipeline) {
        qhead = tx->queue[HTTP_QUEUE_RX];
        for (q = qhead->nextQ; !tx->finalized && q->nextQ != qhead; q = nextQ) {
            nextQ = q->nextQ;
            if (q->start && !(q->flags & HTTP_QUEUE_STARTED)) {
                if (q->pair == 0 || !(q->pair->flags & HTTP_QUEUE_STARTED)) {
                    q->flags |= HTTP_QUEUE_STARTED;
                    q->stage->start(q);
                }
            }
        }
    }
    qhead = tx->queue[HTTP_QUEUE_TX];
    for (q = qhead->prevQ; !tx->finalized && q->prevQ != qhead; q = prevQ) {
        prevQ = q->prevQ;
        if (q->start && !(q->flags & HTTP_QUEUE_STARTED)) {
            q->flags |= HTTP_QUEUE_STARTED;
            q->stage->start(q);
        }
    }
    /* 
        Start the handler
     */
    httpStartHandler(conn);
    if (!tx->finalized && !tx->finalizedConnector && rx->remainingContent > 0) {
        /* 
            If no remaining content, wait till the processing stage to avoid duplicate writable events 
         */
        HTTP_NOTIFY(conn, HTTP_EVENT_WRITABLE, 0);
    }
    if (tx->pendingFinalize) {
        tx->finalizedOutput = 0;
        httpFinalizeOutput(conn);
    }
}


PUBLIC void httpReadyHandler(HttpConn *conn)
{
    HttpQueue   *q;
    
    q = conn->writeq;
    if (q->stage && q->stage->ready && !conn->tx->finalized && !(q->flags & HTTP_QUEUE_READY)) {
        q->flags |= HTTP_QUEUE_READY;
        q->stage->ready(q);
    }
}


static void httpStartHandler(HttpConn *conn)
{
    HttpQueue   *q;
    
    assert(!conn->tx->started);

    conn->tx->started = 1;
    q = conn->writeq;
    if (q->stage->start && !conn->tx->finalized && !(q->flags & HTTP_QUEUE_STARTED)) {
        q->flags |= HTTP_QUEUE_STARTED;
        q->stage->start(q);
    }
}


/*
    Get more output by invoking the stage 'writable' callback. Called by processRunning.
 */
PUBLIC bool httpGetMoreOutput(HttpConn *conn)
{
    HttpQueue   *q;
    
    q = conn->writeq;
    if (!q->stage || !q->stage->writable) {
       return 0;
    }
    if (!conn->tx->finalizedOutput) {
        q->stage->writable(q);
        if (q->count > 0) {
            httpScheduleQueue(q);
            httpServiceQueues(conn);
        }
    }
    return 1;
}


/*  
    Run the queue service routines until there is no more work to be done. NOTE: all I/O is non-blocking.
 */
PUBLIC bool httpServiceQueues(HttpConn *conn)
{
    HttpQueue   *q;
    int         workDone;

    workDone = 0;
    while (conn->state < HTTP_STATE_COMPLETE && (q = httpGetNextQueueForService(conn->serviceq)) != NULL) {
        if (q->servicing) {
            q->flags |= HTTP_QUEUE_RESERVICE;
        } else {
            assert(q->schedulePrev == q->scheduleNext);
            httpServiceQueue(q);
            workDone = 1;
        }
    }
    return workDone;
}


PUBLIC void httpDiscardData(HttpConn *conn, int dir)
{
    HttpTx      *tx;
    HttpQueue   *q, *qhead;

    tx = conn->tx;
    if (tx == 0) {
        return;
    }
    qhead = tx->queue[dir];
    for (q = qhead->nextQ; q != qhead; q = q->nextQ) {
        httpDiscardQueueData(q, 1);
    }
}


static bool matchFilter(HttpConn *conn, HttpStage *filter, HttpRoute *route, int dir)
{
    HttpTx      *tx;

    tx = conn->tx;
    if (filter->match) {
        return filter->match(conn, route, dir);
    }
    if (filter->extensions && tx->ext) {
        return mprLookupKey(filter->extensions, tx->ext) != 0;
    }
    return 1;
}


/*
    @copy   default

    Copyright (c) Embedthis Software LLC, 2003-2013. All Rights Reserved.

    This software is distributed under commercial and open source licenses.
    You may use the Embedthis Open Source license or you may acquire a 
    commercial license from Embedthis Software. You agree to be fully bound
    by the terms of either license. Consult the LICENSE.md distributed with
    this software for full details and other copyrights.

    Local variables:
    tab-width: 4
    c-basic-offset: 4
    End:
    vim: sw=4 ts=4 expandtab

    @end
 */
