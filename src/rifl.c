/*
 * Copyright (c) 2017 Stanford University.
 * All rights reserved.
 */

#include "server.h"
#include "redisassert.h"

/*================================= Globals ================================= */

/* Global vars */
#define RIFL_TABLE_SIZE 1048576 /* 1024*1024. This must be power of two. */

/* 0th index is not used. */
long long processedRpcIds[RIFL_TABLE_SIZE] = {0, };
long long clientIds[RIFL_TABLE_SIZE] = {0, };
int bitmask = RIFL_TABLE_SIZE - 1;
bool witnessRecoveryMode = false; /* Don't bump processedRpcId while recovery */

/*================================= Functions =============================== */
bool riflCheckClientIdOk(client *c) {
    if (clientIds[c->clientId & bitmask]) {
        return clientIds[c->clientId & bitmask] == c->clientId;
    }
    clientIds[c->clientId & bitmask] = c->clientId;
    return true;
}

bool riflCheckDuplicate(long long clientId, long long requestId) {
    if (clientId == 0) {
        assert(requestId == 0);
        return false;
    }

    int index = clientId & bitmask;
    if (processedRpcIds[index] >= requestId) {
        return true;
    }
    if (!witnessRecoveryMode) {
        processedRpcIds[index] = requestId;
    }
    return false;
}

void riflStartRecoveryByWitness() {
    witnessRecoveryMode = true;
}

void riflEndRecoveryByWitness() {
    witnessRecoveryMode = false;
}


long long riflGetNext(long long clientId) {
    int index = (clientId + 1) & bitmask;
    while (index < RIFL_TABLE_SIZE) {
        if (clientIds[index]) {
            return clientIds[index];
        }
        ++index;
    }
    return -1; // End of table.
}

long long riflGetprocessedRpcId(long long clientId) {
    int index = clientId & bitmask;
    assert(clientIds[index] == clientId);
    return processedRpcIds[index];
}