#include "mdLib/mddevice.h"
#include "mdLib/mdromloader.h"
#include "mdLib/mdmidiprotocol.h"
#include "mdLib/mdsysexautomation.h"
#include "baseLib/filesystem.h"
#include "../mdJucePlugin/mdMaschineRecordedSteps.h"
#include "../mdJucePlugin/mdMaschineSectionPlayback.h"
#include "../mdJucePlugin/mdFrontPanelPresentation.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	void require(const bool _condition, const char* const _message)
	{
		if(!_condition)
			throw std::runtime_error(_message);
	}

	void advance(md::Hardware& _hardware, uint32_t _frames)
	{
		while(_frames)
		{
			const auto count = std::min(_frames, 256u);
			_hardware.advance(count);
			_frames -= count;
		}
	}

	bool recordLed(const md::Hardware& _hardware, const md::MachineModel _model)
	{
		const auto panel = _hardware.getFrontPanelSnapshot();
		return _model == md::MachineModel::Machinedrum
			? panel.getModeLed(md::FrontPanel::ModeLed::Record)
			: (panel.getLedBankRaw(0x27) & 1u) == 0;
	}
}

int main(const int _argc, const char* const* const _argv)
{
	if(_argc < 2)
		return 1;
	const auto model = std::string_view(_argv[1]) == "md"
		? md::MachineModel::Machinedrum : md::MachineModel::Monomachine;
	const char* const path = std::getenv(model == md::MachineModel::Machinedrum
		? "GEARMULATOR_MD_FIRMWARE_BIN" : "GEARMULATOR_MM_FIRMWARE_BIN");
	if(!path || !*path)
	{
		std::cout << "SKIP: firmware not supplied\n";
		return 77;
	}
	try
	{
		synthLib::DeviceCreateParams params;
		require(baseLib::filesystem::readFile(params.romData, path),
			"cannot read firmware");
		require(md::RomLoader::isRomForModel(params.romData, model),
			"wrong firmware model");
		params.romName = path;
		params.customData = md::deviceCustomData(model);
		// No homePath: never read or change the user's machine storage.
		auto device = std::make_unique<md::Device>(params);
		auto& hardware = device->getHardware();
		advance(hardware, md::g_samplerate * 25);
		require(hardware.isAudioReady() && hardware.isFirmwareMidiReady(),
			"boot incomplete");
		md::PanelRowState rows;
		if(_argc >= 3 && std::string_view(_argv[2]) == "--copy-paste")
		{
			const auto key = [&](md::PanelControl control, bool down, unsigned milliseconds) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "copy input rejected");
				advance(hardware, md::g_samplerate * milliseconds / 1000);
			};
			const auto tap = [&](md::PanelControl control) { key(control,true,100); key(control,false,100); };
			const auto chord = [&](md::PanelControl control) {
				key(md::PanelControl::Function,true,33);
				key(control,true,66); key(control,false,33);
				key(md::PanelControl::Function,false,100);
			};
			const auto steps = [&]() {
				const auto panel = hardware.getFrontPanelSnapshot();
				unsigned mask = 0;
				for(unsigned i=0;i<16;++i)
					if(model == md::MachineModel::Machinedrum ? panel.getStepLed(i)
						: panel.getMonomachineStepLedColor(i) != md::FrontPanel::LedColor::Off) mask |= 1u << i;
				return mask;
			};
			tap(md::PanelControl::Record);
			tap(md::PanelControl::Trigger5);
			tap(md::PanelControl::Trigger13);
			const auto original = steps();
			require(original != 0, "copy fixture has no trigs");
			tap(md::PanelControl::Record);
			require(!recordLed(hardware,model), "not in pattern copy mode");
			chord(md::PanelControl::Record);
			chord(md::PanelControl::Play);
			tap(md::PanelControl::Record);
			require(steps()==0, "pattern was not cleared before paste");
			tap(md::PanelControl::Record);
			chord(md::PanelControl::Stop);
			tap(md::PanelControl::Record);
			require(steps()==original, "PASTE did not restore the copied pattern");
			std::cout << "PASS: pattern copy/clear/paste restores recorded triggers\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--md-step-editors")
		{
			require(model == md::MachineModel::Machinedrum, "step editor fixture requires MD");
			using mdJucePlugin::maschine::StepEditor;
			using Pad = mdJucePlugin::maschine::nihia::LedColor;
			const auto key = [&](md::PanelControl control, bool down, unsigned milliseconds) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "editor input rejected");
				advance(hardware, md::g_samplerate * milliseconds / 1000);
			};
			const auto tap = [&](md::PanelControl control) { key(control,true,100); key(control,false,100); };
			const auto chord = [&](md::PanelControl control) {
				key(md::PanelControl::Function,true,33);
				key(control,true,66); key(control,false,33);
				key(md::PanelControl::Function,false,100);
			};
			// In an editor the pads show its steps in blue, whatever the record
			// state; returns how many are lit.
			const auto expectEditor = [&](const StepEditor editor, const char* message) {
				const auto panel = hardware.getFrontPanelSnapshot();
				require(mdJucePlugin::maschine::stepEditor(panel, model) == editor, message);
				unsigned lit = 0;
				for(unsigned step = 0; editor != StepEditor::None && step < 16; ++step)
				{
					const auto color = mdJucePlugin::maschine::machinedrumPadColor(panel, step, false, false, false, 0);
					require(color == (panel.getStepLed(step) ? Pad::Blue : Pad::Off),
						"editor pad does not mirror its step");
					lit += panel.getStepLed(step) ? 1u : 0u;
				}
				return lit;
			};
			tap(md::PanelControl::Record);
			tap(md::PanelControl::Trigger5);
			tap(md::PanelControl::Trigger9);
			expectEditor(StepEditor::None, "grid recording taken for an editor");
			chord(md::PanelControl::BankB);
			const auto accents = expectEditor(StepEditor::Accent, "FUNCTION+B did not open ACCENT");
			require(accents > 0, "ACCENT shows no steps");
			advance(hardware, md::g_samplerate * 2);
			require(!recordLed(hardware, model), "RECORD lamp lit in the ACCENT editor");
			require(expectEditor(StepEditor::Accent, "ACCENT closed by itself") == accents,
				"ACCENT steps did not persist");
			tap(md::PanelControl::Trigger5);
			require(expectEditor(StepEditor::Accent, "trig closed ACCENT") != accents,
				"trig did not toggle an accent");
			chord(md::PanelControl::BankB);
			expectEditor(StepEditor::None, "FUNCTION+B did not close ACCENT");
			chord(md::PanelControl::BankC);
			require(expectEditor(StepEditor::Swing, "FUNCTION+C did not open SWING") > 0,
				"SWING shows no steps");
			tap(md::PanelControl::Exit);
			expectEditor(StepEditor::None, "EXIT did not close SWING");
			chord(md::PanelControl::BankD);
			expectEditor(StepEditor::Slide, "FUNCTION+D did not open SLIDE");
			tap(md::PanelControl::Exit);
			expectEditor(StepEditor::None, "EXIT did not close SLIDE");
			require(recordLed(hardware, model), "editors did not return to grid recording");
			std::cout << "PASS: accent/swing/slide editors show their steps on the pads\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--mm-step-editors")
		{
			require(model == md::MachineModel::Monomachine, "step editor fixture requires MM");
			using mdJucePlugin::maschine::StepEditor;
			using Pad = mdJucePlugin::maschine::nihia::LedColor;
			using Native = md::FrontPanel::LedColor;
			const auto key = [&](md::PanelControl control, bool down, unsigned milliseconds) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "editor input rejected");
				advance(hardware, md::g_samplerate * milliseconds / 1000);
			};
			const auto tap = [&](md::PanelControl control) { key(control,true,100); key(control,false,100); };
			const auto chord = [&](md::PanelControl control) {
				key(md::PanelControl::Function,true,33);
				key(control,true,66); key(control,false,33);
				key(md::PanelControl::Function,false,100);
			};
			// Returns which pads show blue, after checking each against its LED.
			const auto expectEditor = [&](const StepEditor editor, const char* message) {
				const auto panel = hardware.getFrontPanelSnapshot();
				require(mdJucePlugin::maschine::stepEditor(panel, model) == editor, message);
				unsigned blue = 0;
				for(unsigned step = 0; editor != StepEditor::None && step < 16; ++step)
				{
					const auto native = panel.getMonomachineStepLedColor(step);
					const auto color = mdJucePlugin::maschine::monomachineEditorPadColor(native);
					require(color == (native == Native::Green ? Pad::Blue
						: native == Native::Yellow ? Pad::Red : Pad::Off), "editor pad does not mirror its step");
					blue |= color == Pad::Blue ? 1u << step : 0u;
				}
				return blue;
			};
			tap(md::PanelControl::Record);
			tap(md::PanelControl::Trigger5);
			tap(md::PanelControl::Trigger9);
			expectEditor(StepEditor::None, "grid recording taken for an editor");
			chord(md::PanelControl::BankA);
			const auto arp = expectEditor(StepEditor::Arpeggiator, "FUNCTION+A did not open ARPEGGIATOR");
			require(arp != 0, "ARPEGGIATOR shows no steps");
			// Toggling a step moves this box sideways; it must stay recognised.
			tap(md::PanelControl::Trigger3);
			require(expectEditor(StepEditor::Arpeggiator, "trig closed ARPEGGIATOR") == (arp & ~(1u << 2)),
				"arp step 3 was not turned off");
			tap(md::PanelControl::Exit);
			expectEditor(StepEditor::None, "EXIT did not close ARPEGGIATOR");
			chord(md::PanelControl::BankB);
			expectEditor(StepEditor::None, "TRANSPOSE taken for a step editor");
			tap(md::PanelControl::Exit);
			chord(md::PanelControl::BankC);
			const auto swing = expectEditor(StepEditor::Swing, "FUNCTION+C did not open SWING");
			require(swing != 0, "SWING shows no steps");
			advance(hardware, md::g_samplerate * 2);
			require(expectEditor(StepEditor::Swing, "SWING closed by itself") == swing, "SWING steps did not persist");
			tap(md::PanelControl::Exit);
			expectEditor(StepEditor::None, "EXIT did not close SWING");
			chord(md::PanelControl::BankD);
			require(expectEditor(StepEditor::Slide, "FUNCTION+D did not open SLIDE") == 0, "new pattern has slides");
			tap(md::PanelControl::Trigger3);
			require(expectEditor(StepEditor::Slide, "trig closed SLIDE") == 1u << 2, "slide step 3 not shown");
			tap(md::PanelControl::Exit);
			expectEditor(StepEditor::None, "EXIT did not close SLIDE");
			std::cout << "PASS: arpeggiator/swing/slide editors show their steps on the pads\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--md-mute-immediate")
		{
			// Muting a track must silence it at once, while its sound keeps running:
			// a resampled bar (hits on steps 1, 5, 9, 13) plays on RAM-P1; muted after
			// the second hit, the third must be silent; unmuted before the fourth, the
			// fourth must play at full level.
			require(model == md::MachineModel::Machinedrum, "resample fixture requires MD");
			const auto key = [&](md::PanelControl control, bool down, unsigned milliseconds) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "panel event rejected");
				advance(hardware, md::g_samplerate * milliseconds / 1000);
			};
			const auto tap = [&](md::PanelControl control) { key(control, true, 50); key(control, false, 100); };
			const auto sysex = [&](std::initializer_list<uint8_t> _body) {
				synthLib::SMidiEvent e(synthLib::MidiEventSource::Host);
				e.sysex = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00};
				e.sysex.insert(e.sysex.end(), _body.begin(), _body.end());
				e.sysex.push_back(0xf7);
				hardware.sendMidi(e);
				advance(hardware, md::g_samplerate / 10);
			};
			// MD OS 1.63 CC map: tracks 1-4 from CC 16/40/72/96, next four on the next channel.
			const auto parameter = [&](uint8_t _track, uint8_t _param, uint8_t _value) {
				constexpr uint8_t base[] = {0x10, 0x28, 0x48, 0x60};
				hardware.sendMidi({synthLib::MidiEventSource::Host, uint8_t(synthLib::M_CONTROLCHANGE + _track / 4),
					uint8_t(base[_track % 4] + _param), _value});
				advance(hardware, 4096);
			};
			constexpr uint8_t recorder = 5, player = 6, source = 2;
			sysex({0x5b, recorder, 32, 1});	// RAM-R1
			sysex({0x5b, player, 34, 1});	// RAM-P1
			sysex({0x5b, source, 0, 1});	// ROM-01
			for(uint8_t t : {uint8_t(0), uint8_t(3), uint8_t(4), uint8_t(8), uint8_t(12), uint8_t(13), uint8_t(14), uint8_t(15)})
				sysex({0x5b, t, 0, 0});		// GND-EM: silence the demo pattern
			for(uint8_t t : {recorder, player, source})
				for(uint8_t p : {uint8_t(19), uint8_t(20), uint8_t(22)})	// DEL, REV, LFO depth
					parameter(t, p, 0);
			// MLEV well below clipping, MBAL centre, ILEV/CUE off, one bar, full rate.
			for(const auto [p, v] : {std::pair<uint8_t, uint8_t>{0, 32}, {1, 64}, {2, 0}, {4, 0}, {5, 0}, {6, 64}, {7, 127}})
				parameter(recorder, p, v);
			const auto setTrig = [&](uint8_t _track, bool _on) {
				for(int attempt = 0; attempt < 4; ++attempt)
				{
					sysex({0x71, 0x22, _track});
					advance(hardware, 8192);
					if(hardware.getFrontPanelSnapshot().getStepLed(0) == _on)
						return;
					tap(md::PanelControl::Trigger1);
				}
				require(false, "could not edit a step-1 trig");
			};
			const auto press = [&](md::PanelControl _c, bool _down) {
				const auto packet = md::panelPacket(model, _c).value();
				const auto e = _down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(e.row, e.mask), "panel event rejected");
			};
			const auto render = [&](uint32_t _frames, std::vector<float>& _out) {
				std::vector<float> left(_frames), right(_frames);
				synthLib::TAudioOutputs outputs{};
				outputs[0] = left.data(); outputs[1] = right.data();
				for(uint32_t offset = 0; offset < _frames; offset += 256)
				{
					hardware.processAudio(outputs, std::min(256u, _frames - offset), 0);
					outputs[0] += 256; outputs[1] += 256;
				}
				_out.insert(_out.end(), left.begin(), left.end());
			};
			const md::PanelControl hits[] = {md::PanelControl::Trigger1, md::PanelControl::Trigger5,
				md::PanelControl::Trigger9, md::PanelControl::Trigger13};
			const auto setSourceTrigs = [&](bool _on) {
				sysex({0x71, 0x22, source});
				advance(hardware, 8192);
				for(unsigned i = 0; i < 4; ++i)
					for(int attempt = 0; attempt < 4 && hardware.getFrontPanelSnapshot().getStepLed(i * 4) != _on; ++attempt)
					{
						tap(hits[i]);
						advance(hardware, 4096);
					}
				for(unsigned i = 0; i < 4; ++i)
					require(hardware.getFrontPanelSnapshot().getStepLed(i * 4) == _on, "could not edit the source trigs");
			};
			tap(md::PanelControl::Record);
			setTrig(recorder, true);
			setSourceTrigs(true);
			tap(md::PanelControl::Record);
			{
				std::vector<float> take;
				press(md::PanelControl::Play, true); render(2048, take); press(md::PanelControl::Play, false);
				render(90000, take);
				tap(md::PanelControl::Stop);
				advance(hardware, md::g_samplerate);
			}
			tap(md::PanelControl::Record);
			setTrig(recorder, false);
			setSourceTrigs(false);
			setTrig(player, true);
			tap(md::PanelControl::Record);
			const auto mute = [&](bool _on) {	// MD CC map: CC 12-15 mute tracks 1-4 of each channel
				hardware.sendMidi({synthLib::MidiEventSource::Host, uint8_t(synthLib::M_CONTROLCHANGE + player / 4),
					uint8_t(12 + player % 4), uint8_t(_on ? 127 : 0)});
			};
			const auto bar = size_t(md::g_samplerate * 60.0 / 125.0 * 4);	// test tempo
			std::vector<float> out;
			press(md::PanelControl::Play, true); render(2048, out); press(md::PanelControl::Play, false);
			render(uint32_t(bar * 3 / 8 - out.size()), out);	// between hits 2 and 3
			mute(true);
			render(uint32_t(bar * 5 / 8 - out.size()), out);	// between hits 3 and 4
			mute(false);
			render(uint32_t(bar - out.size()), out);
			tap(md::PanelControl::Stop);
			const auto peak = [&](size_t _begin, size_t _end) {
				float p = 0; for(size_t i = _begin; i < _end && i < out.size(); ++i) p = std::max(p, std::abs(out[i])); return p;
			};
			const auto second = peak(bar / 4 - 1000, bar / 4 + 8000), third = peak(bar / 2 - 1000, bar / 2 + 8000),
				fourth = peak(bar * 3 / 4 - 1000, bar * 3 / 4 + 8000);
			std::cout << "RAM-P1 hits: second " << second << ", third (muted) " << third << ", fourth (unmuted) " << fourth << "\n";
			require(second > 0.02f, "RAM-P1 did not play the resample");
			require(third < second * 0.01f, "the mute did not silence the playing sample");
			require(fourth > second * 0.7f, "the sample did not keep playing under the mute");
			std::cout << "PASS: a mute silences a playing sample at once and keeps it running\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--md-resample-alignment")
		{
			// A main-mix resample played back on the trig it was recorded on must
			// line up with the live track it doubles. The default pattern's tracks
			// are silenced; a ROM sample on step 1 is recorded by RAM-R1 and then
			// layered with RAM-P1.
			require(model == md::MachineModel::Machinedrum, "resample fixture requires MD");
			const auto key = [&](md::PanelControl control, bool down, unsigned milliseconds) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "panel event rejected");
				advance(hardware, md::g_samplerate * milliseconds / 1000);
			};
			const auto tap = [&](md::PanelControl control) { key(control, true, 50); key(control, false, 100); };
			const auto sysex = [&](std::initializer_list<uint8_t> _body) {
				synthLib::SMidiEvent e(synthLib::MidiEventSource::Host);
				e.sysex = {0xf0, 0x00, 0x20, 0x3c, 0x02, 0x00};
				e.sysex.insert(e.sysex.end(), _body.begin(), _body.end());
				e.sysex.push_back(0xf7);
				hardware.sendMidi(e);
				advance(hardware, md::g_samplerate / 10);
			};
			// MD OS 1.63 CC map: tracks 1-4 from CC 16/40/72/96, next four on the next channel.
			const auto parameter = [&](uint8_t _track, uint8_t _param, uint8_t _value) {
				constexpr uint8_t base[] = {0x10, 0x28, 0x48, 0x60};
				hardware.sendMidi({synthLib::MidiEventSource::Host, uint8_t(synthLib::M_CONTROLCHANGE + _track / 4),
					uint8_t(base[_track % 4] + _param), _value});
				advance(hardware, 4096);
			};
			constexpr uint8_t recorder = 5, player = 6, source = 2;
			sysex({0x5b, recorder, 32, 1});	// RAM-R1
			sysex({0x5b, player, 34, 1});	// RAM-P1
			sysex({0x5b, source, 0, 1});	// ROM-01
			for(uint8_t t : {uint8_t(0), uint8_t(3), uint8_t(4), uint8_t(8), uint8_t(12), uint8_t(13), uint8_t(14), uint8_t(15)})
				sysex({0x5b, t, 0, 0});		// GND-EM: silence the demo pattern
			for(uint8_t t : {recorder, player, source})
				for(uint8_t p : {uint8_t(19), uint8_t(20), uint8_t(22)})	// DEL, REV, LFO depth
					parameter(t, p, 0);
			// MLEV well below clipping, MBAL centre, ILEV/CUE off, one bar, full rate.
			for(const auto [p, v] : {std::pair<uint8_t, uint8_t>{0, 32}, {1, 64}, {2, 0}, {4, 0}, {5, 0}, {6, 64}, {7, 127}})
				parameter(recorder, p, v);
			const auto setTrig = [&](uint8_t _track, bool _on) {
				for(int attempt = 0; attempt < 4; ++attempt)
				{
					sysex({0x71, 0x22, _track});
					advance(hardware, 8192);
					if(hardware.getFrontPanelSnapshot().getStepLed(0) == _on)
						return;
					tap(md::PanelControl::Trigger1);
				}
				require(false, "could not edit a step-1 trig");
			};
			const auto run = [&] {
				const uint32_t frames = 30000;
				std::vector<float> left(frames), right(frames);
				synthLib::TAudioOutputs outputs{};
				outputs[0] = left.data(); outputs[1] = right.data();
				const auto packet = md::panelPacket(model, md::PanelControl::Play).value();
				const auto down = rows.press(packet);
				require(hardware.trySendPanelEvent(down.row, down.mask), "PLAY rejected");
				for(uint32_t offset = 0; offset < frames; offset += 256)
				{
					if(offset == 2048)
					{
						const auto up = rows.release(packet);
						require(hardware.trySendPanelEvent(up.row, up.mask), "PLAY release rejected");
					}
					hardware.processAudio(outputs, std::min(256u, frames - offset), 0);
					outputs[0] += 256; outputs[1] += 256;
				}
				tap(md::PanelControl::Stop);
				advance(hardware, md::g_samplerate / 2);
				return left;
			};
			tap(md::PanelControl::Record);
			setTrig(recorder, true);
			setTrig(source, true);
			tap(md::PanelControl::Record);
			(void)run();						// RAM-R1 records the step-1 hit
			tap(md::PanelControl::Record);
			setTrig(recorder, false);
			setTrig(player, true);
			tap(md::PanelControl::Record);
			const auto layered = run();			// live hit + RAM-P1
			tap(md::PanelControl::Record);
			setTrig(player, false);
			tap(md::PanelControl::Record);
			const auto live = run();			// live hit alone

			const auto onset = [](const std::vector<float>& _a) {
				size_t i = 0;
				while(i < _a.size() && std::abs(_a[i]) < 0.02f) ++i;
				return i;
			};
			const auto correlate = [](const std::vector<float>& _a, size_t _aBegin,
				const std::vector<float>& _b, size_t _bBegin, size_t _length, int _range, double& _gain) {
				double best = -1; int bestLag = 0;
				for(int lag = -_range; lag <= _range; ++lag)
				{
					double ab = 0, aa = 0, bb = 0;
					for(size_t i = 0; i < _length; ++i)
					{
						const double x = _a[_aBegin + i], y = _b[size_t(int64_t(_bBegin + i) + lag)];
						ab += x * y; aa += x * x; bb += y * y;
					}
					const double c = std::abs(ab) / std::sqrt(aa * bb + 1e-30);
					if(c > best) { best = c; bestLag = lag; _gain = ab / (aa + 1e-30); }
				}
				return bestLag;
			};
			const auto liveHit = onset(live), layeredHit = onset(layered);
			require(liveHit > 400 && layeredHit > 400 && liveHit + 4000 < live.size(), "no live hit");
			double gain = 0;
			// The live hit dominates the layered run: align the runs on it, then the
			// difference is RAM-P1's copy.
			const auto runOffset = int64_t(layeredHit) - int64_t(liveHit)
				+ correlate(live, liveHit - 40, layered, layeredHit - 40, 400, 20, gain);
			std::vector<float> copy(live.size(), 0.0f);
			for(size_t i = 0; i < live.size(); ++i)
			{
				const auto j = int64_t(i) + runOffset;
				if(j >= 0 && j < int64_t(layered.size()))
					copy[i] = layered[size_t(j)] - live[i];
			}
			const auto lag = correlate(live, liveHit - 40, copy, liveHit - 40, 2500, 300, gain);
			std::cout << "resampled copy lands " << lag << " frames (" << lag * 1000.0 / md::g_samplerate
				<< " ms) after the live hit, at " << 20 * std::log10(std::abs(gain) + 1e-12) << " dB\n";
			require(gain > 0.1 && gain < 0.5, "RAM-P1 did not play the resampled hit");
			require(lag >= -24 && lag <= 24, "the resample does not line up with the live track");
			std::cout << "PASS: main-mix resample lines up with the track it was recorded from\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--md-step-underline")
		{
			// The MK3 bar underline must follow each SCALE (STEP) press in grid
			// recording even when the display updates rarely, as on a busy computer.
			require(model == md::MachineModel::Machinedrum, "underline fixture requires MD");
			using namespace mdJucePlugin::maschine;
			const auto key = [&](md::PanelControl control, bool down, unsigned milliseconds) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "panel event rejected");
				advance(hardware, md::g_samplerate * milliseconds / 1000);
			};
			const auto tap = [&](md::PanelControl control) { key(control, true, 50); key(control, false, 50); };
			tap(md::PanelControl::Record);
			key(md::PanelControl::Function, true, 100);
			tap(md::PanelControl::Scale);
			key(md::PanelControl::Function, false, 100);
			for(int attempt = 0; attempt < 6 && occupiedScalePages(hardware.getFrontPanelSnapshot(), model) != 0x0f; ++attempt)
				tap(md::PanelControl::Scale);
			require(occupiedScalePages(hardware.getFrontPanelSnapshot(), model) == 0x0f, "pattern length setup failed");
			MachinedrumSectionDisplay display, lampsOnly;
			const auto base = std::chrono::steady_clock::now();
			double seconds = 0;
			bool playing = false;
			const auto update = [&](const double _dt) {
				advance(hardware, uint32_t(md::g_samplerate * _dt));
				seconds += _dt;
				const auto panel = hardware.getFrontPanelSnapshot();
				const auto now = base + std::chrono::microseconds(int64_t(seconds * 1e6));
				display.update(panel, playing, now);
				lampsOnly.update(panel, playing, now);
			};
			update(0.033); // the SCALE setup shows the four pages
			tap(md::PanelControl::Enter);
			tap(md::PanelControl::Play);
			playing = true;
			for(int i = 0; i < 30; ++i) update(0.033);
			int expected = display.sections.selected;
			unsigned lampsOnlyLate = 0;
			constexpr double gaps[] = {0.12, 0.18, 0.09, 0.25};
			for(int press = 0; press < 12; ++press)
			{
				// As the controller does: counted when sent, applied before the next update.
				display.sections.notePresses(1, 15);
				tap(md::PanelControl::Scale);
				expected = (expected + 1) % 4;
				for(int i = 0; i < 6; ++i)
				{
					update(gaps[(press + i) % 4]);
					require(display.sections.selected == expected, "bar underline did not follow a SCALE press");
				}
				lampsOnlyLate += lampsOnly.sections.selected != expected;
			}
			std::cout << "PASS: bar underline follows every SCALE press (lamps alone missed "
				<< lampsOnlyLate << " of 12)\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--md-led-backlog")
		{
			// The editor's step LEDs must show the MD's current state even when the
			// GUI refreshes rarely (a busy computer) and drains a long backlog.
			require(model == md::MachineModel::Machinedrum, "LED backlog fixture requires MD");
			using namespace mdJucePlugin::maschine;
			const auto key = [&](md::PanelControl control, bool down, unsigned milliseconds) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "panel event rejected");
				advance(hardware, md::g_samplerate * milliseconds / 1000);
			};
			const auto tap = [&](md::PanelControl control) { key(control, true, 50); key(control, false, 50); };
			tap(md::PanelControl::Record);
			key(md::PanelControl::Function, true, 100);
			tap(md::PanelControl::Scale);
			key(md::PanelControl::Function, false, 100);
			for(int attempt = 0; attempt < 6 && occupiedScalePages(hardware.getFrontPanelSnapshot(), model) != 0x0f; ++attempt)
				tap(md::PanelControl::Scale);
			tap(md::PanelControl::Enter);
			for(auto trig : {md::PanelControl::Trigger1, md::PanelControl::Trigger5,
				md::PanelControl::Trigger9, md::PanelControl::Trigger13})
				tap(trig);
			tap(md::PanelControl::Play);
			advance(hardware, md::g_samplerate / 2);
			std::array<md::FrontPanelLedTransition, 256> drained;
			std::vector<md::FrontPanelLedTransition> backlog;
			const auto drain = [&] {
				backlog.clear();
				for(;;)
				{
					const auto count = device->drainFrontPanelLedTransitions(drained.data(), drained.size());
					backlog.insert(backlog.end(), drained.begin(), drained.begin() + count);
					if(count < drained.size())
						break;
				}
			};
			const auto stepBits = [](auto&& _raw) {
				return unsigned(uint8_t(~_raw(0x20))) | unsigned(uint8_t(~_raw(0x21))) << 8;
			};
			drain();
			mdJucePlugin::FrontPanelLedPresentation fixed, frameTimed;
			fixed.reset(hardware.getFrontPanelSnapshot());
			frameTimed = fixed;
			std::mt19937 rng(7);
			std::uniform_real_distribution<double> jitter(0.6, 1.4);
			double now = 0, sincePress = 0;
			unsigned frames = 0, wrong = 0, frameTimedWrong = 0;
			while(now < 20000)
			{
				const double dt = 400 * jitter(rng);
				advance(hardware, uint32_t(md::g_samplerate * dt / 1000));
				now += dt;
				sincePress += dt;
				if(sincePress > 700)
				{
					tap(md::PanelControl::Scale);
					now += 100;
					sincePress = 0;
				}
				drain();
				fixed.applyBacklog(backlog.data(), backlog.size(), now);
				for(const auto& transition : backlog)
					frameTimed.apply(transition, now);
				fixed.advance(now);
				frameTimed.advance(now);
				const auto truth = hardware.getFrontPanelSnapshot();
				const auto actual = stepBits([&](uint8_t c) { return truth.getLedBankRaw(c); });
				++frames;
				wrong += stepBits([&](uint8_t c) { return fixed.getLedBankRaw(c); }) != actual;
				frameTimedWrong += stepBits([&](uint8_t c) { return frameTimed.getLedBankRaw(c); }) != actual;
			}
			// A pulse in the newest frame's worth of the backlog may still be held.
			require(wrong <= 1, "step LEDs showed a stale backlog state");
			std::cout << "PASS: step LEDs match the MD in " << frames - wrong << " of " << frames
				<< " late frames (timing by frame: " << frames - frameTimedWrong << ")\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--clear-track")
		{
			require(model == md::MachineModel::Monomachine, "MM clear fixture requires MM");
			const auto key = [&](md::PanelControl control, bool down, unsigned milliseconds) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "clear input rejected");
				advance(hardware, md::g_samplerate * milliseconds / 1000);
			};
			key(md::PanelControl::Record, true, 100);
			key(md::PanelControl::Record, false, 100);
			for(const auto control : {md::PanelControl::Trigger5, md::PanelControl::Trigger9, md::PanelControl::Trigger13}) {
				key(control, true, 100); key(control, false, 100);
			}
			require(hardware.getFrontPanelSnapshot().getMonomachineStepLedColor(12) == md::FrontPanel::LedColor::Red,
				"clear fixture did not record a step");
			key(md::PanelControl::Function, true, 33);
			key(md::PanelControl::Play, true, 66);
			key(md::PanelControl::Play, false, 33);
			key(md::PanelControl::Function, false, 100);
			const auto cleared = hardware.getFrontPanelSnapshot();
			for(unsigned step = 0; step < 16; ++step)
				require(cleared.getMonomachineStepLedColor(step) == md::FrontPanel::LedColor::Off,
					"CLEAR left recorded triggers on the selected track");
			std::cout << "PASS: MM clear removes all selected-track triggers\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--factory-reset")
		{
			require(model == md::MachineModel::Monomachine, "MM reset fixture requires MM");
			const auto tap = [&](md::PanelControl control) {
				const auto packet = md::panelPacket(model, control).value();
				for(const auto event : {rows.press(packet), rows.release(packet)}) {
					require(device->getHardware().trySendPanelEvent(event.row, event.mask), "reset fixture input rejected");
					advance(device->getHardware(), md::g_samplerate / 10);
				}
			};
			tap(md::PanelControl::Record);
			const auto baseline = device->getFrontPanelSnapshot().getMonomachineStepLedColor(12);
			tap(md::PanelControl::Trigger13);
			require(device->getFrontPanelSnapshot().getMonomachineStepLedColor(12) != baseline,
				"reset fixture did not edit the pattern");
			auto prepared = md::Device::prepareFactoryReset(device->getPreparationContext());
			require(prepared && device->commitPreparedState(*prepared), "factory reset failed");
			prepared.reset();
			advance(device->getHardware(), md::g_samplerate * 25);
			require(device->getHardware().isFirmwareMidiReady(), "factory reset did not reboot");
			tap(md::PanelControl::Record);
			require(device->getFrontPanelSnapshot().getMonomachineStepLedColor(12) == baseline,
				"factory reset retained the edited pattern");
			std::cout << "PASS: MM factory reset discards pattern edits and reboots\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--track-mutes")
		{
			require(model == md::MachineModel::Monomachine, "MM mute fixture requires MM");
			const auto muteMask = [&]() {
				const auto panel = hardware.getFrontPanelSnapshot();
				unsigned result = 0;
				for(unsigned track = 0; track < 6; ++track) {
					const auto color = panel.getMonomachineTrackLedColor(track);
					if(color == md::FrontPanel::LedColor::Off || color == md::FrontPanel::LedColor::Yellow)
						result |= 1u << track;
				}
				return result;
			};
			const auto key = [&](md::PanelControl control, bool down, unsigned milliseconds) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "mute panel event rejected");
				advance(hardware, md::g_samplerate * milliseconds / 1000);
			};
			const auto baseline = muteMask();
			// Pad 3 must toggle track 3, including while track 1 has edit focus.
			for(unsigned track : {2u, 4u, 0u, 1u, 3u, 5u}) {
				for(unsigned toggle = 0; toggle < 2; ++toggle) {
					const auto control = static_cast<md::PanelControl>(static_cast<uint8_t>(md::PanelControl::Track1) + track);
					key(md::PanelControl::Function, true, 33);
					key(control, true, 66);
					key(control, false, 33);
					key(md::PanelControl::Function, false, 33);
					require(muteMask() == (baseline ^ (toggle == 0 ? 1u << track : 0)),
						"native mute toggled the wrong track or did not toggle");
				}
			}
			std::cout << "PASS: MM native mute toggles each numbered track and restores its state\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--md-pad-cursor")
		{
			require(model == md::MachineModel::Machinedrum, "MD cursor fixture requires MD");
			using namespace mdJucePlugin::maschine;
			const unsigned length = _argc > 3 && std::string_view(_argv[3]) == "64" ? 64 : 32;
			const unsigned pages = (1u << (length / 16)) - 1;
			const auto tap = [&](md::PanelControl control) {
				const auto packet = md::panelPacket(model, control).value();
				for(const auto event : {rows.press(packet), rows.release(packet)}) {
					require(hardware.trySendPanelEvent(event.row, event.mask), "panel event rejected");
					advance(hardware, md::g_samplerate / 10);
				}
			};
			tap(md::PanelControl::Record);
			const auto function = md::panelPacket(model, md::PanelControl::Function).value();
			const auto down = rows.press(function);
			require(hardware.trySendPanelEvent(down.row, down.mask), "function rejected");
			advance(hardware, md::g_samplerate / 10);
			tap(md::PanelControl::Scale);
			const auto up = rows.release(function);
			require(hardware.trySendPanelEvent(up.row, up.mask), "function release rejected");
			advance(hardware, md::g_samplerate / 10);
			for(int attempt = 0; attempt < 4 && occupiedScalePages(hardware.getFrontPanelSnapshot(), model) != pages; ++attempt)
				tap(md::PanelControl::Scale);
			require(occupiedScalePages(hardware.getFrontPanelSnapshot(), model) == pages, "pattern length setup failed");
			tap(md::PanelControl::Enter);
			tap(md::PanelControl::Play);
			advance(hardware, md::g_samplerate / 10);
			for(unsigned step = 2; step < length * 2 + 2; ++step) {
				const auto panel = hardware.getFrontPanelSnapshot();
				require(panel.getMachinedrumPlaybackStep() == int(step % length), "MD cursor lost native sequencer position");
				for(unsigned pad = 0; pad < 16; ++pad) {
					for(unsigned selected = 0; selected < length / 16; ++selected) {
						const auto c = machinedrumPadColor(panel, pad, true, true, true, selected);
						require((c == nihia::LedColor::Red) == (pad == step % 16 && selected == (step % length) / 16),
							"MD red record cursor wrong or leaked into unselected section");
					}
					require(machinedrumPadColor(panel, pad, false, true, false, 0) == nihia::LedColor::Off,
						"MD normal playback must not show a pad cursor");
				}
				advance(hardware, md::g_samplerate * 120 / 1000);
			}
			std::cout << "PASS: MD native cursor and pad colours across " << length << "-step wraps\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--step-pages")
		{
			require(model == md::MachineModel::Monomachine, "step-page fixture requires MM");
			using namespace mdJucePlugin::maschine;
			using Native = md::FrontPanel::LedColor;
			using Pad = nihia::LedColor;
			const auto key = [&](md::PanelControl control, bool down) {
				const auto packet = md::panelPacket(model, control).value();
				const auto event = down ? rows.press(packet) : rows.release(packet);
				require(hardware.trySendPanelEvent(event.row, event.mask), "panel event rejected");
				advance(hardware, md::g_samplerate / 10);
			};
			const auto tap = [&](md::PanelControl control) { key(control, true); key(control, false); };
			tap(md::PanelControl::Record);
			key(md::PanelControl::Function, true);
			tap(md::PanelControl::Scale);
			key(md::PanelControl::Function, false);
			for(int n = 0; n < 4 && mdJucePlugin::maschine::occupiedScalePages(hardware.getFrontPanelSnapshot(), model) != 3; ++n)
				tap(md::PanelControl::Scale);
			tap(md::PanelControl::Enter);
			for(int i : {4, 8, 12})
				tap(static_cast<md::PanelControl>(static_cast<uint8_t>(md::PanelControl::Trigger1) + i));
			key(md::PanelControl::Play, true);
			bool sawOtherSection = false, sawCursor = false;
			for(int tick = 0; tick < 80; ++tick) {
				if(tick == 1) key(md::PanelControl::Play, false);
				const auto panel = hardware.getFrontPanelSnapshot();
				sawOtherSection |= (occupiedScalePages(panel, model) & 2) != 0;
				for(int i = 0; i < 16; ++i) {
					const auto c = panel.getMonomachineStepLedColor(i);
					const auto pad = monomachinePadColor(c, true, true);
					if(c == Native::Yellow) {
						sawCursor = true;
						require(pad == Pad::Red, "grid cursor not red");
					} else {
						require(pad == (i % 4 == 0 ? Pad::Yellow : Pad::Off),
							"grid pads flickered or left a trail across sections");
					}
				}
				advance(hardware, md::g_samplerate / 10);
			}
			require(sawOtherSection && sawCursor, "multi-section playback not exercised");
			std::cout << "PASS: MM yellow trigs/red cursor remain stable across two sections\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--step-colours")
		{
			require(model == md::MachineModel::Monomachine, "step-colour fixture requires MM");
			using Color = md::FrontPanel::LedColor;
			using mdJucePlugin::maschine::monomachinePadColor;
			using Pad = mdJucePlugin::maschine::nihia::LedColor;
			const auto tap = [&](md::PanelControl control) {
				const auto packet = md::panelPacket(model, control).value();
				for(const auto event : {rows.press(packet), rows.release(packet)}) {
					require(hardware.trySendPanelEvent(event.row, event.mask), "panel event rejected");
					advance(hardware, md::g_samplerate / 10);
				}
			};
			tap(md::PanelControl::Record);
			const auto baseline = hardware.getFrontPanelSnapshot();
			require(baseline.getMonomachineStepLedColor(0) == Color::Red,
				"factory grid trig is not red");
			require(baseline.getMonomachineStepLedColor(1) == Color::Off,
				"factory second step is not empty");
			const auto playPacket = md::panelPacket(model, md::PanelControl::Play).value();
			const auto playDown = rows.press(playPacket);
			require(hardware.trySendPanelEvent(playDown.row, playDown.mask), "play rejected");
			bool sawEmptyPlayhead = false, sawEmptyAfterPlayhead = false;
			for(int tick = 0; tick < 100; ++tick) {
				if(tick == 20) {
					const auto up = rows.release(playPacket);
					require(hardware.trySendPanelEvent(up.row, up.mask), "play release rejected");
				}
				advance(hardware, md::g_samplerate / 200);
				const auto panel = hardware.getFrontPanelSnapshot();
				const auto empty = panel.getMonomachineStepLedColor(1);
				sawEmptyPlayhead |= empty == Color::Yellow;
				sawEmptyAfterPlayhead |= sawEmptyPlayhead && empty == Color::Off;
				require(monomachinePadColor(empty, true, true) == (empty == Color::Yellow ? Pad::Red : Pad::Off),
					"MM playhead left a pad trail on an empty step");
				require(monomachinePadColor(panel.getMonomachineStepLedColor(0), true, true) != Pad::Off,
					"MM recorded step disappeared during playback");
			}
			require(sawEmptyPlayhead && sawEmptyAfterPlayhead, "empty-step playhead cycle was not observed");
			std::cout << "PASS: MM grid-record step colours and no empty-step playhead trail\n";
			return 0;
		}
		if(_argc >= 3 && std::string_view(_argv[2]) == "--drums")
		{
			using namespace md::automation::sysex;
			synthLib::SMidiEvent query(synthLib::MidiEventSource::Host);
			const auto request = globalRequest(model, 0);
			query.sysex.assign(request.begin(), request.end());
			require(hardware.sendMidi(query), "global request rejected");
			advance(hardware, md::g_samplerate);
			std::vector<synthLib::SMidiEvent> midi;
			hardware.readMidiOut(midi);
			std::optional<GlobalDump> global;
			for(const auto& event : midi)
				if(auto parsed = parseGlobalDump(model, event.sysex)) global = parsed;
			require(global.has_value(), "missing MD global key map");
			constexpr uint8_t notes[] = {36,38,40,41,43,45,47,48,50,52,53,55,57,59,60,62};
			const auto select = [&](uint8_t track) {
				synthLib::SMidiEvent event(synthLib::MidiEventSource::Host);
				const auto body = md::midiProtocol::selectTrack(track);
				event.sysex = {0xf0};
				event.sysex.insert(event.sysex.end(), body.begin(), body.end());
				event.sysex.push_back(0xf7);
				require(hardware.sendMidi(event), "track selection rejected");
				advance(hardware, md::g_samplerate / 10);
				require(hardware.getFrontPanelSnapshot().getSelectedMachinedrumTrack() == track,
					"LCD selection metadata disagrees with selected track");
			};
			for(uint8_t track = 0; track < 16; ++track) {
				if(global->drumNoteMap[notes[track]] != track)
					std::cerr << "MAP " << unsigned(notes[track]) << " -> " << unsigned(global->drumNoteMap[notes[track]]) << '\n';
				require(global->drumNoteMap[notes[track]] == track, "MD global key map decoded incorrectly");
				select(track);
			}
			const auto tap = [&](md::PanelControl control) {
				const auto packet = md::panelPacket(model, control).value();
				for(const auto event : {rows.press(packet), rows.release(packet)}) {
					require(hardware.trySendPanelEvent(event.row, event.mask), "panel event rejected");
					advance(hardware, md::g_samplerate / 10);
				}
			};
			for(const uint8_t track : {0, 8}) {
				select(track);
				tap(md::PanelControl::Play);
				for(unsigned mode = 0; mode < 2; ++mode) {
					if(mode) tap(md::PanelControl::Record);
					unsigned hits = 0;
					for(unsigned tick = 0; tick < 250; ++tick) {
						advance(hardware, md::g_samplerate / 100);
						midi.clear();
						hardware.readMidiOut(midi);
						for(const auto& event : midi)
							if((event.a & 0xf0) == 0x90 && event.c && event.b == notes[track]) ++hits;
						const auto panel = hardware.getFrontPanelSnapshot();
						require(panel.getSelectedMachinedrumTrack() == track, "drum hit changed LCD selection");
						require(panel.getDrumLed(track), "selected native lamp unexpectedly changed");
					}
					require(hits > 0, "selected drum hits were not separately observable");
				}
				tap(md::PanelControl::Stop);
				tap(md::PanelControl::Record);
			}
			std::cout << "PASS: MD LCD selection for all 16 drums and independent hits in playback/record\n";
			return 0;
		}
		const auto record = md::panelPacket(model, md::PanelControl::Record);
		require(record.has_value(), "missing Record mapping");
		const auto send = [&](const bool _pressed)
		{
			const auto packet = _pressed ? rows.press(*record) : rows.release(*record);
			require(hardware.trySendPanelEvent(packet.row, packet.mask),
				"Record packet rejected");
		};
		const auto before = recordLed(hardware, model);
		const auto holdMs = _argc >= 3 ? std::strtoul(_argv[2], nullptr, 10) : 100ul;
		send(true);
		advance(hardware, static_cast<uint32_t>(md::g_samplerate * holdMs / 1000));
		send(false);
		advance(hardware, md::g_samplerate / 2);
		const auto after = recordLed(hardware, model);
		std::cout << "Record hold " << holdMs << " ms: "
			<< before << " -> " << after << '\n';
		require(before != after, "Record did not toggle after the press");
		return 0;
	}
	catch(const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
