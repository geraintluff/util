#pragma once

#include <atomic>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <iostream>
#include <cstdio>
#include <fstream>

// This isn't for microbenchmarks, but collecting rough performance stats to identify problem areas
// so our goal is to get the fastest timestamp with enough accuracy that the averages are meaningful.
#if defined(WINDOWS)
#	include <windows.h>
namespace signalsmith {
struct CpuTime {
	using Time = __int64;

	static CpuTime now() {
		LARGE_INTEGER result;
		QueryPerformanceCounter(&result);
		return result.QuadPart;
	}

	double seconds() const {
		LARGE_INTEGER freq;
		QueryPerformanceFrequency(&freq);
		return time/double(freq);
	}
	
	CpuTime operator+(const CpuTime &other) const {
		return {time + other.time};
	}
	CpuTime operator-(const CpuTime &other) const {
		return {time - other.time};
	}
#elif defined(__has_include) && __has_include(<time.h>)
#	include <time.h>
namespace signalsmith {
struct CpuTime {
private:
	inline void norm() {
		if (time.tv_nsec < 0) {
			time = {time.tv_sec - 1, time.tv_nsec + 1000000000};
		} else if (time.tv_nsec >= 1000000000) {
			time = {time.tv_sec + 1, time.tv_nsec - 1000000000};
		}
	}
public:
	using Time = timespec;
	
	static CpuTime now() {
		CpuTime result;
#ifdef CLOCK_MONOTONIC_COARSE // CLOCK_THREAD_CPUTIME_ID seems better, but it's slow enough to be a problem for measuring DSP code
		auto errorCode = clock_gettime(CLOCK_MONOTONIC_COARSE, &result.time);
#elif defined(CLOCK_MONOTONIC_FAST)
		auto errorCode = clock_gettime(CLOCK_MONOTONIC_FAST, &result.time);
#else
		auto errorCode = clock_gettime(CLOCK_MONOTONIC, &result.time);
#endif
		if (errorCode) result = Time{0, 0};
		return result;
	}

	double seconds() const {
		return time.tv_sec + time.tv_nsec*1e-9;
	}
	
	CpuTime operator+(const CpuTime &other) const {
		CpuTime result{Time{time.tv_sec + other.time.tv_sec, time.tv_nsec + other.time.tv_nsec}};
		result.norm();
		return result;
	}
	CpuTime operator-(const CpuTime &other) const {
		CpuTime result{Time{time.tv_sec - other.time.tv_sec, time.tv_nsec - other.time.tv_nsec}};
		result.norm();
		return result;
	}
#else
// Fallback, using C++ stdlib
#	include <chrono>
namespace signalsmith {
struct CpuTime {
	using Time = std::chrono::steady_clock::duration;

	static CpuTime now() {
		return {std::chrono::steady_clock::now().time_since_epoch()};
	}
	double seconds() const {
		return std::chrono::duration<double>(time).count();
	}
	
	CpuTime operator+(const CpuTime &other) const {
		return {time + other.time};
	}
	CpuTime operator-(const CpuTime &other) const {
		return {Time(time - other.time)};
	}
#endif

	CpuTime() {}

	Time time;
	CpuTime(Time time) : time(time) {}
};

namespace _timemonitor_impl {
	struct Event {
		size_t depth;
		const char *label;
		CpuTime time;
		double refSeconds;
		
		bool valid() const {
			return label != nullptr;
		}
	};

	struct EventList {
		std::vector<Event> events;
		std::atomic<size_t> filledToIndex = 0;
		std::atomic<bool> claimed = false;
		std::atomic<bool> wantsToGrow = false;

		EventList(size_t initialSize) : events(initialSize) {}
		EventList(const EventList &other) = delete;
		EventList(EventList &&other) : events(std::move(other.events)) {
			if (claimed || filledToIndex > 0) abort();
		}

		Event * claimFromWriter() {
			if (!claimed.exchange(true)) {
				return events.data() + filledToIndex;
			}
			return nullptr;
		}
		void releaseFromWriter(Event *writerEvent) {
			if (writerEvent != nullptr) {
				filledToIndex = size_t(writerEvent - events.data());
			} else {
				// Passing null means the event-source ran out of space, so we throw away all the new ones
				wantsToGrow = true;
			}
			claimed = false;
		}

		bool mayContainEvents() const { // Doesn't guarantee it *can* be claimed (or have anything to process) but avoids claiming unnecessarily
			return (filledToIndex > 0 || wantsToGrow) && !claimed;
		}
		bool claimFromReport() {
			return !claimed.exchange(true);
		}
		void releaseFromReport() {
			if (!events.size()) {
				events.resize(16);
			} else if (wantsToGrow || filledToIndex > events.size()/2) {
				events.resize(events.size()*2);
			}
			wantsToGrow = false;
			filledToIndex = 0;
			claimed = false;
		}
	};

	using EventListSet = std::vector<EventList>;
}

// Once constructed (or obtained from `TimeMonitor::eventSource()`, this should be used by a single thread (at a time)
// It's fairly small (5 pointers) and doesn't allocate, so could be a thread-local to get an automatic scope
struct TimeMonitorEventSource {
	bool enabled = true;

	TimeMonitorEventSource(_timemonitor_impl::EventListSet &eventLists) : eventLists(eventLists) {}
	// Move but not copy
	TimeMonitorEventSource(TimeMonitorEventSource &&other) : eventLists(other.eventLists) {
		if (other.depth > 0) abort(); // Nope
		// events/eventsEnd/claimedList/depth shouldn't be set if depth is 0
	}
	TimeMonitorEventSource(const TimeMonitorEventSource &other) = delete;

	struct Scoped {
		// No copy/move/etc.
		Scoped(const Scoped &other) = delete;
		Scoped(Scoped &&other) = delete;
		
		~Scoped() {
			eventSource.leave();
		}
		
		void replace(const char *newLabel, double refSeconds=0) {
			eventSource.leave();
			label = newLabel;
			eventSource.enter(label, refSeconds);
		}
		
		Scoped scoped(const char *newLabel, double refSeconds=0) {
			return Scoped(eventSource, newLabel, refSeconds);
		}
		Scoped scoped(const char *newLabel, float refSeconds) {
			return Scoped(eventSource, newLabel, double(refSeconds));
		}
		template<class Fn>
		void scoped(const char *label, Fn &&fn, double refSeconds=0) {
			Scoped sub{eventSource, label, refSeconds};
			fn();
		}
	private:
		TimeMonitorEventSource &eventSource;
		const char *label;

		friend struct TimeMonitorEventSource;
		Scoped(TimeMonitorEventSource &eventSource, const char *label, double refSeconds) : eventSource(eventSource), label(label) {
			eventSource.enter(label, refSeconds);
		}
	};
	
	Scoped scoped(const char *label, double refSeconds=0) {
		return Scoped{*this, label, refSeconds};
	}
	Scoped scoped(const char *label, float refSeconds) {
		return Scoped{*this, label, double(refSeconds)};
	}
	template<class Fn>
	void scoped(const char *label, Fn &&fn, double refSeconds=0) {
		Scoped sub{*this, label, refSeconds};
		fn();
	}
private:
	using EventListSet = _timemonitor_impl::EventListSet;
	using EventList = _timemonitor_impl::EventList;
	using Event = _timemonitor_impl::Event;

	EventListSet &eventLists;
	CpuTime refTime;
	size_t depth = 0;

	EventList *claimedList = nullptr;
	Event *events = nullptr;
	Event *eventsEnd = nullptr;

	inline void tryClaim() {
		events = eventsEnd = nullptr;
		if (!enabled) return;
		for (auto &list : eventLists) {
			events = list.claimFromWriter();
			if (events) {
				claimedList = &list;
				eventsEnd = list.events.data() + list.events.size();
				break;
			}
		}
	}
	
	inline void enter(const char *name, double refSeconds) {
		auto now = CpuTime::now();
		if (depth == 0) {
			tryClaim();
			refTime = now;
		}
		++depth;
		if (events && events < eventsEnd) {
			*events = {depth, name, now - refTime, refSeconds};
			++events;
		} else {
			events = nullptr;
		}
	}
	inline void leave() {
		--depth;
		if (events && events < eventsEnd) {
			auto now = CpuTime::now();
			*events = {depth, "", now - refTime};
			++events;
		} else {
			events = nullptr;
		}
		if (depth == 0) {
			if (claimedList) claimedList->releaseFromWriter(events);
			claimedList = nullptr;
		}
	}
};
	
struct TimeMonitor {
	TimeMonitor(size_t listCount=2, size_t initialSize=256) {
		while (eventLists.size() < listCount) {
			eventLists.emplace_back(initialSize);
		}
	}
	
	using EventSource = TimeMonitorEventSource;
	EventSource eventSource() {
		return {eventLists};
	}

	// Report data/methods

	struct Stats {
		double count = 0; // observations, as floating-point so we can decay it
		double sum = 0;
		double sum2 = 0;
		
		void add(double v) {
			++count;
			sum += v;
			sum2 += v*v;
		}
		void decay(double d) {
			count *= d;
			sum *= d;
			sum2 *= d;
		}
		
		double mean() const {
			return (count > 0) ? sum/count : 0;
		}
	};

	struct ReportItem;
	struct Report {
		std::unordered_map<std::string, ReportItem> items;
		
		void clear() {
			items.clear();
		}

		struct Named {
			const std::string &name;
			const ReportItem &item;
		};
		std::vector<Named> named(bool lengthSort=false) const {
			// A pointer version so we can sort them, and also not re-allocate strings
			struct PtrNamed {
				const std::string *name;
				const ReportItem *item;
			};
			std::vector<PtrNamed> ptrResult;
			ptrResult.reserve(items.size());
			for (auto &pair : items) {
				ptrResult.emplace_back(PtrNamed{&pair.first, &pair.second});
			}
			if (!lengthSort) {
				// Earliest first
				std::sort(ptrResult.begin(), ptrResult.end(), [](const PtrNamed &a, const PtrNamed &b){
					return a.item->start.mean() < b.item->start.mean();
				});
			} else {
				// Longest first
				std::sort(ptrResult.begin(), ptrResult.end(), [](const PtrNamed &a, const PtrNamed &b){
					return a.item->duration.mean() > b.item->duration.mean();
				});
			}
			
			std::vector<Named> result;
			result.reserve(ptrResult.size());
			for (auto &p : ptrResult) {
				result.emplace_back(Named{*p.name, *p.item});
			}
			return result;
		}
		
		template<class Fn>
		void forEach(Fn &&fn, bool lengthSort=false, size_t depth=0) const {
			for (auto &namedItem : named(lengthSort)) {
				fn(namedItem.name, namedItem.item, depth);
				namedItem.item.subReport.forEach(fn, lengthSort, depth + 1);
			}
		}

		void log(bool longestFirst=false) const {
			std::vector<double> refSeconds;
			size_t indent = 0;
			size_t prevDepth = 0;
			forEach([&](auto &name, auto &item, size_t depth) {
				for (size_t i = 0; i + 2 < depth; ++i) std::cout << " | ";
				if (depth > 0) std::cout << " \\__ ";
				prevDepth = depth;
				refSeconds.resize(depth);

				double itemRefSeconds = item.refSeconds.sum;
				if (!itemRefSeconds && !refSeconds.empty()) itemRefSeconds = refSeconds.back();
				refSeconds.push_back(itemRefSeconds);
				if (itemRefSeconds) {
					double ratio = item.duration.sum/itemRefSeconds;
					std::printf("%#5.2f%% ", ratio*100);
				}
				std::cout << name << "\n";
			}, longestFirst);
		}
	};
	struct ReportItem {
		Stats start, duration;
		Stats refSeconds;
		
		Report subReport;
	};
	Report report;

	void reset() {
		report.clear();
		depth = 0;
	}

	void update(double averagePeriod=-1) {
		for (auto &list : eventLists) {
			if (list.mayContainEvents() && list.claimFromReport()) {
				updateFromEvents(list.events.data(), list.filledToIndex, averagePeriod);
				list.releaseFromReport();
			}
		}
	}
	
	std::string filePerfettoJson;
private:
	using EventListSet = _timemonitor_impl::EventListSet;
	using EventList = _timemonitor_impl::EventList;
	using Event = _timemonitor_impl::Event;
	EventListSet eventLists; // fixed size, must not be reallocated after construction
	
	ReportItem & getCurrentItem() {
		ReportItem *ri = nullptr;
		for (auto &scopeName : scopedNames) {
			Report &rep = (ri ? ri->subReport : report);
			auto &item = rep.items[scopeName];
			ri = &item;
		}
		return *ri;
	}
	
	// Time that the latest top-level scope was opened
	size_t depth = 0;
	std::vector<double> scopedTimes;
	std::vector<std::string> scopedNames;
		
	void processEvent(const Event &event) {
		scopedNames.resize(depth);
		scopedTimes.resize(depth);

		auto eventTime = event.time.seconds();

		if (event.label[0] == '\0') { // empty string
			if (depth == event.depth + 1) {
				auto startTime = scopedTimes.back();
				auto duration = eventTime - startTime;
				auto &item = getCurrentItem();
				item.start.add(startTime);
				item.duration.add(duration);

				--depth;
				scopedNames.pop_back();
				scopedTimes.pop_back();
			}
			return;
		}

		// If it's not increasing the depth, then it's an unscoped event
		if (depth == event.depth) {
			scopedNames.emplace_back(event.label);
			auto &item = getCurrentItem();
			item.start.add(eventTime);
			scopedNames.pop_back();
			return;
		}

		if (depth + 1 == event.depth) { // Opening an event scope
			scopedNames.emplace_back(event.label);
			scopedTimes.emplace_back(eventTime);

			auto &item = getCurrentItem();
			if (event.refSeconds > 0) item.refSeconds.add(event.refSeconds);
			++depth;
		}
	}
	
	void decayAndPrune(Report &report, double ratio, double pruneLimit) {
		for (auto iter = report.items.begin(); iter != report.items.end(); ) {
			auto &item = iter->second;
			if (item.start.count < pruneLimit) {
				iter = report.items.erase(iter);
				continue;
			}
			item.start.decay(ratio);
			item.duration.decay(ratio);
			item.refSeconds.decay(ratio);
			decayAndPrune(item.subReport, ratio, pruneLimit);
		}
	}

	void updateFromEvents(Event *events, size_t eventCount, double averagePeriod) {
		if (!eventCount) return;
		createOrAppendPerfettoJson(events, eventCount);

		for (size_t i = 0; i < eventCount; ++i) {
			processEvent(events[i]);
		}

		// Check maximum reference time and use it for moving-average decay
		if (depth == 0 && averagePeriod > 0) {
			double maxPeriod = 0;
			for (auto &pair : report.items) {
				auto &item = pair.second;
				if (item.refSeconds.sum > maxPeriod) maxPeriod = item.refSeconds.sum;
			}
			if (maxPeriod > averagePeriod) {
				decayAndPrune(report, averagePeriod/maxPeriod, 0.1);
			}
		}
	}

	void createOrAppendPerfettoJson(Event *events, size_t eventCount) {
		if (!filePerfettoJson.empty()) {
			std::ofstream output(filePerfettoJson, std::ios::binary | std::ios::ate);
			if (output) {
				if (output.tellp() == 0) {
					output << "[\n";
				} else {
					output.seekp(-1, std::ios_base::end); // skip the closing `]` (if there is one)
					output << ",\n";
				}
				for (size_t i = 0; i < eventCount; ++i) {
					if (i > 0) output << ",\n";
					auto &event = events[i];
					auto ns = static_cast<unsigned long long>(std::round(event.time.seconds()*1e6));
					if (event.label[0] != '\0') {
						output << "{\"ph\":\"B\",\"pid\":0,\"tid\":0,\"ts\":" << ns << ",\"name\":\"" << event.label << "\"}";
					} else {
						output << "{\"ph\":\"E\",\"pid\":0,\"tid\":0,\"ts\":" << ns << "}";
					}
				}
				output << "]";
			} else {
				std::cerr << "Failed to write Perfetto JSON: " << filePerfettoJson << std::endl;
			}
			output.flush();
		}
	}
};

} // namespace
