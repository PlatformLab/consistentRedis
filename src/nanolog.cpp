#include "nanolog.h"
#include "xray_interface.h"
#include "stdio.h"
#include "helper.h"
#include <vector>
#include <atomic>

extern thread_local int req_id;

thread_local NanologBuffer* buffer;

NanologBufferQueue buffer_queue;

std::atomic_flag lock = ATOMIC_FLAG_INIT;

#define ALLOC_SIZE 2048

NanologBuffer::NanologBuffer() {
	capacity = ALLOC_SIZE;
	size = 0;
	entries = new NanologEntry[ALLOC_SIZE];
}

NanologEntry::NanologEntry(uint64_t tsc, int32_t func_id, uint8_t cpu, XRayEntryType entry, int req_id) {
	tsc = tsc;
	func_id = func_id;
	cpu = cpu;
	entry_type = entry;
	req_id = req_id;
}

XRayLogInitStatus nanolog_init(size_t buf_size, size_t max_bufs, void* args, size_t args_size) {
	printf("nanolog initialized\n");
	return XRayLogInitStatus::XRAY_LOG_INITIALIZED;
}

XRayLogInitStatus nanolog_finalize() {
	printf("nanolog finalized\n");
	return XRayLogInitStatus::XRAY_LOG_FINALIZED;
}

[[clang::xray_never_instrument]]
NanologBuffer* get_buffer() {
	if (buffer != NULL && buffer -> capacity > buffer -> size) {
		return buffer;
	} else {
		// allocate a buffer
		buffer = new NanologBuffer;
		// append to the vector
		// lock
		while (lock.test_and_set(std::memory_order_acquire));
		buffer_queue.buffers.push_back(buffer);
		// unlock
		lock.clear(std::memory_order_release); 
		// set the thread-local pointer
		return buffer;
	}
}

void nanolog_handle_arg0(int32_t func_id, XRayEntryType entry_type) {
	NanologBuffer* buf = get_buffer();
	uint8_t cpuid;
	uint64_t tsc;
	tsc = readTSC(cpuid);
	buf->entries[buf->size] = NanologEntry(tsc, func_id, cpuid, entry_type, req_id);
	buf -> size += 1;
}

XRayLogFlushStatus nanolog_flush_log() {
	return XRayLogFlushStatus::XRAY_LOG_FLUSHED;
}

