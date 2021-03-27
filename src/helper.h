#pragma once
#include <stdint.h>
#include <x86intrin.h>

inline __attribute__((always_inline)) uint64_t readTSC(uint8_t &CPU) __attribute__((xray_never_instrument)) {
	unsigned LongCPU;
	unsigned long Rax, Rdx;
	__asm__ __volatile__("rdtscp\n" : "=a"(Rax), "=d"(Rdx), "=c"(LongCPU) ::);
	CPU = LongCPU;
	return (Rdx << 32) + Rax;
}

uint64_t xray_average(const uint64_t* data, uint64_t len);

uint64_t xray_stddev(const uint64_t* data, uint64_t len);

uint64_t xray_max(const uint64_t* data, uint64_t len);

int xray_local_compare(const void* p1, const void* p2);

uint64_t xray_percentile(uint64_t* data, uint64_t len, int p);
