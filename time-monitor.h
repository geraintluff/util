#pragma once

#include <atomic>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <iostream>

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
			if (writerEvent <= events.data() + events.size()) {
				// Only keep event sequence if it's complete
				filledToIndex = size_t(writerEvent - events.data());
			} else {
				wantsToGrow = true;
			}
			claimed = false;
		}

		bool mayContainEvents() const { // Doesn't guarantee it *can* be claimed (or have anything to process) but avoids claiming unnecessarily
			return (filledToIndex > 0) && !claimed;
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
	size_t depth = 0;

	EventList *claimedList = nullptr;
	Event *events = nullptr;
	Event *eventsEnd = nullptr;

	inline void tryClaim() {
		events = eventsEnd = nullptr;
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
		if (depth == 0) tryClaim();
		++depth;
		if (events && events < eventsEnd) {
			*events = {depth, name, CpuTime::now(), refSeconds};
		}
		++events;
	}
	inline void leave() {
		--depth;
		if (events && events < eventsEnd) {
			*events = {depth, "", CpuTime::now()};
		}
		++events;
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

	struct ReportItem {
		size_t depth;
		std::string name;

		Stats start, duration;
		Stats refSeconds;
	};

	void reset() {
		itemMap.clear();
		depth = 0;
	}

	std::vector<ReportItem> items() const {
		std::vector<ReportItem> result;
		for (auto &pair : itemMap) {
			result.emplace_back(pair.second);
		}
		// Inherit reference times
		for (size_t i = 0; i < result.size(); ++i) {
			for (size_t j = 0; j < result.size(); ++j) {
				if (i == j) continue;
				auto &a = result[i], &b = result[j];
				if (a.name.size() > b.name.size() && a.name.substr(0, b.name.size()) == b.name) {
					if (!a.refSeconds.mean() && b.refSeconds.mean() > 0) {
						a.refSeconds = b.refSeconds;
					}
				}
			}
		}
		std::sort(result.begin(), result.end(), [](const ReportItem &a, const ReportItem &b){
			return a.start.mean() < b.start.mean();
		});
		return result;
	}
	
	void log(std::ostream &output=std::cout, bool includeTrivial=false) const {
		for (auto &item : items()) {
			if (!includeTrivial && item.duration.count < 0.001) continue;
			
			output << item.name;
//				output << " @ " << item.start.mean();
			
			if (item.refSeconds.sum > 0 && item.duration.sum > 0) {
				double ratio = item.duration.sum/item.refSeconds.sum;
				output << "\t" << (ratio*100) << "%";
			}
			output << "\n";
		}
	}

	void update(double averagePeriod=-1) {
		for (auto &list : eventLists) {
			if (list.mayContainEvents() && list.claimFromReport()) {
				updateFromEvents(list.events.data(), list.filledToIndex, averagePeriod);
				list.releaseFromReport();
			}
		}
	}
private:
	using EventListSet = _timemonitor_impl::EventListSet;
	using EventList = _timemonitor_impl::EventList;
	using Event = _timemonitor_impl::Event;
	EventListSet eventLists; // fixed size, must not be reallocated after construction

	std::unordered_map<std::string, ReportItem> itemMap;
	ReportItem & addToStats(const std::string &name, double start) {
		auto &item = itemMap[name];
		item.start.add(start);
		return item;
	}
	void addToStats(const std::string &name, double start, double duration) {
		auto &item = addToStats(name, start);
		item.duration.add(duration);
	}
	
	// Time that the latest top-level scope was opened
	CpuTime refTime;
	size_t depth = 0;
	std::vector<double> scopedTimes;
	std::vector<std::string> scopedNames;
	void resetEventHandling() {
		depth = 0;
		scopedTimes.resize(0);
		scopedNames.resize(0);
	}
	
	void processEvent(const Event &event) {
		scopedNames.resize(depth);
		scopedTimes.resize(depth);

		auto pushName = [&](const char *label){
			if (scopedNames.empty()) {
				scopedNames.emplace_back(label);
			} else {
				scopedNames.emplace_back(scopedNames.back() + " / " + label);
			}
		};

		auto eventTime = (event.time - refTime).seconds();

		if (event.label[0] == '\0') {
			if (depth == event.depth + 1) {
				auto startTime = scopedTimes.back();
				auto duration = eventTime - startTime;
				addToStats(scopedNames.back(), startTime, duration);

				--depth;
				scopedNames.pop_back();
				scopedTimes.pop_back();
			}
			return;
		}

		// If it's not increasing the depth, then it's an unscoped event
		if (depth == event.depth) {
			pushName(event.label);
			addToStats(scopedNames.back(), eventTime);
			scopedNames.pop_back();
			return;
		}

		// update the reference time only when at top-level scope
		if (depth == 0) {
			refTime = event.time;
			eventTime = (event.time - refTime).seconds();
		}

		if (depth + 1 == event.depth) { // Opening an event scope
			scopedTimes.emplace_back(eventTime);
			pushName(event.label);

			auto &item = itemMap[scopedNames.back()];
			if (item.name.empty()) item.name = scopedNames.back();
			item.depth = depth;
			if (event.refSeconds > 0) item.refSeconds.add(event.refSeconds);
			++depth;
		}
	}

	void updateFromEvents(Event *events, size_t eventCount, double averagePeriod) {
//		resetEventHandling();
		for (size_t i = 0; i < eventCount; ++i) {
			processEvent(events[i]);
		}

		// Check maximum reference time and use it for moving-average decay
		if (depth == 0 && averagePeriod > 0) {
			double maxPeriod = 0;
			for (auto &pair : itemMap) {
				auto &item = pair.second;
				if (item.refSeconds.sum > maxPeriod) maxPeriod = item.refSeconds.sum;
			}
			averagePeriod *= 0.36788;
			if (maxPeriod > averagePeriod) {
				double decay = averagePeriod/maxPeriod;
				for (auto &pair : itemMap) {
					auto &item = pair.second;
					item.start.decay(decay);
					item.duration.decay(decay);
					item.refSeconds.decay(decay);
				}
			}
		}
	}
};

} // namespace
