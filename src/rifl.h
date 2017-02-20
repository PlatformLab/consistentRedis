/*
 * Copyright (c) 2017 Stanford University.
 * All rights reserved.
 */

#ifndef __RIFL_H
#define __RIFL_H

#include <stdio.h>

/* TBD: include only necessary headers. */
#include "server.h"

/*
 * Assumption: client only sends RPCs in order.
 * We can assume this since Redis uses TCP socket.
 */
bool riflCheckClientIdOk(client *c);
bool riflCheckDuplicate(long long clientId, long long requestId);
void riflStartRecoveryByWitness();
void riflEndRecoveryByWitness();

/* Used for AOF rewrite. */
long long riflGetNext(long long clientId);
long long riflGetprocessedRpcId(long long clientId);

#endif
