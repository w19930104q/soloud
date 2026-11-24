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
   claim that you wrote the original software. If you use this software in a
   product, an acknowledgment in the product documentation would be
   appreciated but is not required.

   2. Altered source versions must be plainly marked as such, and must not be
   misrepresented as being the original software.

   3. This notice may not be removed or altered from any source
   distribution.
*/

#ifndef SOLOUD_CROSSFADE_H
#define SOLOUD_CROSSFADE_H

#include "soloud.h"

namespace SoLoud
{
	class Crossfade;

	// One participant of a cross-fade. Owns the sub-instance created from the
	// user's AudioSource and performs the rate conversion needed to bring the
	// sub-source to the cross-fade source's output sample rate.
	class CrossfadeSubVoice
	{
	public:
		CrossfadeSubVoice();
		~CrossfadeSubVoice();

		// Create the sub-instance and seek it to its configured start position.
		// Returns an error code without leaving a half-created instance behind.
		result init(AudioSource &aSource, time aStartOffset, float aOutputSamplerate);

		// Rewind the sub-instance back to its configured start position.
		result rewind();

		// Produce aFrames output-rate frames, converted to aChannels channels,
		// multiplied by aGain, added into aBuffer.
		void accumulate(float *aBuffer, unsigned int aFrames, unsigned int aBufferSize,
			unsigned int aChannels, float aGain);

		// True once the sub-instance has ended and all of its frames are drained.
		bool hasEnded();

	private:
		// Pull aFrames more native frames and append them to mBlock, padding
		// with silence once the sub-instance has ended.
		void pullAppend(unsigned int aFrames);

		AudioSourceInstance *mInstance;
		// Output rate this voice converts to.
		float mOutputSamplerate;
		// Native rate of the wrapped sub-source.
		float mSourceSamplerate;
		// Number of channels the wrapped sub-source produces.
		unsigned int mSourceChannels;
		// Start offset (in seconds) the sub-instance is (re)wound to.
		time mStartOffset;
		// Scratch passed to the generic seek implementation.
		float *mSeekScratch;
		unsigned int mSeekScratchSize;
		// Native frames [mBlockStart, mBlockStart+mBlockLength), planar.
		float *mBlock;
		unsigned int mBlockCapacity;
		unsigned int mBlockLength;
		unsigned int mBlockStart;
		// Fractional source position of the next output sample, in source frames.
		double mFracPos;
		// Number of real (non-padding) native frames produced by the instance.
		unsigned int mRealFrames;
		// Set once the sub-instance reports it has ended.
		bool mInstanceEnded;
	};

	class CrossfadeInstance : public AudioSourceInstance
	{
		Crossfade *mParent;
		CrossfadeSubVoice *mFrom;
		CrossfadeSubVoice *mTo;
		// Output-rate frames emitted since the fade started; reset by rewind().
		unsigned int mFadePosition;
	public:
		CrossfadeInstance(Crossfade *aParent);
		virtual ~CrossfadeInstance();
		// Build the two sub-voices from the parent source's configuration.
		// Returns an error code and cleans up on failure.
		result init();
		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
		virtual bool hasEnded();
		virtual result rewind();
	};

	// Reusable cross-fade playback source.
	//
	// Wraps two arbitrary audio sources ("from" and "to") whose sample rates,
	// channel counts and playback positions may all differ. During the fade the
	// "from" source gain ramps from aFromStartGain down to aFromEndGain while the
	// "to" source gain ramps the complementary way; afterwards only "to" is
	// audible. All rate conversion, channel matching, rewinding and parameter
	// validation is handled by the source itself.
	class Crossfade : public AudioSource
	{
	public:
		Crossfade();
		virtual ~Crossfade();
		virtual CrossfadeInstance *createInstance();

		// Configure the cross-fade. aFadeTime is the fade length in seconds at
		// the output sample rate; aFromStartGain/aFromEndGain are the "from"
		// source gains at the beginning and end of the fade (the "to" source
		// uses the complementary 1-x gains). Start offsets, in seconds, let the
		// two sub-sources begin playback from different positions.
		//
		// Validation happens before any state is touched: on error the previous
		// configuration (if any) stays intact and no half-built object exists.
		result setParams(AudioSource &aFromSource, AudioSource &aToSource,
			time aFadeTime,
			float aFromStartGain = 1.0f, float aFromEndGain = 0.0f,
			time aFromOffset = 0.0, time aToOffset = 0.0);

		// Override the output format. Defaults to the "to" source's format.
		// Only the channel layouts the engine supports (1, 2, 4, 6, 8) are
		// accepted.
		result setOutputParams(float aSamplerate, unsigned int aChannels);

		AudioSource *mFromSource;
		AudioSource *mToSource;
		time mFadeTime;
		float mFromStartGain;
		float mFromEndGain;
		time mFromOffset;
		time mToOffset;
	};
};

#endif
