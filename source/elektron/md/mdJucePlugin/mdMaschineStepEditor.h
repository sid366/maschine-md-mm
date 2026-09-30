#pragma once

#include "mdLib/mdfrontpanel.h"
#include "mdLib/mdtypes.h"

#include <array>
#include <string>

namespace mdJucePlugin::maschine
{
	enum class StepEditor { None, Accent, Swing, Slide, Arpeggiator };

	// Editors that show their own steps on the step LEDs instead of trigs:
	// MD 1.63 FUNCTION+B/C/D (ACCENT, SWING, SLIDE; the RECORD lamp goes dark)
	// and MM 1.32b FUNCTION+A/C/D (ARPEGGIATOR, SWING, SLIDE, followed by the
	// track). Their heading stays on screen while the editor is open. Each is
	// matched with a blank pixel margin, anywhere along its row, because the
	// firmware moves some of these boxes sideways.
	inline StepEditor stepEditor(const md::FrontPanel& panel, const md::MachineModel model)
	{
		struct Heading
		{
			md::MachineModel model;
			StepEditor editor;
			unsigned y;
			std::array<const char*, 7> rows;
		};
		static constexpr Heading headings[] = {
			{md::MachineModel::Machinedrum, StepEditor::Accent, 12, {
				"0000000000000000000000000000000000000",
				"0111110111110111110111110111110111110",
				"0100010100000100000100000100010001000",
				"0111110100000100000111110100010001000",
				"0100010100000100000100000100010001000",
				"0100010111110111110111110100010001000",
				"0000000000000000000000000000000000000"}},
			{md::MachineModel::Machinedrum, StepEditor::Swing, 12, {
				"000000000000000000000000000",
				"011111010101010111110111110",
				"010000010101010100010100000",
				"011111010101010100010101110",
				"000001010101010100010100010",
				"011111011111010100010111110",
				"000000000000000000000000000"}},
			{md::MachineModel::Machinedrum, StepEditor::Slide, 17, {
				"000000000000000000000000000",
				"011111010000010111100111110",
				"010000010000010100010100000",
				"011111010000010100010111110",
				"000001010000010100010100000",
				"011111011111010111100111110",
				"000000000000000000000000000"}},
			{md::MachineModel::Monomachine, StepEditor::Arpeggiator, 6, {
				"000000000000000000000000000000000000000000000000000000000000000",
				"011111011111011111011111011111011111010111110111110111110111110",
				"010001010001010001010000010000010000010100010001000100010100010",
				"011111011111011111011111010111010111010111110001000100010111110",
				"010001010010010000010000010001010001010100010001000100010100100",
				"010001010001010000011111011111011111010100010001000111110100010",
				"000000000000000000000000000000000000000000000000000000000000000"}},
			{md::MachineModel::Monomachine, StepEditor::Swing, 16, {
				"000000000000000000000000000",
				"011111010101010111110111110",
				"010000010101010100010100000",
				"011111010101010100010101110",
				"000001010101010100010100010",
				"011111011111010100010111110",
				"000000000000000000000000000"}},
			{md::MachineModel::Monomachine, StepEditor::Slide, 22, {
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
			if(heading.model != model)
				continue;
			const auto width = static_cast<unsigned>(std::char_traits<char>::length(heading.rows[0]));
			for(unsigned left = 0; left + width <= md::FrontPanel::g_lcdWidth; ++left)
			{
				bool match = true;
				for(unsigned y = 0; match && y < heading.rows.size(); ++y)
					for(unsigned x = 0; match && x < width; ++x)
						match = panel.getLcdPixel(left + x, heading.y + y) == (heading.rows[y][x] == '1');
				if(match)
					return heading.editor;
			}
		}
		return StepEditor::None;
	}
}
