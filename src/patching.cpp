#include <stdio.h>
#include "xray/xray_log_interface.h"
#include "helper.h"
#include <stdint.h>
#include "nanolog.h"

int instrumented() __attribute__((xray_always_instrument));
int instrumented2() __attribute__((xray_always_instrument));
int instrumented3() __attribute__((xray_always_instrument));
int instrumented4() __attribute__((xray_always_instrument));
int instrumented5() __attribute__((xray_always_instrument));

#ifndef XRAY_MODE
#define XRAY_MODE "nanolog"
#endif

#define REPEAT 1000
#define CACHE_REPEAT 10
#define PERCENTILE 95

thread_local int req_id;

int main() {
	// log the data
	uint64_t *d1, *d2;
	d1 = (uint64_t*) malloc(REPEAT * sizeof(uint64_t));
	d2 = (uint64_t*) malloc(REPEAT * sizeof(uint64_t));
	uint8_t c1, c2;
	uint64_t s, e;

	// Enable the instrumentation and patch the code
	auto register_status = __xray_log_register_mode(XRAY_MODE, nanolog_impl);
        if (register_status != XRayLogRegisterStatus::XRAY_REGISTRATION_OK) {
                printf("Error registering XRay mode\n");
                return 1;
        }

	auto select_status = __xray_log_select_mode(XRAY_MODE);
	if (select_status != XRayLogRegisterStatus::XRAY_REGISTRATION_OK) {
		printf("Error selecting XRay mode\n");
		return 1;
	}
	auto config_status = __xray_log_init_mode(XRAY_MODE, "verbosity=0");
	if (config_status != XRayLogInitStatus::XRAY_LOG_INITIALIZED) {
		printf("Error initializing XRay mode\n");
		return 1;
	}
	auto patch_status = __xray_patch();
	if (patch_status != XRayPatchingStatus::SUCCESS) {
		printf("Error patching the binary\n");
		return 1;
	}

	// try to make the functions into the icache
	for (int i = 0; i < CACHE_REPEAT; i++) {
		patch_status = __xray_patch();
		if (patch_status != XRayPatchingStatus::SUCCESS) {
			printf("Error patching the binary\n");
			return 1;
		}
		patch_status = __xray_unpatch();
		if (patch_status != XRayPatchingStatus::SUCCESS) {
			printf("Error unpatching the binary\n");
			return 1;
		}
	}
	for (int i = 0; i < REPEAT; i++) {
		bool interrupted = false;
		s = readTSC(c1);
		patch_status = __xray_patch();
		e = readTSC(c2);
		if (patch_status != XRayPatchingStatus::SUCCESS) {
			printf("Error patching the binary\n");
			return 1;
		}
		if (c1 != c2) {
			interrupted = true;
		}
		d1[i] = e - s;
		s = readTSC(c1);
		patch_status = __xray_unpatch();
		e = readTSC(c2);
		if (patch_status != XRayPatchingStatus::SUCCESS) {
			printf("Error unpatching the binary\n");
			return 1;
		}
		if (c1 != c2) {
			interrupted = true;
		}
		d2[i] = e - s;
		if (interrupted) {
			i -= 1;
		}
		
	}
	uint64_t patch_avg, patch_stddev, patch_max, patch_min;
	uint64_t unpatch_avg, unpatch_stddev, unpatch_max, unpatch_min;
	patch_avg = xray_average(d1, REPEAT);
	patch_stddev = xray_stddev(d1, REPEAT);
	patch_min = xray_percentile(d1, REPEAT, 100-PERCENTILE);
	patch_max = xray_percentile(d1, REPEAT, PERCENTILE);
	unpatch_avg = xray_average(d2, REPEAT);
	unpatch_stddev = xray_stddev(d2, REPEAT);
	unpatch_min = xray_percentile(d2, REPEAT, 100-PERCENTILE);
	unpatch_max = xray_percentile(d2, REPEAT, PERCENTILE);

	// Print out the results
	printf("Patching:   %ld cycles (+/- %ld),\t%d%%=%ld, %d%%=%ld\n", patch_avg, patch_stddev, 100-PERCENTILE, patch_min, PERCENTILE, patch_max);
	printf("Unpatching: %ld cycles (+/- %ld),\t%d%%=%ld, %d%%=%ld\n", unpatch_avg, unpatch_stddev, 100-PERCENTILE, unpatch_min, PERCENTILE, unpatch_max);

	return 0;
}

int instrumented() {
	return 0;
}
int instrumented2() {
	return 1;
}
int instrumented3() {
	return 2;
}
int instrumented4() {
	return 3;
}
int instrumented5() {
	return 4;
}
