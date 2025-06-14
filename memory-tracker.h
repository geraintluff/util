/* Currently only working/tested on Mac.  You need to compile in `memory-tracker.hxx` as well, which does the actual stuff */
#pragma once

#include <cstddef>

namespace signalsmith {

struct MemoryTracker {
	static const bool implemented; // Whether the implementation actually tracks memory or not

	size_t allocBytes, freeBytes, currentBytes;
	MemoryTracker();
	
	MemoryTracker diff() const {
		MemoryTracker now;
		return {now.allocBytes - allocBytes, now.freeBytes - freeBytes};
	}

	// Is a `.diff()` result non-zero
	operator bool() const {
		return allocBytes > 0 || freeBytes > 0;
	}
private:
	MemoryTracker(size_t allocBytes, size_t freeBytes) : allocBytes(allocBytes), freeBytes(freeBytes), currentBytes(allocBytes - freeBytes) {}
};

} // namespace
