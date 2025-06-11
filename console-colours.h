#pragma once

#include <cstdlib>
#include <cstring>

namespace Console {
	static const struct ConsoleColourChecker {
		ConsoleColourChecker() {
			auto *envTerm = std::getenv("TERM");
			supported = envTerm && std::strcmp(envTerm, "dumb");
		}
		operator bool() const {
			return supported;
		}
	private:
		bool supported;
	} colours;

	static const char *Reset = colours ? "\x1b[0m" : "";
	static const char *Bright = colours ? "\x1b[1m" : "";
	static const char *Dim = colours ? "\x1b[2m" : "";
	static const char *Underscore = colours ? "\x1b[4m" : "";
	static const char *Blink = colours ? "\x1b[5m" : "";
	static const char *Reverse = colours ? "\x1b[7m" : "";
	static const char *Hidden = colours ? "\x1b[8m" : "";

	namespace Foreground {
		static const char *Black = colours ? "\x1b[30m" : "";
		static const char *Red = colours ? "\x1b[31m" : "";
		static const char *Green = colours ? "\x1b[32m" : "";
		static const char *Yellow = colours ? "\x1b[33m" : "";
		static const char *Blue = colours ? "\x1b[34m" : "";
		static const char *Magenta = colours ? "\x1b[35m" : "";
		static const char *Cyan = colours ? "\x1b[36m" : "";
		static const char *White = colours ? "\x1b[37m" : "";
	}

	namespace Background {
		static const char *Black = colours ? "\x1b[40m" : "";
		static const char *Red = colours ? "\x1b[41m" : "";
		static const char *Green = colours ? "\x1b[42m" : "";
		static const char *Yellow = colours ? "\x1b[43m" : "";
		static const char *Blue = colours ? "\x1b[44m" : "";
		static const char *Magenta = colours ? "\x1b[45m" : "";
		static const char *Cyan = colours ? "\x1b[46m" : "";
		static const char *White = colours ? "\x1b[47m" : "";
	}

	using namespace Foreground;
}
