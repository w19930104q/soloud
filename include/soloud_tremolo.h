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

#ifndef SOLOUD_TREMOLO_H
#define SOLOUD_TREMOLO_H

#include "soloud.h"
#include "soloud_audiosource.h"

namespace SoLoud
{
	class Tremolo;

	// Wrapper instance: pulls audio from the wrapped source and applies
	// a per-frame LFO gain envelope. The LFO phase is kept in a double
	// accumulator advanced once per produced frame, so the oscillation
	// depends only on rate / samplerate and never on the buffer sizes the
	// engine happens to pull.
	class TremoloInstance : public AudioSourceInstance
	{
	public:
		TremoloInstance(Tremolo *aParent);
		~TremoloInstance();

		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
		virtual bool hasEnded();
		virtual result rewind();

		// LFO phase in cycles, [0, 1). Starts at zero and wraps with fmod.
		double mPhase;

	private:
		AudioSourceInstance *mInstance;
		float mRate;
		float mDepth;
		// Cycle offset (in [0, 0.5]) applied to every channel except channel 0.
		float mChannelOffset;
		float mPhaseDelta;
	};

	// Configurable tremolo playback source. Wraps another AudioSource and
	// modulates its gain with a triangle LFO:
	//   gain = (1 - depth) + depth * |2 * phase - 1|
	// so depth 1.0 starts every cycle at full gain and reaches silence at
	// the half-cycle point. Rate is in Hz; channelPhase shifts every extra
	// channel (e.g. the right channel of a stereo source) by up to half a
	// cycle, enabling inverted left/right swing.
	class Tremolo : public AudioSource
	{
	public:
		enum CHANNELPHASE
		{
			// All channels swing together.
			IN_PHASE = 0,
			// Extra channels swing half a cycle out of phase with channel 0.
			INVERTED = 1
		};

		Tremolo();
		virtual ~Tremolo();

		// Set the wrapped source. Rejects sources whose samplerate is not
		// a finite positive number.
		result setSource(AudioSource &aSource);
		// LFO rate in Hz. Must be finite and > 0.
		result setRate(float aRate);
		// Modulation depth, 0 (no effect) .. 1 (gain swings to silence).
		result setDepth(float aDepth);
		// Phase shift of extra channels relative to channel 0, 0 .. 1,
		// mapped internally to a 0 .. 0.5 cycle offset.
		result setChannelPhase(float aChannelPhase);
		// Configure everything at once. The whole call is validated first;
		// on failure the previous configuration is left untouched.
		result setParams(AudioSource &aSource, float aRate, float aDepth, float aChannelPhase);

		virtual AudioSourceInstance *createInstance();

		AudioSource *mSource;
		float mRate;
		float mDepth;
		float mChannelPhase;
	};
};

#endif
