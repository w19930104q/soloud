/*
SoLoud audio engine
Copyright (c) 2013-2020 Jari Komppa

This software is provided 'as-is', without any express or implied
warranty. In no event will the authors be held liable for any damages
arising from the use of this software.

Permission is granted to anyone to use this software for any purpose,
including commercial applications, and to alter it and redistribute it
freely, subject to the following restrictions:

   1. The origin of this software must not be misrepresented; you must not
   claim that you wrote the original software. If you use this software
   in a product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#ifndef SOLOUD_NOISEGATE_H
#define SOLOUD_NOISEGATE_H

#include "soloud.h"

namespace SoLoud
{
	class NoiseGate;

	// Noise gate instance. Wraps an instance of the input source and
	// zeroes the output while the gate is closed. The gate decision is
	// made once per sample frame from the loudest channel, so all
	// channels of a frame are always passed or muted together (linked
	// channels) instead of each channel gating on its own.
	class NoiseGateInstance : public AudioSourceInstance
	{
	public:
		NoiseGateInstance(NoiseGate *aParent);
		virtual ~NoiseGateInstance();

		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
		virtual bool hasEnded();
		virtual result rewind();

	public:
		// Parent noise gate source
		NoiseGate *mParent;
		// Instance of the input source we're gating. Owned.
		AudioSourceInstance *mSource;
		// Current gate state: true = open (signal passes), false = closed (output zeroed)
		bool mGateOpen;
		// Samples of hold time left before the gate is allowed to close
		unsigned int mHoldSamplesLeft;
	};

	// Noise gate audio source. Gates another audio source: signal at or
	// above the open threshold passes through, signal below the close
	// threshold is muted after the hold time has elapsed. Separate open
	// and close thresholds provide hysteresis so the gate doesn't
	// chatter on signals hovering around a single threshold.
	class NoiseGate : public AudioSource
	{
	public:
		NoiseGate();
		NoiseGate(AudioSource *aSource);
		virtual ~NoiseGate();

		// Set the audio source to gate. The source is not owned by the
		// gate and must outlive any playing gate instances.
		result setSource(AudioSource *aSource);
		// Set the open and close thresholds as linear amplitude (0..1).
		// Signal at or above the open threshold opens the gate; signal
		// below the close threshold starts the hold countdown to close.
		// The close threshold is clamped to not exceed the open one.
		void setThresholds(float aOpenThreshold, float aCloseThreshold);
		// Set how long (in seconds) the gate stays open after the signal
		// falls below the close threshold, so weak tails aren't cut off.
		void setHoldTime(float aHoldTime);

		virtual AudioSourceInstance *createInstance();

	public:
		// Input source to gate. Not owned.
		AudioSource *mSource;
		// Gate opens at or above this amplitude
		float mOpenThreshold;
		// Gate (starts to) close below this amplitude
		float mCloseThreshold;
		// Hold time in seconds before the gate closes
		float mHoldTime;
	};
};

#endif
