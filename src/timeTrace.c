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

#include "timeTrace.h"
#include <errno.h>
#include <stdint.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>

/*================================= Globals ================================= */

/* Global vars */
// Determines the number of events we can retain as an exponent of 2
//#define BUFFER_SIZE_EXP 16

// Total number of events that we can retain any given time.
#define BUFFER_SIZE (1 << 16)

// Bit mask used to implement a circular event buffer
#define BUFFER_MASK() (BUFFER_SIZE - 1)

// Index within events of the slot to use for the next call to the
// record method.
int nextIndex;

/**
 * This structure holds one entry in the TimeTrace.
 */
struct Event {
    uint64_t timestamp;        // Time when a particular event occurred.
    const char* format;        // Format string describing the event.
                               // NULL means that this entry is unused.
    int arg0;             // Argument that may be referenced by format
                               // when printing out this event.
    int arg1;             // Argument that may be referenced by format
                               // when printing out this event.
    int arg2;             // Argument that may be referenced by format
                               // when printing out this event.
    int arg3;             // Argument that may be referenced by format
                               // when printing out this event.
};

// Holds information from the most recent calls to the record method.
struct Event events[BUFFER_SIZE] = {{ 0 }};

double cyclesPerSec = 0;

/*================================= Functions =============================== */
void init() {
    if (cyclesPerSec != 0)
        return;

    // Compute the frequency of the fine-grained CPU timer: to do this,
    // take parallel time readings using both rdtsc and gettimeofday.
    // After 10ms have elapsed, take the ratio between these readings.

    struct timeval startTime, stopTime;
    uint64_t startCycles, stopCycles, micros;
    double oldCycles;

    // There is one tricky aspect, which is that we could get interrupted
    // between calling gettimeofday and reading the cycle counter, in which
    // case we won't have corresponding readings.  To handle this (unlikely)
    // case, compute the overall result repeatedly, and wait until we get
    // two successive calculations that are within 0.1% of each other.
    oldCycles = 0;
    while (1) {
        if (gettimeofday(&startTime, NULL) != 0) {
            fprintf(stderr, "Cycles::init couldn't read clock: %s", strerror(errno));
            fflush(stderr);
            exit(1);
        }
        startCycles = rdtsc();
        while (1) {
            if (gettimeofday(&stopTime, NULL) != 0) {
                fprintf(stderr, "Cycles::init couldn't read clock: %s",
                        strerror(errno));
                fflush(stderr);
                exit(1);
            }
            stopCycles = rdtsc();
            micros = (stopTime.tv_usec - startTime.tv_usec) +
                    (stopTime.tv_sec - startTime.tv_sec)*1000000;
            if (micros > 10000) {
                cyclesPerSec = (double)(stopCycles - startCycles);
                cyclesPerSec = 1000000.0*cyclesPerSec/
                        (double)(micros);
                break;
            }
        }
        double delta = cyclesPerSec/1000.0;
        if ((oldCycles > (cyclesPerSec - delta)) &&
                (oldCycles < (cyclesPerSec + delta))) {
            return;
        }
        oldCycles = cyclesPerSec;
    }
}

double toSeconds(uint64_t cycles)
{
    if (cyclesPerSec == 0) init();
    return ((double)(cycles))/cyclesPerSec;
}

void recordWithTime(uint64_t timestamp, const char* format,
        uint32_t arg0, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    struct Event* event = &events[nextIndex];
    nextIndex = (nextIndex + 1) & BUFFER_MASK();

    event->timestamp = timestamp;
    event->format = format;
    event->arg0 = arg0;
    event->arg1 = arg1;
    event->arg2 = arg2;
    event->arg3 = arg3;
}

void
printTrace(const char *filename)
{
    uint8_t printedAnything = 0;
    // reader start?

    // Initialize file for writing
    FILE* output = filename ? fopen(filename, "a") : stdout;

    // Holds the index of the next event to consider from each trace.
    int current;

    // Find the first (oldest) event in each trace. This will be events[0]
    // if we never completely filled the buffer, otherwise events[nextIndex+1].
    // This means we don't print the entry at nextIndex; this is convenient
    // because it simplifies boundary conditions in the code below.

    int index = (nextIndex + 1) % BUFFER_SIZE;
    if (events[index].format != NULL) {
        current = index;
    } else {
        current = 0;
    }

    // Decide on the time of the first event to be included in the output.
    // This is most recent of the oldest times in all the traces (an empty
    // trace has an "oldest time" of 0). The idea here is to make sure
    // that there's no missing data in what we print (if trace A goes back
    // farther than trace B, skip the older events in trace A, since there
    // might have been related events that were once in trace B but have since
    // been overwritten).
    uint64_t startTime = 0;
    startTime = events[current].timestamp;

    // Each iteration through this loop processes one event (the one with
    // the earliest timestamp).
    double prevTime = 0.0;
    while (1) {
        if (current == nextIndex || events[current].format == NULL) {
            // Don't have any more events to process.
            break;
        }

        printedAnything = 1;
        struct Event* event = &events[current];
        current = (current + 1) & BUFFER_MASK();

        char message[1000];
        double ns = toSeconds(event->timestamp - startTime) * 1e09;

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        snprintf(message, sizeof(message), event->format, event->arg0,
                 event->arg1, event->arg2, event->arg3);
#pragma GCC diagnostic pop
        fprintf(output, "%8.1f ns (+%6.1f ns): %s", ns, ns - prevTime,
                message);
        fputc('\n', output);

        prevTime = ns;
    }

    if (!printedAnything) {
        fprintf(output, "No time trace events to print");
    }

    if (output && output != stdout)
        fclose(output);
}
