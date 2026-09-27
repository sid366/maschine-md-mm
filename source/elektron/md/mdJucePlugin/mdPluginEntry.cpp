#include "mdPluginProcessor.h"

#if defined(MD_JUCEPLUGIN_COMBINED)
#include "mdCombinedProcessor.h"
#endif

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
	#if defined(MD_JUCEPLUGIN_SOLO_MACHINEDRUM)
	return new mdJucePlugin::CombinedProcessor(md::MachineModel::Machinedrum);
	#elif defined(MD_JUCEPLUGIN_SOLO_MONOMACHINE)
	return new mdJucePlugin::CombinedProcessor(md::MachineModel::Monomachine);
	#elif defined(MD_JUCEPLUGIN_COMBINED)
	return new mdJucePlugin::CombinedProcessor();
	#else
	return new mdJucePlugin::AudioPluginAudioProcessor();
	#endif
}
