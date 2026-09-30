#pragma once

#include "mdLib/mdfrontpanel.h"

#include <array>

namespace mdJucePlugin::maschine
{
	enum class MdStepEditor { None, Accent, Swing, Slide };

	// MD 1.63 keeps an ACCENT, SWING or SLIDE heading on screen while
	// FUNCTION+B/C/D edits the pattern's accent, swing or slide steps. Its step
	// LEDs then show those steps, not trigs, and the RECORD lamp goes dark.
	// Each heading is matched with a blank pixel margin around it.
	inline MdStepEditor mdStepEditor(const md::FrontPanel& panel)
	{
		struct Heading
		{
			MdStepEditor editor;
			unsigned x, y;
			std::array<const char*, 7> rows;
		};
		static constexpr Heading headings[] = {
			{MdStepEditor::Accent, 45, 12, {
				"0000000000000000000000000000000000000",
				"0111110111110111110111110111110111110",
				"0100010100000100000100000100010001000",
				"0111110100000100000111110100010001000",
				"0100010100000100000100000100010001000",
				"0100010111110111110111110100010001000",
				"0000000000000000000000000000000000000"}},
			{MdStepEditor::Swing, 50, 12, {
				"000000000000000000000000000",
				"011111010101010111110111110",
				"010000010101010100010100000",
				"011111010101010100010101110",
				"000001010101010100010100010",
				"011111011111010100010111110",
				"000000000000000000000000000"}},
			{MdStepEditor::Slide, 50, 17, {
				"000000000000000000000000000",
				"011111010000010111100111110",
				"010000010000010100010100000",
				"011111010000010100010111110",
				"000001010000010100010100000",
				"011111011111010111100111110",
				"000000000000000000000000000"}},
		};
		for(const auto& heading : headings)
		{
			bool match = true;
			for(unsigned y = 0; match && y < heading.rows.size(); ++y)
				for(unsigned x = 0; match && heading.rows[y][x]; ++x)
					match = panel.getLcdPixel(heading.x + x, heading.y + y) == (heading.rows[y][x] == '1');
			if(match)
				return heading.editor;
		}
		return MdStepEditor::None;
	}
}
