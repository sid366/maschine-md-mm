#pragma once

#include "mdMaschineController.h"
#include "mdCombinedMidiRouter.h"
#include "mdPluginProcessor.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace mdJucePlugin
{
	class CombinedProcessor final : public juce::AudioProcessor
	{
	public:
		// With a solo model only that instrument is created, so a single-machine
		// product keeps the Maschine integration without emulating the other one.
		explicit CombinedProcessor(std::optional<md::MachineModel> _soloModel = std::nullopt);
		~CombinedProcessor() override;

		void setFocusedModel(md::MachineModel _model) { m_maschine.setFocusedModel(_model); }

		// Null when that instrument is not part of this product.
		AudioPluginAudioProcessor* machinedrum() { return m_machinedrum.get(); }
		AudioPluginAudioProcessor* monomachine() { return m_monomachine.get(); }
		AudioPluginAudioProcessor* processorFor(md::MachineModel _model)
		{
			return _model == md::MachineModel::Monomachine ? monomachine() : machinedrum();
		}
		std::optional<md::MachineModel> soloModel() const { return m_soloModel; }

		struct SysexCapture
		{
			md::MachineModel model;
			std::vector<uint8_t> bytes;
			bool incomplete = false;
		};
		bool beginSysexCapture(md::MachineModel _model);
		std::optional<SysexCapture> endSysexCapture();
		bool isSysexCapturing() const { return m_captureEnabled.load(); }

		void prepareToPlay(double _sampleRate, int _maximumBlockSize) override;
		void releaseResources() override;
		void processBlock(juce::AudioBuffer<float>& _audio,
			juce::MidiBuffer& _midi) override;
		bool isBusesLayoutSupported(const BusesLayout& _layouts) const override;

		juce::AudioProcessorEditor* createEditor() override;
		bool hasEditor() const override { return true; }
		const juce::String getName() const override { return PluginName; }
		bool acceptsMidi() const override { return true; }
		bool producesMidi() const override { return true; }
		bool isMidiEffect() const override { return false; }
		double getTailLengthSeconds() const override { return 0.0; }

		int getNumPrograms() override { return 1; }
		int getCurrentProgram() override { return 0; }
		void setCurrentProgram(int) override {}
		const juce::String getProgramName(int) override { return {}; }
		void changeProgramName(int, const juce::String&) override {}
		void getStateInformation(juce::MemoryBlock& _destination) override;
		void setStateInformation(const void* _data, int _size) override;

	private:
		static constexpr int g_machinedrumOutput = 0;
		static constexpr int g_monomachineOutput = 1;
		static constexpr int g_mixOutput = 2;
		struct OutputSlot
		{
			const char* name;
			int width;		// 2 = stereo, 1 = mono
			int machine;	// g_machinedrumOutput, g_monomachineOutput or g_mixOutput
			int source;		// first channel of the machine's six outputs A-F
			bool pluginOnly = false;	// already part of a stereo pair in Standalone
		};
		static std::vector<OutputSlot> outputSlots(std::optional<md::MachineModel> _soloModel);
		static int countStandaloneChannels(const std::vector<OutputSlot>& _slots);
		static BusesProperties createBuses(std::optional<md::MachineModel> _soloModel,
			bool _standaloneOutputs);
		void writeOutputs(juce::AudioBuffer<float>& _audio, int _samples);
		void runMonomachineWorker();
		void captureSysex(const juce::MidiBuffer& _midi, md::MachineModel _model);
		void runFastBoot(AudioPluginAudioProcessor& _processor,
			std::atomic<bool>& _active);
		void stopFastBootWorkers();

		const std::optional<md::MachineModel> m_soloModel;
		const bool m_standaloneOutputs;
		const std::vector<OutputSlot> m_outputSlots;
		const int m_outputChannels;
		std::unique_ptr<AudioPluginAudioProcessor> m_machinedrum;
		std::unique_ptr<AudioPluginAudioProcessor> m_monomachine;
		maschine::Controller m_maschine;
		juce::AudioBuffer<float> m_mdAudio;
		juce::AudioBuffer<float> m_mmAudio;
		juce::MidiBuffer m_mdMidi;
		juce::MidiBuffer m_mmMidi;
		CombinedMidiRouter m_midiRouter;
		std::mutex m_captureMutex;
		std::vector<uint8_t> m_captureBytes;
		std::atomic<bool> m_captureEnabled{false};
		std::atomic<bool> m_captureIncomplete{false};
		std::atomic<md::MachineModel> m_captureModel{md::MachineModel::Machinedrum};
		bool m_capturePreviousHostRoute = false;
		juce::WaitableEvent m_mmWorkReady;
		juce::WaitableEvent m_mmWorkFinished;
		std::atomic<bool> m_stoppingWorker{false};
		std::thread m_mmWorker;
		std::atomic<bool> m_stopFastBoot{false};
		std::atomic<bool> m_mdFastBootActive{false};
		std::atomic<bool> m_mmFastBootActive{false};
		std::thread m_mdFastBootWorker;
		std::thread m_mmFastBootWorker;
		int m_maximumBlockSize = 0;

		JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CombinedProcessor)
	};
}
