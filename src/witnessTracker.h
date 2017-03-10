/*
 * Copyright (c) 2017 Stanford University.
 * All rights reserved.
 */

#ifndef __WITNESSTRACKER_H
#define __WITNESSTRACKER_H

#include <stdio.h>

/* TBD: include only necessary headers. */
#include "server.h"

void trackUnsyncedRpc(client *c);
void scheduleFsyncAndWitnessGc();
void witnessListChanged();
bool recoverFromWitness();

#endif
