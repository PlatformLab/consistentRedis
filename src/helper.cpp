#include <math.h>
#include <stdlib.h>
#include "helper.h"

uint64_t xray_average(const uint64_t* data, uint64_t len) {
	uint64_t res = 0;
	// TODO: not considering overflow
	for (int i = 0; i < len; i++) {
		res += data[i];
	}
	return res / len;
}

uint64_t xray_stddev(const uint64_t* data, uint64_t len) {
	uint64_t res = 0;
	// TODO: not considering overflow
	for (int i = 0; i < len; i++) {
		res += data[i] * data[i];
	}
	res = res / len;
	uint64_t avg = xray_average(data, len);
	res = res - avg * avg;
	res = uint64_t(sqrt(double(res)));
	return res;
}

uint64_t xray_max(const uint64_t* data, uint64_t len) {
	uint64_t res = 0;
	for (int i = 0; i < len; i++) {
		if (data[i] > res) {
			res = data[i];
		}
	}
	return res;
}

int xray_local_compare(const void* p1, const void* p2) {
	uint64_t a, b;
	a = *(uint64_t*)p1;
	b = *(uint64_t*)p2;
	if (a < b) return -1;
	if (a == b) return 0;
	return 1;
}

uint64_t xray_percentile(uint64_t* data, uint64_t len, int p) {
	// sort the data
	qsort(data, size_t(len), sizeof(uint64_t), xray_local_compare);
	int idx = double(len) * double(p) / 100.0;
	return data[idx];
}
