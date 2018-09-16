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
//#define WITNESS_NUM_ENTRIES_PER_TABLE 4096 // Must be power of 2.
//#define HASH_BITMASK 4095
#define WITNESS_NUM_ENTRIES_PER_TABLE 1024 // Must be power of 2.
#define HASH_BITMASK 1023
#define WITNESS_ASSOCIATIVITY 4

/**
 * Holds information to recover an RPC request in case of the master's crash
 */
struct Entry {
    bool occupied[WITNESS_ASSOCIATIVITY]; // TODO(seojin): check padding to 64-bit improves perf?
    int16_t requestSize[WITNESS_ASSOCIATIVITY];
    uint32_t keyHash[WITNESS_ASSOCIATIVITY];
    int64_t clientId[WITNESS_ASSOCIATIVITY];
    int64_t requestId[WITNESS_ASSOCIATIVITY];
    char request[WITNESS_ASSOCIATIVITY][MAX_WITNESS_REQUEST_SIZE];
    unsigned long long GcSeqNum[WITNESS_ASSOCIATIVITY]; // GcRpcCount when it arrived.
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
    int gcMissedCount;
    unsigned long long totalGcRpcs;
    int totalRecordRpcs;
    int totalRejection;
    int trueCollision;
};

struct WitnessGcInfo {
    int hashIndex;
    long long clientId;
    long long requestId;
};

/* 0th index is not used. */
struct WitnessGcInfo obsoleteRpcs[50] = {{0,0,0}, };
int obsoleteRpcsSize = 0;

void addToObsoleteRpcs(int hashIndex, long long clientId, long long requestId) {
    if (obsoleteRpcsSize < 50) {
        obsoleteRpcs[obsoleteRpcsSize].hashIndex = hashIndex;
        obsoleteRpcs[obsoleteRpcsSize].clientId = clientId;
        obsoleteRpcs[obsoleteRpcsSize].requestId = requestId;
    }
    // Just ignore if this buffer is full..
}

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
    long long keyHash, clientId, requestId;
    if (getLongFromObjectInBase64OrReply(c, c->argv[1], &masterIdx, NULL) != C_OK) return;
    if (getLongFromObjectInBase64OrReply(c, c->argv[2], &hashIndex, NULL) != C_OK) return;
    if (getLongLongFromObjectInBase64OrReply(c, c->argv[3], &keyHash, NULL) != C_OK) return;
    if (getLongLongFromObjectInBase64OrReply(c, c->argv[4], &clientId, NULL) != C_OK) return;
    if (getLongLongFromObjectInBase64OrReply(c, c->argv[5], &requestId, NULL) != C_OK) return;
    size_t requestSize = sdslen(c->argv[6]->ptr);
    void* data = c->argv[6]->ptr;

    struct Master* buffer = &masters[masterIdx];
    assert(requestSize <= MAX_WITNESS_REQUEST_SIZE);
    assert(hashIndex < WITNESS_NUM_ENTRIES_PER_TABLE);

    buffer->totalRecordRpcs++;
    // Sanity check.
    if (!buffer->writable) {
        addReply(c, shared.witnessReject);
        return;
    }

    int slot = WITNESS_ASSOCIATIVITY; // This means not available.
    for (int i = 0; i < WITNESS_ASSOCIATIVITY; ++i) {
        if (buffer->table[hashIndex].occupied[i]) {
            // Check slot has obsolete RPC.
            if (buffer->totalGcRpcs - buffer->table[hashIndex].GcSeqNum[i] > 100) {
                // Temporary hack. assuming the timing gap between witness
                // and master RPCs are not over 40ms.
                // If the entry has been stayed over 1000 cycles, just delete.
//                serverLog(LL_NOTICE,"Obsolete witness record detected. Re-using this slot");
                --buffer->occupiedCount;
                slot = i;
                break;
            } else if (buffer->totalGcRpcs - buffer->table[hashIndex].GcSeqNum[i] > 2) {
                // Put it in ObsoleteRecords.
                addToObsoleteRpcs(hashIndex, clientId, requestId);
            }

            if (buffer->table[hashIndex].keyHash[i] == (uint32_t)keyHash) {
                // KeyHash collision with existing request.
                slot = WITNESS_ASSOCIATIVITY;
                buffer->trueCollision++;
                break;
            }
        } else {
            slot = i;
        }
    }
    if (slot < WITNESS_ASSOCIATIVITY) {
        buffer->table[hashIndex].occupied[slot] = true;
        buffer->table[hashIndex].keyHash[slot] = (uint32_t)keyHash;
        buffer->table[hashIndex].requestSize[slot] = requestSize;
        buffer->table[hashIndex].clientId[slot] = clientId;
        buffer->table[hashIndex].requestId[slot] = requestId;
        memcpy(buffer->table[hashIndex].request[slot], data, requestSize);
        buffer->table[hashIndex].GcSeqNum[slot] = buffer->totalGcRpcs;
        addReply(c, shared.witnessAccept);
        ++buffer->occupiedCount;
    } else {
        addReply(c, shared.witnessReject);
        buffer->totalRejection++;
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
        if (getLongFromObjectInBase64OrReply(c, c->argv[i], &hashIndex, NULL) != C_OK) return;
        if (getLongLongFromObjectInBase64OrReply(c, c->argv[i+1], &clientId, NULL) != C_OK) return;
        if (getLongLongFromObjectInBase64OrReply(c, c->argv[i+2], &requestId, NULL) != C_OK) return;

        bool foundInTable = false;
        for (int slot = 0; slot < WITNESS_ASSOCIATIVITY; ++slot) {
            if (buffer->table[hashIndex].occupied[slot] &&
                    buffer->table[hashIndex].clientId[slot] == clientId &&
                    buffer->table[hashIndex].requestId[slot] == requestId) {
                buffer->table[hashIndex].occupied[slot] = false;
                --buffer->occupiedCount;
                foundInTable = true;
    //            succeeded++;
                break;
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
        if (!foundInTable) {
            ++buffer->gcMissedCount;
        }
    }
    ++buffer->totalGcRpcs;
//    addReply(c, shared.ok);

    // Reply with ObsoleteRpcs.
    addReplyMultiBulkLen(c, obsoleteRpcsSize * 3);
    for (int i = 0; i < obsoleteRpcsSize; ++i) {
        addReplyBulkLongLong(c, obsoleteRpcs[i].hashIndex);
        addReplyBulkLongLong(c, obsoleteRpcs[i].clientId);
        addReplyBulkLongLong(c, obsoleteRpcs[i].requestId);
    }
    obsoleteRpcsSize = 0;

//    serverLog(LL_NOTICE,"Witness GC received. total entries: %d, cleaned: %d, failed: %d",
//            (c->argc-2)/3, succeeded, failed);
    if (server.unixtime - lastStatPrintTime > 10) {
        serverLog(LL_NOTICE,"Witness stat.. occupied: %d, use ratio: %2.3f %%, total GC missed count: %d, total GC rpcs: %llu, total rejection: %d, false collision: %d, cumRejectRate: %4.3f %%",
                buffer->occupiedCount, ((double)buffer->occupiedCount * 100) /
                WITNESS_NUM_ENTRIES_PER_TABLE / WITNESS_ASSOCIATIVITY,
                buffer->gcMissedCount, buffer->totalGcRpcs, buffer->totalRejection,
                buffer->totalRejection - buffer->trueCollision,
                (double)(buffer->totalRejection) * 100 / (double)(buffer->totalRecordRpcs));
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
        for (int slot = 0; slot < WITNESS_ASSOCIATIVITY; ++slot) {
            if (buffer->table[i].occupied[slot]) {
    //            totalSize += buffer->table[i].requestSize;
                count++;
            }
        }
    }
    addReplyMultiBulkLen(c, count);
//    addReplyMultiBulkLen(c, totalSize);
    for (int i = 0; i < WITNESS_NUM_ENTRIES_PER_TABLE; ++i) {
        for (int slot = 0; slot < WITNESS_ASSOCIATIVITY; ++slot) {
            if (buffer->table[i].occupied[slot]) {
                addReplySds(c, sdsnewlen(buffer->table[i].request[slot],
                                         buffer->table[i].requestSize[slot]));
            }
        }
    }
}