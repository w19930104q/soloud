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

#ifndef SOLOUD_LIMITER_H
#define SOLOUD_LIMITER_H

#include "soloud.h"

namespace SoLoud
{
	class Limiter;

	// Instance of the peak limiter source. Wraps a child source instance,
	// delays it by the lookahead time and applies one common gain to all
	// channels so the stereo image does not wander.
	class LimiterInstance : public AudioSourceInstance
	{
	protected:
		// Parent limiter source (parameters)
		Limiter *mParent;
		// Owned child source instance
		AudioSourceInstance *mSource;
		// Planar delay buffer, mDelaySize samples per channel
		float *mDelayBuf;
		// Allocated samples per channel in mDelayBuf
		unsigned int mDelaySize;
		// Lookahead window length in samples
		unsigned int mLookaheadSamples;
		// Index of the oldest valid sample in mDelayBuf
		unsigned int mDelayHead;
		// Number of valid samples in mDelayBuf
		unsigned int mDelayCount;
		// Current limiter gain, shared by all channels
		float mGain;
		// Maximum gain increase per sample (release window slope)
		float mReleaseStep;
		// Maximum gain decrease per sample (lookahead attack ramp slope)
		float mAttackStep;
		// Child source has ended (and possibly been drained)
		bool mSourceEnded;
		// (Re)allocate the delay buffer and compute slopes. Called lazily
		// on first getAudio() so mChannels/mSamplerate are valid.
		void initBuffer();
		// Top up the delay buffer from the child source.
		void topUp();
	public:
		LimiterInstance(Limiter *aParent, AudioSourceInstance *aSource);
		virtual ~LimiterInstance();
		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
		virtual bool hasEnded();
		// Rewind both the limiter state and the child source.
		virtual result rewind();
	};

	// Peak limiter audio source with lookahead and release window.
	//
	// Wraps another audio source and limits its peaks to the ceiling:
	// - Content below the ceiling is passed through untouched (gain 1).
	// - Peaks above the ceiling are held exactly at the ceiling; the
	//   lookahead window lets the gain ramp down before the peak arrives,
	//   so spikes are never let through and then slammed.
	// - After a peak the gain recovers linearly from 0 to 1 over the
	//   release window, so material following a peak is still attenuated
	//   until the window has passed.
	// - One gain is computed from the loudest channel and applied to all
	//   channels, keeping the stereo image locked.
	// - The source can be rewound (and seeked via the base class tape
	//   seek) as long as the wrapped source can be rewound.
	//
	// Note that the output is delayed by the lookahead time; the tail is
	// flushed when the wrapped source ends, so no audio is lost.
	class Limiter : public AudioSource
	{
	public:
		// Wrapped source. Not owned by the limiter.
		AudioSource *mSource;
		// Peak ceiling, 0..1. Default 0.8.
		float mCeiling;
		// Lookahead time in seconds. Default 0.005 (5 ms).
		float mLookahead;
		// Release window in seconds; time for the gain to ramp from 0
		// back to 1. Default 0.1 (100 ms).
		float mRelease;

		Limiter();
		// Convenience: setSource(aSource) + setParams().
		Limiter(AudioSource *aSource, float aCeiling = 0.8f, float aLookahead = 0.005f, float aRelease = 0.1f);
		virtual ~Limiter();
		// Set the source to limit. Does not take ownership.
		result setSource(AudioSource *aSource);
		// Set ceiling (0..1], lookahead and release window (seconds, >= 0).
		result setParams(float aCeiling, float aLookahead, float aRelease);
		// Set the peak ceiling (0..1].
		void setCeiling(float aCeiling);
		// Set the lookahead time in seconds (>= 0).
		void setLookahead(float aLookahead);
		// Set the release window in seconds (>= 0).
		void setRelease(float aRelease);
		virtual LimiterInstance *createInstance();
	};
};

#endif
