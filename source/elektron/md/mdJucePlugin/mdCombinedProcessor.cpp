#include "mdCombinedProcessor.h"

#include "mdCombinedEditor.h"
#include "mdLib/mddevice.h"
#include "mdLib/mdsysexfile.h"

#include "dsp56kBase/threadtools.h"

#include <algorithm>
#include <chrono>
#include <cstdio>

namespace mdJucePlugin
{
	namespace
	{
		constexpr uint32_t g_stateMagic = 0x4d444d4d;
		constexpr int g_stateVersion = 1;

		void prepareChild(juce::AudioProcessor& _processor,
			const double _sampleRate, const int _blockSize)
		{
			_processor.setPlayConfigDetails(2, 2, _sampleRate, _blockSize);
			_processor.prepareToPlay(_sampleRate, _blockSize);
		}

		std::unique_ptr<AudioPluginAudioProcessor> createMachine(
			const md::MachineModel _model,
			const std::optional<md::MachineModel> _soloModel)
		{
			if(_soloModel && *_soloModel != _model)
				return {};
			return std::make_unique<AudioPluginAudioProcessor>(_model, false);
		}
	}

	// Each machine renders its six outputs A-F as on the hardware: A/B is the
	// main pair (channels 0/1), C-F are the individual outputs (2-5). The
	// product's outputs are listed below. Plug-in hosts get one bus per entry,
	// including A and B on their own. JUCE Standalone only opens the main bus, so
	// there the other entries form one main bus in this order. The first entry
	// is always the stereo main output, so a stereo device plays the main mix.
	std::vector<CombinedProcessor::OutputSlot> CombinedProcessor::outputSlots(
		const std::optional<md::MachineModel> _soloModel)
	{
		const auto machineOutputs = [](std::vector<OutputSlot>& _slots,
			const int _machine, const char* const (&_names)[7])
		{
			_slots.push_back({_names[0], 2, _machine, 0});
			for(int output = 0; output < 6; ++output)
				_slots.push_back({_names[output + 1], 1, _machine, output, output < 2});
		};
		std::vector<OutputSlot> slots;
		if(_soloModel)
		{
			static const char* const names[7] = {"Main A/B",
				"Out A", "Out B", "Out C", "Out D", "Out E", "Out F"};
			machineOutputs(slots, *_soloModel == md::MachineModel::Monomachine
				? g_monomachineOutput : g_machinedrumOutput, names);
			return slots;
		}
		static const char* const mdNames[7] = {"MD Main A/B",
			"MD Out A", "MD Out B", "MD Out C", "MD Out D", "MD Out E", "MD Out F"};
		static const char* const mmNames[7] = {"MM Main A/B",
			"MM Out A", "MM Out B", "MM Out C", "MM Out D", "MM Out E", "MM Out F"};
		slots.push_back({"Main", 2, g_mixOutput, 0});
		machineOutputs(slots, g_machinedrumOutput, mdNames);
		machineOutputs(slots, g_monomachineOutput, mmNames);
		return slots;
	}

	int CombinedProcessor::countStandaloneChannels(const std::vector<OutputSlot>& _slots)
	{
		int channels = 0;
		for(const auto& slot : _slots)
			if(!slot.pluginOnly)
				channels += slot.width;
		return channels;
	}

	juce::AudioProcessor::BusesProperties CombinedProcessor::createBuses(
		const std::optional<md::MachineModel> _soloModel, const bool _standaloneOutputs)
	{
		auto buses = BusesProperties()
			.withInput("Input A/B", juce::AudioChannelSet::stereo(), true);
		const auto slots = outputSlots(_soloModel);
		if(_standaloneOutputs)
		{
			const auto channels = countStandaloneChannels(slots);
			return buses.withOutput("Outputs 1-" + juce::String(channels),
				juce::AudioChannelSet::discreteChannels(channels), true);
		}
		for(size_t slot = 0; slot < slots.size(); ++slot)
			buses = buses.withOutput(slots[slot].name, slots[slot].width == 2
				? juce::AudioChannelSet::stereo() : juce::AudioChannelSet::mono(),
				slot == 0);
		return buses;
	}

	CombinedProcessor::CombinedProcessor(const std::optional<md::MachineModel> _soloModel)
		: AudioProcessor(createBuses(_soloModel,
			juce::JUCEApplicationBase::isStandaloneApp()))
		, m_soloModel(_soloModel)
		, m_standaloneOutputs(juce::JUCEApplicationBase::isStandaloneApp())
		, m_outputSlots(outputSlots(_soloModel))
		, m_outputChannels(countStandaloneChannels(m_outputSlots))
		, m_machinedrum(createMachine(md::MachineModel::Machinedrum, _soloModel))
		, m_monomachine(createMachine(md::MachineModel::Monomachine, _soloModel))
		, m_maschine(m_machinedrum.get(), m_monomachine.get())
	{
		// A solo product runs its one machine on the host audio thread.
		if(m_machinedrum && m_monomachine)
			m_mmWorker = std::thread([this] { runMonomachineWorker(); });
	}

	CombinedProcessor::~CombinedProcessor()
	{
		if(isSysexCapturing())
			(void)endSysexCapture();
		stopFastBootWorkers();
		m_stoppingWorker.store(true, std::memory_order_release);
		m_mmWorkReady.signal();
		if(m_mmWorker.joinable())
			m_mmWorker.join();
	}

	bool CombinedProcessor::beginSysexCapture(const md::MachineModel _model)
	{
		if(isSysexCapturing())
			return false;
		auto* const machine = processorFor(_model);
		if(!machine)
			return false;
		{
			std::lock_guard lock(m_captureMutex);
			m_captureBytes.clear();
			m_captureBytes.reserve(md::g_midiSysexTransferMaxBytes);
			m_captureIncomplete.store(false, std::memory_order_relaxed);
			m_captureModel.store(_model, std::memory_order_release);
		}
		auto& routing = machine->getMidiRoutingMatrix();
		using Source = synthLib::MidiEventSource;
		using Type = synthLib::MidiRoutingMatrix::EventType;
		m_capturePreviousHostRoute = routing.enabled(Source::Device,
			Source::Host, Type::SysEx);
		m_captureEnabled.store(true, std::memory_order_release);
		routing.setEnabled(Source::Device, Source::Host, Type::SysEx, true);
		return true;
	}

	std::optional<CombinedProcessor::SysexCapture>
	CombinedProcessor::endSysexCapture()
	{
		if(!m_captureEnabled.exchange(false, std::memory_order_acq_rel))
			return std::nullopt;
		const auto model = m_captureModel.load(std::memory_order_acquire);
		using Source = synthLib::MidiEventSource;
		using Type = synthLib::MidiRoutingMatrix::EventType;
		if(auto* const machine = processorFor(model))
			machine->getMidiRoutingMatrix().setEnabled(Source::Device,
				Source::Host, Type::SysEx, m_capturePreviousHostRoute);
		std::lock_guard lock(m_captureMutex);
		return SysexCapture{model, std::move(m_captureBytes),
			m_captureIncomplete.load(std::memory_order_relaxed)};
	}

	void CombinedProcessor::captureSysex(const juce::MidiBuffer& _midi,
		const md::MachineModel _model)
	{
		if(!m_captureEnabled.load(std::memory_order_acquire)
			|| m_captureModel.load(std::memory_order_acquire) != _model)
			return;
		for(const auto event : _midi)
		{
			const auto* bytes = event.data;
			const auto size = static_cast<size_t>(event.numBytes);
			if(size < 9 || bytes[0] != 0xf0 || bytes[size - 1] != 0xf7)
				continue;
			// Only user dumps and sample dumps belong in an exported file.
			const auto elektronHeader = bytes[1] == 0x00
				&& bytes[2] == 0x20 && bytes[3] == 0x3c
				&& bytes[4] == (_model == md::MachineModel::Machinedrum ? 2 : 3);
			const auto elektron = elektronHeader
				&& ((size >= 15 && (bytes[6] == 0x50 || bytes[6] == 0x52
					|| bytes[6] == 0x67 || bytes[6] == 0x69
					|| bytes[6] == 0x5d))
					|| (size == 13 && bytes[6] == 0x73));
			const auto sample = _model == md::MachineModel::Machinedrum
				&& bytes[1] == 0x7e && (bytes[3] == 1 || bytes[3] == 2);
			if(!elektron && !sample)
				continue;
			std::unique_lock lock(m_captureMutex, std::try_to_lock);
			if(!lock.owns_lock())
			{
				if(m_captureEnabled.load(std::memory_order_relaxed))
					m_captureIncomplete.store(true, std::memory_order_relaxed);
				continue;
			}
			if(!m_captureEnabled.load(std::memory_order_relaxed))
				return;
			if(size > md::g_midiSysexTransferMaxBytes - m_captureBytes.size())
			{
				m_captureIncomplete.store(true, std::memory_order_relaxed);
				continue;
			}
			m_captureBytes.insert(m_captureBytes.end(), bytes, bytes + size);
		}
	}

	void CombinedProcessor::stopFastBootWorkers()
	{
		m_stopFastBoot.store(true, std::memory_order_release);
		if(m_mdFastBootWorker.joinable())
			m_mdFastBootWorker.join();
		if(m_mmFastBootWorker.joinable())
			m_mmFastBootWorker.join();
		m_mdFastBootActive.store(false, std::memory_order_release);
		m_mmFastBootActive.store(false, std::memory_order_release);
	}

	void CombinedProcessor::runFastBoot(AudioPluginAudioProcessor& _processor,
		std::atomic<bool>& _active)
	{
		constexpr uint32_t chunkFrames = 256;
		constexpr uint32_t maximumFrames = md::g_samplerate * 25;
		constexpr uint32_t stableFrames = md::g_samplerate * 3 / 2;
		auto& plugin = _processor.getPlugin();
		const auto started = std::chrono::steady_clock::now();
		uint32_t frames = 0;
		uint32_t readySince = 0;
		bool finished = false;
		bool active = false;

		while(!m_stopFastBoot.load(std::memory_order_acquire)
			&& std::chrono::steady_clock::now() - started < std::chrono::seconds(30)
			&& frames < maximumFrames && !finished)
		{
			// A restored project may still be booting its replacement hardware.
			// Let its normal audio path finish before taking ownership of boot.
			const auto state = plugin.tryWithDeviceLocked(
				[](synthLib::Device* const _device)
				{
					const auto* const device = dynamic_cast<md::Device*>(_device);
					if(!device || !device->isValid()) return -1;
					if(device->isProjectStateRestorePending()) return 0;
					if(device->getHardware().isFactoryFlashInitializationExpected())
						return -1;
					if(device->getHardware().isFirmwareMidiReady()
						&& device->getFrontPanelSnapshot().countLitPixels() >= 2000)
						return 2;
					return 1;
				});
			if(!state || *state == 0)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
				continue;
			}
			if(*state < 0)
				break;
			if(*state == 2 && !active)
				break;

			if(!active)
			{
				_active.store(true, std::memory_order_release);
				active = true;
			}

			const auto advanced = plugin.withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					auto* const device = dynamic_cast<md::Device*>(_device);
					if(!device || device->isProjectStateRestorePending())
						return false;
					device->getHardware().advance(chunkFrames);
					return true;
				});
			if(!advanced)
				break;
			frames += chunkFrames;

			if((frames % 2048) != 0)
				continue;
			finished = plugin.withDeviceLocked(
				[&](synthLib::Device* const _device)
				{
					const auto* const device = dynamic_cast<md::Device*>(_device);
					if(!device) return false;
					const auto& hardware = device->getHardware();
					const bool ready = hardware.isFirmwareMidiReady()
						&& hardware.getFrontPanelSnapshot().countLitPixels() >= 2000;
					if(!ready)
						readySince = 0;
					else if(readySince == 0)
						readySince = frames;
					return readySince != 0 && frames - readySince >= stableFrames;
				});
		}

		_active.store(false, std::memory_order_release);
		if(active)
		{
			const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				std::chrono::steady_clock::now() - started).count();
			std::fprintf(stderr, "[MD-MM] %s fast boot: %u frames in %lld ms (%s)\n",
				_processor.getModel() == md::MachineModel::Machinedrum ? "MD" : "MM",
				frames, static_cast<long long>(elapsed), finished ? "ready" : "fallback");
		}
	}

	void CombinedProcessor::runMonomachineWorker()
	{
		dsp56k::ThreadTools::setCurrentThreadPriority(
			dsp56k::ThreadPriority::Highest);
		for(;;)
		{
			m_mmWorkReady.wait();
			if(m_stoppingWorker.load(std::memory_order_acquire))
				return;
			static_cast<juce::AudioProcessor&>(*m_monomachine).processBlock(
				m_mmAudio, m_mmMidi);
			m_mmWorkFinished.signal();
		}
	}

	bool CombinedProcessor::isBusesLayoutSupported(
		const BusesLayout& _layouts) const
	{
		const auto input = _layouts.getMainInputChannelSet();
		if(_layouts.inputBuses.size() != 1
			|| !(input.isDisabled() || input == juce::AudioChannelSet::stereo()))
			return false;
		if(m_standaloneOutputs)
		{
			// Keep every output channel whatever the device offers. JUCE's player
			// otherwise switches the processor to a stereo device's layout, and
			// Standalone then never offers more than two outputs again. On a
			// stereo device the player simply passes on the first two (main).
			return _layouts.outputBuses.size() == 1
				&& _layouts.getMainOutputChannelSet().size() == m_outputChannels;
		}
		if(_layouts.outputBuses.size() != static_cast<int>(m_outputSlots.size())
			|| _layouts.getMainOutputChannelSet() != juce::AudioChannelSet::stereo())
			return false;
		for(int bus = 1; bus < _layouts.outputBuses.size(); ++bus)
		{
			const auto channels = _layouts.getChannelSet(false, bus);
			if(!channels.isDisabled()
				&& channels.size() != m_outputSlots[static_cast<size_t>(bus)].width)
				return false;
		}
		return true;
	}

	void CombinedProcessor::prepareToPlay(const double _sampleRate,
		const int _maximumBlockSize)
	{
		stopFastBootWorkers();
		m_maximumBlockSize = std::max(1, _maximumBlockSize);
		m_mdMidi.ensureSize(4096);
		m_mmMidi.ensureSize(4096);
		m_midiRouter.reset();
		int latency = 0;
		for(auto* const machine : {m_machinedrum.get(), m_monomachine.get()})
		{
			if(!machine)
				continue;
			// Every machine renders all three of its output pairs; processBlock()
			// hands them to this product's outputs.
			auto machineLayout = machine->getBusesLayout();
			machineLayout.inputBuses.set(0, getBusesLayout().getMainInputChannelSet().isDisabled()
				? juce::AudioChannelSet::disabled() : juce::AudioChannelSet::stereo());
			for(int bus = 0; bus < machineLayout.outputBuses.size(); ++bus)
				machineLayout.outputBuses.set(bus, juce::AudioChannelSet::stereo());
			if(!machine->setBusesLayout(machineLayout))
				prepareChild(*machine, _sampleRate, m_maximumBlockSize);
			else
			{
				machine->setRateAndBufferSizeDetails(_sampleRate, m_maximumBlockSize);
				static_cast<juce::AudioProcessor&>(*machine).prepareToPlay(
					_sampleRate, m_maximumBlockSize);
			}
			(machine == m_machinedrum.get() ? m_mdAudio : m_mmAudio).setSize(
				std::max(machine->getTotalNumInputChannels(),
					machine->getTotalNumOutputChannels()),
				m_maximumBlockSize, false, true);
			latency = std::max(latency, machine->getLatencySamples());
		}
		setLatencySamples(latency);
		if(juce::JUCEApplicationBase::isStandaloneApp())
		{
			m_stopFastBoot.store(false, std::memory_order_release);
			if(m_machinedrum)
				m_mdFastBootWorker = std::thread([this]
					{ runFastBoot(*m_machinedrum, m_mdFastBootActive); });
			if(m_monomachine)
				m_mmFastBootWorker = std::thread([this]
					{ runFastBoot(*m_monomachine, m_mmFastBootActive); });
		}
	}

	void CombinedProcessor::releaseResources()
	{
		stopFastBootWorkers();
		for(auto* const machine : {m_machinedrum.get(), m_monomachine.get()})
			if(machine)
				static_cast<juce::AudioProcessor&>(*machine).releaseResources();
	}

	void CombinedProcessor::processBlock(juce::AudioBuffer<float>& _audio,
		juce::MidiBuffer& _midi)
	{
		juce::ScopedNoDenormals noDenormals;
		const auto samples = _audio.getNumSamples();
		if(samples <= 0)
			return;
		if(samples > m_maximumBlockSize || _audio.getNumChannels() < 2)
		{
			_audio.clear();
			_midi.clear();
			return;
		}

		// Each machine gets the host input and renders into its own buffer.
		const auto prepareMachineBuffer = [&](juce::AudioBuffer<float>& _buffer,
			const AudioPluginAudioProcessor* const _machine)
		{
			if(!_machine)
				return;
			_buffer.setSize(std::max(_machine->getTotalNumInputChannels(),
				_machine->getTotalNumOutputChannels()), samples, false, false, true);
			_buffer.clear();
			const auto inputs = std::min(_machine->getTotalNumInputChannels(),
				_audio.getNumChannels());
			for(int channel = 0; channel < inputs; ++channel)
				_buffer.copyFrom(channel, 0, _audio, channel, 0, samples);
		};
		prepareMachineBuffer(m_mdAudio, m_machinedrum.get());
		prepareMachineBuffer(m_mmAudio, m_monomachine.get());
		m_mdMidi.clear();
		m_mmMidi.clear();
		const auto focused = m_maschine.focusedModel();
		const auto mmNoteChannel = m_monomachine
			? m_monomachine->getMonomachineNoteChannel(
				m_maschine.selectedMonomachineTrack())
			: uint8_t{0xff};
		for(const auto event : _midi)
		{
			const auto message = event.getMessage();
			const auto* const bytes = message.getRawData();
			const auto size = message.getRawDataSize();
			if(!bytes || size <= 0 || event.samplePosition >= samples)
				continue;
			const auto destinations = m_midiRouter.route(bytes[0],
				size > 1 ? bytes[1] : 0, size > 2 ? bytes[2] : 0, focused);
			if(m_machinedrum && (destinations & CombinedMidiRouter::machinedrum))
				m_mdMidi.addEvent(message, event.samplePosition);
			if(m_monomachine && (destinations & CombinedMidiRouter::monomachine))
			{
				const auto channels = m_midiRouter.monomachineChannels(bytes[0],
					size > 1 ? bytes[1] : 0, size > 2 ? bytes[2] : 0,
					mmNoteChannel);
				if(bytes[0] >= 0xf0)
					m_mmMidi.addEvent(message, event.samplePosition);
				else
				{
					for(int channel = 0; channel < 16; ++channel)
					{
						if((channels & (1u << channel)) == 0)
							continue;
						auto routed = message;
						routed.setChannel(channel + 1);
						m_mmMidi.addEvent(routed, event.samplePosition);
					}
				}
			}
		}

		// The machines do not share mutable emulation state. With both present,
		// run MM on its persistent high-priority worker while the host audio thread
		// runs MD, then join at the block boundary. This keeps the heavier engine
		// from serially consuming the other engine's deadline budget.
		const bool runMd = m_machinedrum
			&& !m_mdFastBootActive.load(std::memory_order_acquire);
		const bool runMm = m_monomachine
			&& !m_mmFastBootActive.load(std::memory_order_acquire);
		const bool mmOnWorker = runMm && m_machinedrum;
		if(mmOnWorker)
			m_mmWorkReady.signal();
		if(runMd)
			static_cast<juce::AudioProcessor&>(*m_machinedrum).processBlock(
				m_mdAudio, m_mdMidi);
		else
		{
			m_mdAudio.clear();
			m_mdMidi.clear();
		}
		if(mmOnWorker)
			m_mmWorkFinished.wait();
		else if(runMm)
			static_cast<juce::AudioProcessor&>(*m_monomachine).processBlock(
				m_mmAudio, m_mmMidi);
		else
		{
			m_mmAudio.clear();
			m_mmMidi.clear();
		}
		captureSysex(m_mdMidi, md::MachineModel::Machinedrum);
		captureSysex(m_mmMidi, md::MachineModel::Monomachine);

		writeOutputs(_audio, samples);
		_midi.clear();
		_midi.addEvents(m_mdMidi, 0, samples, 0);
		_midi.addEvents(m_mmMidi, 0, samples, 0);
	}

	void CombinedProcessor::writeOutputs(juce::AudioBuffer<float>& _audio,
		const int _samples)
	{
		_audio.clear();
		int standaloneChannel = 0;
		for(size_t slotIndex = 0; slotIndex < m_outputSlots.size(); ++slotIndex)
		{
			const auto& slot = m_outputSlots[slotIndex];
			if(m_standaloneOutputs && slot.pluginOnly)
				continue;
			// Standalone: consecutive channels of the single main bus (as many as
			// the device opened). Plug-in: this slot's own bus, if enabled.
			auto target = m_standaloneOutputs
				? juce::AudioBuffer<float>(_audio.getArrayOfWritePointers()
					+ std::min(standaloneChannel, _audio.getNumChannels()),
					std::clamp(_audio.getNumChannels() - standaloneChannel, 0, slot.width),
					_samples)
				: getBusBuffer(_audio, false, static_cast<int>(slotIndex));
			standaloneChannel += slot.width;
			const auto channels = std::min(target.getNumChannels(), slot.width);
			for(int channel = 0; channel < channels; ++channel)
			{
				const auto source = slot.source + channel;
				if(slot.machine == g_mixOutput)
				{
					// Both machines' main outputs, each at half level.
					if(source < m_mdAudio.getNumChannels())
						target.addFrom(channel, 0, m_mdAudio, source, 0, _samples, 0.5f);
					if(source < m_mmAudio.getNumChannels())
						target.addFrom(channel, 0, m_mmAudio, source, 0, _samples, 0.5f);
					continue;
				}
				const auto& buffer = slot.machine == g_monomachineOutput ? m_mmAudio : m_mdAudio;
				if(source < buffer.getNumChannels())
					target.copyFrom(channel, 0, buffer, source, 0, _samples);
			}
		}
	}

	juce::AudioProcessorEditor* CombinedProcessor::createEditor()
	{
		return new CombinedEditor(*this);
	}

	void CombinedProcessor::getStateInformation(juce::MemoryBlock& _destination)
	{
		juce::MemoryBlock mdState;
		juce::MemoryBlock mmState;
		if(m_machinedrum)
			static_cast<juce::AudioProcessor&>(*m_machinedrum).getStateInformation(mdState);
		if(m_monomachine)
			static_cast<juce::AudioProcessor&>(*m_monomachine).getStateInformation(mmState);
		juce::MemoryOutputStream stream(_destination, false);
		stream.writeInt(static_cast<int>(g_stateMagic));
		stream.writeInt(g_stateVersion);
		stream.writeByte(m_maschine.focusedModel() == md::MachineModel::Monomachine
			? 1 : 0);
		stream.writeInt(static_cast<int>(mdState.getSize()));
		stream.write(mdState.getData(), mdState.getSize());
		stream.writeInt(static_cast<int>(mmState.getSize()));
		stream.write(mmState.getData(), mmState.getSize());
	}

	void CombinedProcessor::setStateInformation(const void* const _data,
		const int _size)
	{
		if(!_data || _size <= 0)
			return;
		juce::MemoryInputStream stream(_data, static_cast<size_t>(_size), false);
		if(static_cast<uint32_t>(stream.readInt()) != g_stateMagic
			|| stream.readInt() != g_stateVersion)
			return;
		m_maschine.setFocusedModel(stream.readByte() != 0
			? md::MachineModel::Monomachine : md::MachineModel::Machinedrum);
		const auto mdSize = stream.readInt();
		if(mdSize < 0 || static_cast<int64_t>(mdSize) > stream.getNumBytesRemaining())
			return;
		juce::MemoryBlock mdState(static_cast<size_t>(mdSize));
		if(stream.read(mdState.getData(), mdState.getSize()) != mdSize)
			return;
		const auto mmSize = stream.readInt();
		if(mmSize < 0 || static_cast<int64_t>(mmSize) > stream.getNumBytesRemaining())
			return;
		juce::MemoryBlock mmState(static_cast<size_t>(mmSize));
		if(stream.read(mmState.getData(), mmState.getSize()) != mmSize)
			return;
		// An empty block belongs to a machine absent from the saving product.
		if(m_machinedrum && mdState.getSize() > 0)
			static_cast<juce::AudioProcessor&>(*m_machinedrum).setStateInformation(
				mdState.getData(), static_cast<int>(mdState.getSize()));
		if(m_monomachine && mmState.getSize() > 0)
			static_cast<juce::AudioProcessor&>(*m_monomachine).setStateInformation(
				mmState.getData(), static_cast<int>(mmState.getSize()));
	}
}
