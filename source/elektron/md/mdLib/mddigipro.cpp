#include "mddigipro.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <complex>

namespace md
{
	namespace
	{
		constexpr double g_pi = 3.14159265358979323846;

		// Level of the full-resolution cycle, matching published DigiPRO banks
		// (about -20.5 dBFS, leaving headroom for the emphasis below).
		constexpr double g_cyclePeak = 795169.0;

		// Published banks lift each lower resolution's upper harmonics by the
		// inverse of this 5-tap response, measured from those banks. x is the
		// harmonic number divided by the resolution's point count. The lift
		// compounds from one resolution to the next.
		double emphasis(const double _x)
		{
			constexpr double c0 = 0.38022, c1 = 0.20202, c2 = 0.10754;
			constexpr double dc = c0 + 2.0 * c1 + 2.0 * c2;	// unity gain at DC
			return dc / (c0 + 2.0 * c1 * std::cos(g_pi * _x) + 2.0 * c2 * std::cos(2.0 * g_pi * _x));
		}

		int32_t toSample(const double _value)
		{
			constexpr double limit = 8388607.0;
			return static_cast<int32_t>(std::lround(std::clamp(_value, -limit, limit)));
		}

		// Sums harmonics 1.._harmonics.size() at _points evenly spaced phases.
		std::vector<double> synthesize(const std::vector<std::complex<double>>& _harmonics,
			const size_t _points)
		{
			std::vector<double> values(_points);
			for(size_t i = 0; i < _points; ++i)
			{
				for(size_t h = 0; h < _harmonics.size(); ++h)
				{
					const double phase = 2.0 * g_pi * static_cast<double>(((h + 1) * i) % _points)
						/ static_cast<double>(_points);
					values[i] += _harmonics[h].real() * std::cos(phase) - _harmonics[h].imag() * std::sin(phase);
				}
			}
			return values;
		}
	}

	std::vector<int32_t> makeDigiProPoints(const std::vector<float>& _cycle)
	{
		const size_t length = _cycle.size();
		if(length < 2 || length > g_digiProMaxCycleLength)
			return {};

		// Band-limit the cycle: keep the harmonics below the source's own Nyquist
		// and below the 1024-point resolution's. DC is dropped.
		const size_t harmonicCount = std::min<size_t>((length - 1) / 2, 511);
		if(harmonicCount == 0)
			return {};
		std::vector<std::complex<double>> harmonics(harmonicCount);
		for(size_t h = 1; h <= harmonicCount; ++h)
		{
			std::complex<double> sum;
			for(size_t n = 0; n < length; ++n)
			{
				const double phase = -2.0 * g_pi * static_cast<double>((h * n) % length)
					/ static_cast<double>(length);
				sum += static_cast<double>(_cycle[n]) * std::complex<double>(std::cos(phase), std::sin(phase));
			}
			harmonics[h - 1] = sum * (2.0 / static_cast<double>(length));
		}

		// Normalise the plain band-limited cycle to the published level.
		double peak = 0.0;
		for(const auto value : synthesize(harmonics, 1024))
			peak = std::max(peak, std::abs(value));
		if(peak < 1e-6)
			return {};
		const double scale = g_cyclePeak / peak;

		// The full resolution already carries the lift of two notional finer
		// resolutions; every halving adds its own.
		for(size_t h = 1; h <= harmonicCount; ++h)
			harmonics[h - 1] *= emphasis(static_cast<double>(h) / 1024.0)
				* emphasis(static_cast<double>(h) / 2048.0);

		std::vector<int32_t> points;
		points.reserve(g_digiProPointCount);
		for(size_t size = 1024; size >= 4; size /= 2)
		{
			if(size < 1024)
			{
				harmonics.resize(std::min(harmonics.size(), size / 2 - 1));
				for(size_t h = 1; h <= harmonics.size(); ++h)
					harmonics[h - 1] *= emphasis(static_cast<double>(h) / static_cast<double>(size));
			}
			for(const auto value : synthesize(harmonics, size))
				points.push_back(toSample(value * scale));
		}
		return points;
	}

	std::vector<uint8_t> makeDigiProMessage(const uint8_t _slot, const std::string& _name,
		const std::vector<int32_t>& _points)
	{
		if(_points.size() != g_digiProPointCount || _slot >= g_digiProSlotCount)
			return {};

		std::vector<uint8_t> raw;
		raw.reserve(g_digiProPointCount * 3);
		for(const auto point : _points)
		{
			const auto value = static_cast<uint32_t>(point) & 0xffffffu;
			raw.push_back(static_cast<uint8_t>(value >> 16));
			raw.push_back(static_cast<uint8_t>(value >> 8));
			raw.push_back(static_cast<uint8_t>(value));
		}

		std::vector<uint8_t> message{0xf0, 0x00, 0x20, 0x3c, 0x03, 0x00, 0x5d, 0x01, 0x01, _slot};
		const auto name = makeDigiProName(_name);
		message.insert(message.end(), name.begin(), name.end());

		// Elektron packing: each 7 bytes are preceded by a byte of their MSBs,
		// first byte's MSB in bit 6.
		for(size_t i = 0; i < raw.size(); i += 7)
		{
			const auto count = std::min<size_t>(7, raw.size() - i);
			uint8_t high = 0;
			for(size_t j = 0; j < count; ++j)
				high |= static_cast<uint8_t>((raw[i + j] >> 7) << (6 - j));
			message.push_back(high);
			for(size_t j = 0; j < count; ++j)
				message.push_back(raw[i + j] & 0x7f);
		}

		uint32_t sum = 0;
		for(size_t i = 10; i < message.size(); ++i)
			sum += message[i];
		const auto length = static_cast<uint32_t>(message.size() + 4 + 1 - 10);
		message.push_back(static_cast<uint8_t>((sum >> 7) & 0x7f));
		message.push_back(static_cast<uint8_t>(sum & 0x7f));
		message.push_back(static_cast<uint8_t>((length >> 7) & 0x7f));
		message.push_back(static_cast<uint8_t>(length & 0x7f));
		message.push_back(0xf7);
		return message;
	}

	std::string makeDigiProName(const std::string& _source)
	{
		std::string name;
		for(const auto c : _source)
		{
			const auto upper = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
			if((upper >= 'A' && upper <= 'Z') || (upper >= '0' && upper <= '9') || upper == '+' || upper == '-')
				name.push_back(upper);
		}
		if(name.size() > 4)
		{
			// Keep a trailing number, which usually tells waves of a set apart.
			size_t digits = 0;
			while(digits < name.size() && std::isdigit(static_cast<unsigned char>(name[name.size() - 1 - digits])))
				++digits;
			if(digits > 0 && digits <= 3)
				name = name.substr(0, 4 - digits) + name.substr(name.size() - digits);
			else
				name.resize(4);
		}
		name.resize(4, ' ');
		return name;
	}
}
