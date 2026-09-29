#include "mdLib/mddigipro.h"
#include "mdLib/mdsysexfile.h"
#include "sysexContentOracle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace
{
	using md::test::Sysex;

	void require(const bool _condition, const char* _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	std::vector<int32_t> decodePoints(const Sysex& _message)
	{
		const auto raw = md::test::unpackElektron(_message.begin() + 14, _message.end() - 5);
		std::vector<int32_t> points;
		for(size_t i = 0; i + 2 < raw.size(); i += 3)
		{
			const auto value = (uint32_t(raw[i]) << 16) | (uint32_t(raw[i + 1]) << 8) | raw[i + 2];
			points.push_back(int32_t(value ^ 0x800000u) - 0x800000);
		}
		return points;
	}

	std::vector<float> sine(const size_t _length)
	{
		std::vector<float> cycle(_length);
		for(size_t i = 0; i < _length; ++i)
			cycle[i] = static_cast<float>(0.5 * std::sin(2.0 * 3.14159265358979323846 * double(i) / double(_length)));
		return cycle;
	}

	void selfTest()
	{
		const auto points = md::makeDigiProPoints(sine(300));
		require(points.size() == md::g_digiProPointCount, "point count");
		int32_t peak = 0;
		for(size_t i = 0; i < 1024; ++i)
			peak = std::max(peak, std::abs(points[i]));
		require(std::abs(peak - 795169) <= 10, "full-resolution level");
		// A sine keeps only its fundamental: the 4-point resolution is 0, +a, 0, -a.
		const auto* last = points.data() + md::g_digiProPointCount - 4;
		require(last[0] == 0 && last[2] == 0 && last[1] > 795169 && last[3] == -last[1], "4-point resolution");

		const auto message = md::makeDigiProMessage(45, "sine", points);
		require(message.size() == md::g_digiProMessageSize, "message size");
		std::vector<md::MidiSysexMessage> parsed;
		require(md::parseMidiSysexFile(message, md::MachineModel::Monomachine, &parsed)
			== md::MidiSysexStreamValidation::Valid, "parser rejects message");
		require(parsed.size() == 1 && parsed[0].kind == md::MidiSysexMessageKind::DigiPro, "not a DigiPRO message");
		require(message[9] == 45 && std::string(message.begin() + 10, message.begin() + 14) == "SINE", "slot or name");
		require(decodePoints(message) == points, "points do not round-trip");

		require(md::makeDigiProName("MG12") == "MG12", "name MG12");
		require(md::makeDigiProName("waftwaveevolvesine64") == "WA64", "name with number");
		require(md::makeDigiProName("Wave 7") == "WAV7", "name with space");
		require(md::makeDigiProName("x") == "X   ", "short name");
		require(md::makeDigiProName("pwm_sine") == "PWMS", "long name");

		require(md::makeDigiProPoints({0.5f}).empty(), "accepted a 1-sample cycle");
		require(md::makeDigiProPoints(std::vector<float>(256, 0.0f)).empty(), "accepted silence");
		require(md::makeDigiProPoints(std::vector<float>(256, 0.3f)).empty(), "accepted DC");
		require(md::makeDigiProPoints(std::vector<float>(md::g_digiProMaxCycleLength + 1, 0.1f)).empty(), "accepted overlong cycle");
		std::puts("DigiPRO self test passed");
	}

	// Minimal 16-bit PCM WAV reader, mono mixdown.
	std::vector<float> readWav(const std::string& _path)
	{
		std::ifstream file(_path, std::ios::binary);
		const Sysex bytes((std::istreambuf_iterator<char>(file)), {});
		if(bytes.size() < 12) return {};
		const auto u16 = [&](size_t i) { return uint32_t(bytes[i]) | (uint32_t(bytes[i + 1]) << 8); };
		const auto u32 = [&](size_t i) { return u16(i) | (u16(i + 2) << 16); };
		uint32_t channels = 0, bits = 0;
		for(size_t pos = 12; pos + 8 <= bytes.size();)
		{
			const std::string id(bytes.begin() + pos, bytes.begin() + pos + 4);
			const auto size = u32(pos + 4);
			if(id == "fmt ") { channels = u16(pos + 10); bits = u16(pos + 22); }
			if(id == "data" && channels && bits == 16)
			{
				std::vector<float> samples;
				for(size_t i = pos + 8; i + 2 * channels <= pos + 8 + size && i + 2 * channels <= bytes.size(); i += 2 * channels)
				{
					float sum = 0;
					for(uint32_t c = 0; c < channels; ++c)
						sum += float(int16_t(u16(i + 2 * c))) / 32768.0f;
					samples.push_back(sum / float(channels));
				}
				return samples;
			}
			pos += 8 + size + (size & 1);
		}
		return {};
	}

	// Compares conversions of a published bank's source WAVs with the bank.
	int compareBank(const std::string& _bank, const std::string& _wavFolder)
	{
		std::ifstream file(_bank, std::ios::binary);
		const Sysex bytes((std::istreambuf_iterator<char>(file)), {});
		double worst = 0;
		for(const auto& message : md::test::splitSysex(bytes))
		{
			const std::string name(message.begin() + 10, message.begin() + 14);
			auto wav = readWav(_wavFolder + "/" + name + ".WAV");
			if(wav.empty()) wav = readWav(_wavFolder + "/" + name + ".wav");
			if(wav.empty()) { std::printf("%s: no WAV\n", name.c_str()); continue; }
			const auto reference = decodePoints(message);
			const auto converted = md::makeDigiProPoints(wav);
			int32_t referencePeak = 0, convertedPeak = 0;
			for(size_t i = 0; i < 1024; ++i)
			{
				referencePeak = std::max(referencePeak, std::abs(reference[i]));
				convertedPeak = std::max(convertedPeak, std::abs(converted[i]));
			}
			// Each resolution's worst difference, relative to its own peak or, for
			// resolutions the wave barely reaches, to 1% of the full cycle's.
			double levelWorst = 0;
			size_t offset = 0;
			for(size_t size = 1024; size >= 4; offset += size, size /= 2)
			{
				const double gain = double(referencePeak) / convertedPeak;
				double peak = 0, error = 0;
				for(size_t i = 0; i < size; ++i)
				{
					peak = std::max(peak, std::abs(double(reference[offset + i])));
					error = std::max(error, std::abs(reference[offset + i] - gain * converted[offset + i]));
				}
				levelWorst = std::max(levelWorst, error / std::max(peak, referencePeak * 0.01));
			}
			std::printf("%s slot=%2u worstShapeError=%.2f%% level=%+.2f dB\n", name.c_str(), message[9], 100 * levelWorst,
				20 * std::log10(double(convertedPeak) / referencePeak));
			worst = std::max(worst, levelWorst);
		}
		std::printf("worst shape error %.2f%%\n", 100 * worst);
		return 0;
	}
}

int main(int argc, char** argv)
{
	try
	{
		// --make <bank.syx> <wave.wav>...: one wave per file, from slot 1.
		if(argc >= 4 && std::string(argv[1]) == "--make")
		{
			Sysex bank;
			for(int i = 3; i < argc && i - 3 < int(md::g_digiProSlotCount); ++i)
			{
				const auto points = md::makeDigiProPoints(readWav(argv[i]));
				require(!points.empty(), "unreadable or silent WAV");
				std::string name = argv[i];
				name = name.substr(name.find_last_of('/') + 1);
				name = name.substr(0, name.find_last_of('.'));
				const auto message = md::makeDigiProMessage(uint8_t(i - 3), name, points);
				bank.insert(bank.end(), message.begin(), message.end());
			}
			std::ofstream(argv[2], std::ios::binary).write(reinterpret_cast<const char*>(bank.data()), std::streamsize(bank.size()));
			std::printf("wrote %d waves\n", std::min(argc - 3, int(md::g_digiProSlotCount)));
			return 0;
		}
		if(argc == 3)
			return compareBank(argv[1], argv[2]);
		selfTest();
		return 0;
	}
	catch(const std::exception& error)
	{
		std::fprintf(stderr, "%s\n", error.what());
		return 1;
	}
}
