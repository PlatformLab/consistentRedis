/* Copyright (c) 2017 Stanford University
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR(S) DISCLAIM ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL AUTHORS BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "server.h"
#include "redisassert.h"

#define MAX_WITNESS_REQUEST_SIZE 2048
//    static const int NUM_ENTRIES_PER_TABLE = 512; // Must be power of 2.
#define WITNESS_NUM_ENTRIES_PER_TABLE 4096 // Must be power of 2.
#define HASH_BITMASK 4095

/**
 * Holds information to recover an RPC request in case of the master's crash
 */
struct Entry {
    bool occupied; // TODO(seojin): check padding to 64-bit improves perf?
    int16_t requestSize;
    int64_t clientId;
    int64_t requestId;
    char request[MAX_WITNESS_REQUEST_SIZE];
};

/**
 * Holds information of a master being witnessed. Holds recent & unsynced
 * requests to the master.
 */
struct Master {
    uint64_t id;
    bool writable;
    struct Entry table[WITNESS_NUM_ENTRIES_PER_TABLE];
    int occupiedCount;
};

struct Master masters[10];
time_t lastStatPrintTime = 0;

void witnessInit() {
    memset(masters, 0, sizeof(masters));
    for (int i = 0; i < 10; ++i) {
        masters[i].writable = true;
    }
}

void wrecordCommand(client *c) {
    long masterIdx, hashIndex;
    long long clientId, requestId;
    if (getLongFromObjectOrReply(c, c->argv[1], &masterIdx, NULL) != C_OK) return;
    if (getLongFromObjectOrReply(c, c->argv[2], &hashIndex, NULL) != C_OK) return;
    if (getLongLongFromObjectOrReply(c, c->argv[3], &clientId, NULL) != C_OK) return;
    if (getLongLongFromObjectOrReply(c, c->argv[4], &requestId, NULL) != C_OK) return;
    size_t requestSize = sdslen(c->argv[5]->ptr);
    void* data = c->argv[5]->ptr;

    struct Master* buffer = &masters[masterIdx];
    assert(requestSize <= MAX_WITNESS_REQUEST_SIZE);
    assert(hashIndex < WITNESS_NUM_ENTRIES_PER_TABLE);

    // Sanity check.
    if (!buffer->writable) {
        addReply(c, shared.witnessReject);
        return;
    }

    if (!buffer->table[hashIndex].occupied) {
        buffer->table[hashIndex].occupied = true;
        buffer->table[hashIndex].requestSize = requestSize;
        buffer->table[hashIndex].clientId = clientId;
        buffer->table[hashIndex].requestId = requestId;
        memcpy(buffer->table[hashIndex].request, data, requestSize);
        addReply(c, shared.witnessAccept);
        ++buffer->occupiedCount;
    } else {
        addReply(c, shared.witnessReject);
#if 0
        char* dataInStr = malloc(buffer->table[hashIndex].requestSize + 1);
        memcpy(dataInStr, buffer->table[hashIndex].request, buffer->table[hashIndex].requestSize);
        dataInStr[buffer->table[hashIndex].requestSize] = 0;
        serverLog(LL_NOTICE,"Witness record request rejected. Occupied by data: %s "
                "======== (size: %d, clientId: %"PRId64", requestId: %"PRId64")\n",
                dataInStr, buffer->table[hashIndex].requestSize, buffer->table[hashIndex].clientId,
                buffer->table[hashIndex].requestId);
        free(dataInStr);
#endif
    }
}

void
witnessGcCommand(client *c) {
    long masterIdx;
    if (getLongFromObjectOrReply(c, c->argv[1], &masterIdx, NULL) != C_OK) return;

    struct Master* buffer = &masters[masterIdx];

//    int succeeded = 0, failed = 0;
    for (int i = 2; i < c->argc; i += 3) {
        long hashIndex;
        long long clientId, requestId;
        if (getLongFromObjectOrReply(c, c->argv[i], &hashIndex, NULL) != C_OK) return;
        if (getLongLongFromObjectOrReply(c, c->argv[i+1], &clientId, NULL) != C_OK) return;
        if (getLongLongFromObjectOrReply(c, c->argv[i+2], &requestId, NULL) != C_OK) return;

        if (buffer->table[hashIndex].occupied &&
                buffer->table[hashIndex].clientId == clientId &&
                buffer->table[hashIndex].requestId == requestId) {
            buffer->table[hashIndex].occupied = false;
            --buffer->occupiedCount;
//            succeeded++;
        } else {
//            serverLog(LL_NOTICE,"Witness GC failed. hashIndex: %ld, occupied: %d"
//                    " clientId: %"PRId64" (given %lld) requestId: %"PRId64" (given %lld)",
//                    hashIndex,
//                    buffer->table[hashIndex].occupied,
//                    buffer->table[hashIndex].clientId, clientId,
//                    buffer->table[hashIndex].requestId, requestId);
//            failed++;
        }
    }
    addReply(c, shared.ok);
//    serverLog(LL_NOTICE,"Witness GC received. total entries: %d, cleaned: %d, failed: %d",
//            (c->argc-2)/3, succeeded, failed);
    if (server.unixtime - lastStatPrintTime > 10) {
        serverLog(LL_NOTICE,"Witness stat.. occupied: %d, use ratio: %2.3f",
                buffer->occupiedCount, ((double)buffer->occupiedCount * 100) / WITNESS_NUM_ENTRIES_PER_TABLE);
        lastStatPrintTime = server.unixtime;
    }
}

void witnessGetRecoveryDataCommand(client *c) {
    long masterIdx;
    if (getLongFromObjectOrReply(c, c->argv[1], &masterIdx, NULL) != C_OK) return;

    struct Master* buffer = &masters[masterIdx];
    int count = 0;
//    int totalSize = 0;
    for (int i = 0; i < WITNESS_NUM_ENTRIES_PER_TABLE; ++i) {
        if (buffer->table[i].occupied) {
//            totalSize += buffer->table[i].requestSize;
            count++;
        }
    }
    addReplyMultiBulkLen(c, count);
//    addReplyMultiBulkLen(c, totalSize);
    for (int i = 0; i < WITNESS_NUM_ENTRIES_PER_TABLE; ++i) {
        if (buffer->table[i].occupied) {
            addReplySds(c, sdsnewlen(buffer->table[i].request,
                                     buffer->table[i].requestSize));
        }
    }
}