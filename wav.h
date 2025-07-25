#pragma once

#include <vector>
#include <iostream>
#include <fstream>
#include <cstring>

class Wav {
	// text values interpreted as little-endian ints
	uint32_t value_RIFF = 0x46464952;
	uint32_t value_WAVE = 0x45564157;
	uint32_t value_fmt = 0x20746d66;
	uint32_t value_data = 0x61746164;

	// Little-endian read/write methods
	static uint32_t read16(std::istream& in) {
		unsigned char a[2];
		in.read((char*)a, sizeof(a));
		return ((uint32_t)a[0]) + ((uint32_t)a[1])*256;
	}
	static uint32_t read24(std::istream& in) {
		unsigned char a[3];
		in.read((char*)a, sizeof(a));
		return ((uint32_t)a[0]&0xff) + ((uint32_t)a[1])*256 + ((uint32_t)a[2])*65536;
	}
	static uint32_t read32(std::istream& in) {
		unsigned char a[4];
		in.read((char*)a, sizeof(a));
		return ((uint32_t)a[0]&0xff) + ((uint32_t)a[1])*256 + ((uint32_t)a[2])*65536 + ((uint32_t)a[3])*256*65536;
	}
	
	static void write16(std::ostream& out, uint16_t value) {
		char a[2] = {(char)(value>>0), (char)(value>>8)};
		out.write(a, sizeof(a));
	}
	static void write24(std::ostream& out, uint32_t value) {
		char a[3] = {(char)(value>>0), (char)(value>>8), (char)(value>>16)};
		out.write(a, sizeof(a));
	}
	static void write32(std::ostream& out, uint32_t value) {
		char a[4] = {(char)(value>>0), (char)(value>>8), (char)(value>>16), (char)(value>>24)};
		out.write(a, sizeof(a));
	}

public:
	struct Result {
		enum class Code {
			OK = 0,
			IO_ERROR,
			FORMAT_ERROR,
			UNSUPPORTED,
			WEIRD_CONFIG
		};
		Code code = Code::OK;
		std::string reason;
		
		Result(Code code, std::string reason="") : code(code), reason(reason) {};
		// Used to neatly test for success
		explicit operator bool () const {
			return code == Code::OK;
		};
		const Result & warn(std::ostream& output=std::cerr) const {
			if (!(bool)*this) {
				output << "WAV error: " << reason << std::endl;
			}
			return *this;
		}
	};
	
	size_t sampleRate = 48000;
	size_t channels = 1, offset = 0;
	std::vector<double> samples;
	size_t length() const {
		return samples.size()/channels - offset;
	}
	void resize(size_t length) {
		samples.resize((offset + length)*channels, 0);
	}
	template<bool isConst>
	class ChannelReader {
		using CSample = typename std::conditional<isConst, const double, double>::type;
		CSample *data;
		size_t stride;
	public:
		ChannelReader(CSample *samples, size_t channels) : data(samples), stride(channels) {}
		
		CSample & operator [](size_t i) {
			return data[i*stride];
		}
	};
	ChannelReader<false> operator [](size_t c) {
		return ChannelReader<false>(samples.data() + offset*channels + c, channels);
	}
	ChannelReader<true> operator [](size_t c) const {
		return ChannelReader<true>(samples.data() + offset*channels + c, channels);
	}
	
	Result result = Result(Result::Code::OK);

	Wav() {}
	Wav(double sampleRate, size_t channels) : sampleRate(sampleRate), channels(channels) {}
	Wav(double sampleRate, size_t channels, const std::vector<double> &samples) : sampleRate(sampleRate), channels(channels), samples(samples) {}
	Wav(std::string filename) {
		result = read(filename).warn();
	}
	
	enum class Format {
		invalid,
		int16,
		int24,
		float32
	};
	static constexpr Format formatInvalid = Format::invalid;
	static constexpr Format formatInt16 = Format::int16;
	static constexpr Format formatInt24 = Format::int24;
	static constexpr Format formatFloat32 = Format::float32;

	Format getFormat(uint16_t format, uint16_t bits) const {
		if (format == 1/*PCM*/) {
			if (bits == 16) return formatInt16;
			if (bits == 24) return formatInt24;
		} else if (format == 3/*float*/) {
			if (bits == 32) return formatFloat32;
		}
		return formatInvalid;
	}
	
	Result read(std::string filename) {
		std::ifstream file;
		file.open(filename, std::ios::binary);
		if (!file.is_open()) return result = Result(Result::Code::IO_ERROR, "Failed to open file: " + filename);

		// RIFF chunk
		if (read32(file) != value_RIFF) return result = Result(Result::Code::FORMAT_ERROR, "Input is not a RIFF file");
		read32(file); // File length - we don't check this
		if (read32(file) != value_WAVE) return result = Result(Result::Code::FORMAT_ERROR, "Input is not a plain WAVE file");
		
		auto blockStart = file.tellg(); // start of the blocks - we will seek back to here periodically
		bool hasFormat = false, hasData = false;
		
		Format format = Format::invalid; // Shouldn't matter, we should always read the `fmt ` chunk before `data`
		while (!file.eof()) {
			auto blockType = read32(file), blockLength = read32(file);
			if (file.eof()) break;
			if (!hasFormat && blockType == value_fmt) {
				auto formatInt = read16(file);
				format = (Format)formatInt;
				channels = read16(file);
				if (channels < 1) return result = Result(Result::Code::FORMAT_ERROR, "Cannot have zero channels");
				
				sampleRate = read32(file);
				if (sampleRate < 1) return result = Result(Result::Code::FORMAT_ERROR, "Cannot have zero sampleRate");

				auto expectedBytesPerSecond = read32(file);
				auto bytesPerFrame = read16(file);
				auto bitsPerSample = read16(file);
				format = getFormat(formatInt, bitsPerSample);
				if (!format == formatInvalid) return result = Result(Result::Code::UNSUPPORTED, "Unsupported format:bits: " + std::to_string(formatInt) + ":" + std::to_string(bitsPerSample));
				// Since it's plain WAVE, we can do some extra checks for consistency
				if (bitsPerSample*channels != bytesPerFrame*8) return result = Result(Result::Code::FORMAT_ERROR, "Format sizes don't add up");
				if (expectedBytesPerSecond != sampleRate*bytesPerFrame) return result = Result(Result::Code::FORMAT_ERROR, "Format sizes don't add up");

				hasFormat = true;
				file.clear();
				file.seekg(blockStart);
			} else if (hasFormat && blockType == value_data) {
				std::vector<double> samples(0);
				if (format == formatInt16) {
					samples.reserve(blockLength/2);
					for (size_t i = 0; i < blockLength/2; ++i) {
						uint16_t value = read16(file);
						if (file.eof()) break;
						if (value >= 32768) {
							samples.push_back(((double)value - 65536)/32768);
						} else {
							samples.push_back((double)value/32768);
						}
					}
				} else if (format == formatInt24) {
					samples.reserve(blockLength/3);
					for (size_t i = 0; i < blockLength/3; ++i) {
						uint32_t value = read24(file);
						if (file.eof()) break;
						if (value >= 8388608) {
							samples.push_back(((double)value - 16777216)/8388608);
						} else {
							samples.push_back((double)value/8388608);
						}
					}
				} else if (format == formatFloat32) {
					samples.reserve(blockLength/4);
					for (size_t i = 0; i < blockLength/4; ++i) {
						uint32_t intValue = read32(file);
						if (file.eof()) break;
						float floatValue;
						static_assert(sizeof(float) == sizeof(uint32_t), "floats must be 32-bit");
						std::memcpy(&floatValue, &intValue, sizeof(float));
						samples.push_back(floatValue);
					}
				}
				while (samples.size()%channels != 0) {
					samples.push_back(0);
				}
				this->samples = samples;
				offset = 0;
				hasData = true;
			} else {
				// We either don't recognise
				file.ignore(blockLength);
			}
		}
		if (!hasFormat) return result = Result(Result::Code::FORMAT_ERROR, "missing `fmt ` block");
		if (!hasData) return result = Result(Result::Code::FORMAT_ERROR, "missing `data` block");
		return result = Result(Result::Code::OK);
	}
	
	Result write(std::string filename, Format format=formatInt16) {
		if (channels == 0 || channels > 65535) return result = Result(Result::Code::WEIRD_CONFIG, "Invalid channel count");
		if (sampleRate <= 0 || sampleRate > 0xFFFFFFFFu) return result = Result(Result::Code::WEIRD_CONFIG, "Invalid sample rate");
		
		std::ofstream file;
		file.open(filename, std::ios::binary);
		if (!file.is_open()) return result = Result(Result::Code::IO_ERROR, "Failed to open file: " + filename);
		
		int formatCode = 0;
		int bytesPerSample = 0;
		if (format == formatInt16) {
			formatCode = 1;
			bytesPerSample = 2;
		} else if (format == formatInt24) {
			formatCode = 1;
			bytesPerSample = 3;
		} else if (format == formatFloat32) {
			formatCode = 3;
			bytesPerSample = 4;
		} else {
			return result = Result(Result::Code::FORMAT_ERROR, "invalid format");
		}
		
		// File size - 44 bytes is RIFF header, "fmt" block, and "data" block header
		unsigned int dataLength = (samples.size() - offset*channels)*bytesPerSample;
		unsigned int fileLength = 44 + dataLength;

		// RIFF chunk
		write32(file, value_RIFF);
		write32(file, fileLength - 8); // File length, excluding the RIFF header
		write32(file, value_WAVE);
		// "fmt " block
		write32(file, value_fmt);
		write32(file, 16); // block length
		write16(file, (uint16_t)format);
		write16(file, channels);
		write32(file, sampleRate);
		unsigned int expectedBytesPerSecond = sampleRate*channels*bytesPerSample;
		write32(file, expectedBytesPerSecond);
		write16(file, channels*bytesPerSample); // Bytes per frame
		write16(file, bytesPerSample*8); // bits per sample
		
		// "data" block
		write32(file, value_data);
		write32(file, dataLength);
		switch (format) {
		case formatInt16:
			for (unsigned int i = offset*channels; i < samples.size(); i++) {
				double value = samples[i]*32768;
				if (value > 32767) value = 32767;
				if (value <= -32768) value = -32768;
				if (value < 0) value += 65536;
				write16(file, (uint16_t)value);
			}
			break;
		case formatInt24:
			for (unsigned int i = offset*channels; i < samples.size(); i++) {
				double value = samples[i]*8388608;
				if (value > 8388607) value = 8388607;
				if (value <= -8388608) value = -8388608;
				if (value < 0) value += 16777216;
				write24(file, (uint32_t)value);
			}
			break;
		case formatFloat32:
			for (unsigned int i = offset*channels; i < samples.size(); i++) {
				float floatValue = float(samples[i]);
				uint32_t intValue;
				std::memcpy(&intValue, &floatValue, sizeof(float));
				write32(file, intValue);
			}
			break;
		}
		return result = Result(Result::Code::OK);
	}
	
	void makeMono() {
		std::vector<double> newSamples(samples.size()/channels, 0);
		
		for (size_t channel = 0; channel < channels; ++channel) {
			for (size_t i = 0; i < newSamples.size(); ++i) {
				newSamples[i] += samples[i*channels + channel];
			}
		}
		for (size_t i = 0; i < newSamples.size(); ++i) {
			newSamples[i] /= channels;
		}
		
		channels = 1;
		samples = newSamples;
	}
	
	void normalise(bool reduceOnly=false, double absLevel=0.9999) {
		double maxAbs = (reduceOnly ? absLevel : 1e-30);
		for (size_t i = offset*channels; i < samples.size(); ++i) {
			auto a = std::abs(samples[i]);
			if (a > maxAbs) maxAbs = a;
		}
		if (maxAbs > absLevel) {
			double gain = absLevel/maxAbs;
			for (auto &s : samples) s *= gain;
		}
	}
};
