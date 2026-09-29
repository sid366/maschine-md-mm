#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace md
{
	// Monomachine SFX-60 MKII DigiPRO user waveforms, in the SysEx format that
	// GLOBAL > FILE > DIGIPRO MGR > RECEIVE accepts. One message holds one
	// single-cycle wave at nine resolutions (1024, 512, ... 4 points) of signed
	// 24-bit samples; the lower resolutions serve higher notes without aliasing.
	inline constexpr size_t g_digiProSlotCount = 64;
	inline constexpr size_t g_digiProMaxCycleLength = 8192;
	inline constexpr size_t g_digiProPointCount = 2044;	// 1024 + 512 + ... + 4
	inline constexpr size_t g_digiProMessageSize = 7027;

	// Builds the nine resolutions from one cycle of audio. Returns an empty
	// vector for a cycle that is too short, too long or silent.
	std::vector<int32_t> makeDigiProPoints(const std::vector<float>& _cycle);

	// Wraps the points in a DigiPRO wave message for _slot (0-63). The name is
	// shown on the Monomachine; see makeDigiProName.
	std::vector<uint8_t> makeDigiProMessage(uint8_t _slot, const std::string& _name,
		const std::vector<int32_t>& _points);

	// Four characters the Monomachine can show, derived from a file name:
	// "MG12" stays "MG12", "Wave 7" becomes "WAV7".
	std::string makeDigiProName(const std::string& _source);
}
