#include "mdFrontPanelPresentation.h"

#include <cstdlib>
#include <iterator>
#include <iostream>

namespace
{
	void expect(const bool _condition, const char* const _message)
	{
		if(_condition)
			return;
		std::cerr << "mdFrontPanelPresentationTest: " << _message << '\n';
		std::exit(1);
	}

	void checkShortLitPulse()
	{
		md::FrontPanel panel;
		mdJucePlugin::FrontPanelLedPresentation presentation;
		presentation.reset(panel);
		constexpr double now = 1000.0;

		presentation.apply({1, 100, 0x26, 0x7f}, now);
		presentation.apply({2, 120, 0x26, 0xff}, now);
		presentation.advance(now);
		expect(presentation.isLit(0x26, 7),
			"tempo pulse collapsed inside one UI frame");
		presentation.advance(now
			+ mdJucePlugin::FrontPanelLedPresentation::g_minimumVisibleMilliseconds);
		expect(!presentation.isLit(0x26, 7),
			"tempo pulse did not return to firmware state");
	}

	void checkShortDarkPulse()
	{
		md::FrontPanel panel;
		const uint8_t pageOn[] = {0x27, 0x7f};
		panel.processBytes(pageOn, sizeof(pageOn));
		mdJucePlugin::FrontPanelLedPresentation presentation;
		presentation.reset(panel);
		constexpr double now = 2000.0;

		presentation.apply({3, 200, 0x27, 0xff}, now);
		presentation.apply({4, 220, 0x27, 0x7f}, now);
		presentation.advance(now);
		expect(!presentation.isLit(0x27, 7),
			"track-page dark pulse collapsed inside one UI frame");
		presentation.advance(now
			+ mdJucePlugin::FrontPanelLedPresentation::g_minimumVisibleMilliseconds);
		expect(presentation.isLit(0x27, 7),
			"track-page dark pulse did not return to firmware state");
	}

	void checkBicolorPulse()
	{
		md::FrontPanel panel;
		mdJucePlugin::FrontPanelLedPresentation presentation;
		presentation.reset(panel);
		constexpr double now = 3000.0;

		presentation.apply({5, 300, 0x20, 0xfd}, now);
		presentation.apply({6, 320, 0x20, 0xff}, now);
		presentation.advance(now);
		expect(md::FrontPanel::decodeMonomachineStepLedColor(
			presentation.getLedBankRaw(0x20), 0) == md::FrontPanel::LedColor::Red,
			"red TRIG pulse was not preserved");
	}

	void checkAllTrigPulses()
	{
		md::FrontPanel panel;
		mdJucePlugin::FrontPanelLedPresentation presentation;
		presentation.reset(panel);
		constexpr double now = 4000.0;

		// MM encodes four bicolor TRIG LEDs in each of banks 0x20..0x23.
		// Exercise all sixteen as short red pulses that begin and end before a
		// presentation frame, exactly the case a snapshot-based GUI used to miss.
		for(uint8_t bank = 0x20; bank <= 0x23; ++bank)
		{
			presentation.apply({static_cast<uint64_t>(bank - 0x20) * 2 + 7,
				400, bank, 0x55}, now);
			presentation.apply({static_cast<uint64_t>(bank - 0x20) * 2 + 8,
				420, bank, 0xff}, now);
		}
		presentation.advance(now);

		for(uint32_t trig = 0; trig < 16; ++trig)
		{
			const auto bank = static_cast<uint8_t>(0x20 + (trig >> 2));
			expect(md::FrontPanel::decodeMonomachineStepLedColor(
				presentation.getLedBankRaw(bank), trig & 3)
					== md::FrontPanel::LedColor::Red,
				"one of the sixteen short TRIG pulses was skipped");
		}
	}

	void checkLateFrameBacklog()
	{
		md::FrontPanel panel;
		mdJucePlugin::FrontPanelLedPresentation presentation;
		presentation.reset(panel);
		constexpr double now = 5000.0;
		constexpr uint64_t cyclesPerMs = md::g_ucClockHz / 1000;

		// A frame that arrives 400 ms late (a busy computer). Step 1 blinked at
		// the start of the backlog, step 2 just now, step 3 lit and stayed lit.
		const md::FrontPanelLedTransition backlog[] =
		{
			{10, 0 * cyclesPerMs, 0x20, 0xfe},
			{11, 10 * cyclesPerMs, 0x20, 0xff},
			{12, 200 * cyclesPerMs, 0x20, 0xfb},
			{13, 390 * cyclesPerMs, 0x20, 0xf9},
			{14, 395 * cyclesPerMs, 0x20, 0xfb},
			{15, 400 * cyclesPerMs, 0x22, 0xfe},
		};
		presentation.applyBacklog(backlog, std::size(backlog), now);
		presentation.advance(now);
		expect(!presentation.isLit(0x20, 0),
			"a pulse that ended 390 ms ago was shown as current");
		expect(presentation.isLit(0x20, 1),
			"a pulse inside the last frame was not shown");
		expect(presentation.isLit(0x20, 2),
			"a step lit in the backlog was not shown");
		presentation.advance(now
			+ mdJucePlugin::FrontPanelLedPresentation::g_minimumVisibleMilliseconds);
		expect(!presentation.isLit(0x20, 1) && presentation.isLit(0x20, 2),
			"late frame did not settle to the firmware state");
	}
}

int main()
{
	checkShortLitPulse();
	checkShortDarkPulse();
	checkBicolorPulse();
	checkAllTrigPulses();
	checkLateFrameBacklog();
	std::cout << "MD/MM front-panel presentation: PASS\n";
	return 0;
}
