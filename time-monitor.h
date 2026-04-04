#pragma once

#include <atomic>
#include <vector>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <iostream>

// We want CPU time, not wall-clock time, so we can't use `std::chrono::high_resolution_clock`
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
		return *this;
	}
public:
	using Time = timespec;
	
	static CpuTime now() {
		CpuTime result;
		auto errorCode = clock_gettime(CLOCK_MONOTONIC, &result.time); // CLOCK_THREAD_CPUTIME_ID seems better, but it's slow
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
// Fallback, using C stdlib
#	include <ctime>
namespace signalsmith {
struct CpuTime {
	using Time = std::clock_t;

	static CpuTime now() {
		return {std::clock()};
	}
	double seconds() const {
		return time/double(CLOCKS_PER_SEC);
	}
	
	CpuTime operator+(const CpuTime &other) const {
		return {time + other.time};
	}
	CpuTime operator-(const CpuTime &other) const {
		return {time - other.time};
	}
#endif

	CpuTime() {}

	Time time;
	CpuTime(Time time) : time(time) {}
};

struct TimeMonitor {
	TimeMonitor(size_t initialSize=256) : maxEvents(initialSize) {
		eventStorage.resize(maxEvents);
		zeroEventsList(eventStorage);
		eventIndex = 0;
		events = eventStorage.data();
	}
	
	inline void mark(const char *name) {
		if (eventIndex < maxEvents) {
			events[eventIndex] = {depth, name, CpuTime::now()};
			++eventIndex;
		}
	}

	struct Scoped {
		// No copy/move/etc.
		Scoped(const Scoped &other) = delete;
		Scoped(Scoped &&other) = delete;
		
		~Scoped() {
			monitor.leave();
		}
		
		void replace(const char *newLabel, double refSeconds=0) {
			monitor.leave();
			label = newLabel;
			monitor.enter(label, refSeconds);
		}
		
		Scoped scoped(const char *newLabel, double refSeconds=0) {
			return Scoped(monitor, newLabel, refSeconds);
		}
	private:
		TimeMonitor &monitor;
		const char *label;

		friend struct TimeMonitor;
		Scoped(TimeMonitor &monitor, const char *label, double refSeconds) : monitor(monitor), label(label) {
			monitor.enter(label, refSeconds);
		}
	};
	Scoped scoped(const char *label, double refSeconds=0) {
		return Scoped{*this, label, refSeconds};
	}

	struct Event {
		size_t depth;
		const char *label;
		CpuTime time;
		double refSeconds;
		
		bool valid() const {
			return label != nullptr;
		}
	};

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

	struct Report {
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
	private:
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
	
		friend struct TimeMonitor;
		void update(std::vector<Event> &events, double averagePeriod) {
			for (auto &event : events) processEvent(event);

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

	const Report & collect(double averagePeriod=-1) {
		size_t reportLength = eventIndex;
		
		bool invalid = (reportLength >= maxEvents);
		
		size_t newMaxEvents = maxEvents;
		if (reportLength >= maxEvents/2) {
			newMaxEvents *= 2;
		}
		std::vector<Event> reportStorage(newMaxEvents);
		zeroEventsList(reportStorage);
		reportStorage.swap(eventStorage);
		
		events = eventStorage.data(); // point to new data
		size_t swapLength = eventIndex.exchange(0); // reset index
		// It's possible that some events were written to the new location at the old index, in between those two statements
		for (size_t i = reportLength; i < swapLength; ++i) {
			if (eventStorage[i].valid()) {
				reportStorage[i] = eventStorage[i]; // copy to the old location
			}
		}
		// And then finally, check that the new location hasn't already filled up enough that it could've overlapped with the copying we just did
		if (eventIndex >= reportLength) {
			invalid = true;
		} else {
			reportLength = swapLength;
		}

		// Only at this point, tell the monitor it has more space for events
		maxEvents = newMaxEvents;
		
		reportStorage.resize(reportLength);
		for (auto &e : reportStorage) {
			if (!e.valid()) {
				invalid = true;
				break;
			}
		}
		if (!invalid) {
			report.update(reportStorage, averagePeriod);
		} else {
			report.reset();
		}
		return report;
	}
	
private:
	size_t depth = 0;

	size_t maxEvents = 0;
	std::atomic<size_t> eventIndex;
	std::atomic<Event *> events;

	std::vector<Event> eventStorage; // only updated in `.report()`

	void zeroEventsList(std::vector<Event> &list) {
		for (auto &e : list) e = {0, nullptr, {}, 0};
	}

	inline void enter(const char *name, double refSeconds) {
		++depth;
		if (eventIndex < maxEvents) {
			auto now = CpuTime::now();
			events[eventIndex] = {depth, name, now, refSeconds};
			++eventIndex;
		}
	}
	inline void leave() {
		--depth;
		if (eventIndex < maxEvents) {
			// An event with an empty label indicates the end of a previously-opened scope
			events[eventIndex] = {depth, "", CpuTime::now()};
			++eventIndex;
		}
	}
	
	Report report;
};

} // namespace
