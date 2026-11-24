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

#include <math.h>
#include <string.h>
#include "soloud.h"
#include "soloud_audiosource.h"
#include "soloud_envdelay.h"

namespace SoLoud
{
	// Below this absolute level the feedback tail is considered silent.
	static const float ENVDELAY_SILENCE_LEVEL = 0.000001f;
	// Sanity cap for the delay length, in frames.
	static const unsigned int ENVDELAY_MAX_DELAY_FRAMES = 10000000;

	EnvDelayInstance::EnvDelayInstance(EnvDelay *aParent)
	{
		// init() copies mChannels / samplerate from the EnvDelay source, so
		// it must run before any channel-sized buffers are allocated
		init(*aParent, mPlayIndex);

		mSource = aParent->mSource->createInstance();
		mSource->init(*aParent->mSource, mPlayIndex);

		mDelayFrames = aParent->mDelayFrames;
		mFeedback = aParent->mFeedback;
		mWet = aParent->mWet;
		mOffset = 0;
		mSourceEnded = false;
		mTailEnded = false;

		// Planar delay line, shared zero-initialized allocation for all channels
		mDelayBuffer = new float[mDelayFrames * mChannels];
		memset(mDelayBuffer, 0, sizeof(float) * mDelayFrames * mChannels);

		// Scratch for pulling dry material; planar layout with
		// SAMPLE_GRANULARITY stride per channel, matching the engine
		mDryBuffer = new float[SAMPLE_GRANULARITY * mChannels];
		memset(mDryBuffer, 0, sizeof(float) * SAMPLE_GRANULARITY * mChannels);
	}

	EnvDelayInstance::~EnvDelayInstance()
	{
		delete mSource;
		delete[] mDelayBuffer;
		delete[] mDryBuffer;
	}

	unsigned int EnvDelayInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		if (mTailEnded)
		{
			for (unsigned int ch = 0; ch < mChannels; ch++)
				memset(aBuffer + ch * aBufferSize, 0, sizeof(float) * aSamplesToRead);
			return aSamplesToRead;
		}

		// Pull the dry material from the wrapped source. Planar buffers use
		// SAMPLE_GRANULARITY as the per-channel stride (the engine's own
		// convention); the remainder is zero-filled once the wrapped source
		// ends, but the delay line keeps producing the feedback tail.
		unsigned int readcount = 0;
		if (!mSourceEnded)
		{
			readcount = mSource->getAudio(mDryBuffer, aSamplesToRead, SAMPLE_GRANULARITY);
			if (mSource->hasEnded())
				mSourceEnded = true;
		}
		for (unsigned int ch = 0; ch < mChannels; ch++)
		{
			if (readcount < aSamplesToRead)
				memset(mDryBuffer + ch * SAMPLE_GRANULARITY + readcount, 0, sizeof(float) * (aSamplesToRead - readcount));
		}

		const float dryLevel = 1.0f - mWet;

		for (unsigned int ch = 0; ch < mChannels; ch++)
		{
			const float *dryCh = mDryBuffer + ch * SAMPLE_GRANULARITY;
			float *outCh = aBuffer + ch * aBufferSize;
			float *lineCh = mDelayBuffer + ch * mDelayFrames;

			for (unsigned int i = 0; i < aSamplesToRead; i++)
			{
				// Sample delayed by mDelayFrames (the line starts zeroed,
				// so the first reflection appears exactly on time)
				float delayed = lineCh[mOffset];

				// Output: continuous dry signal mixed with the echo tap
				outCh[i] = dryCh[i] * dryLevel + delayed * mWet;

				// Feed the dry material plus the feedback tap back into
				// the line, generating the repeated reflections
				lineCh[mOffset] = dryCh[i] + delayed * mFeedback;

				mOffset++;
				if (mOffset >= mDelayFrames)
					mOffset = 0;
			}
		}

		// Only end once the source material is finished AND every repeated
		// reflection has decayed below the silence threshold. The whole
		// delay line is scanned because taps are spaced mDelayFrames apart
		// and the current write offset may sit in the zero region between
		// taps; checking one position would truncate the tail early.
		if (mSourceEnded)
		{
			float tailPeak = 0.0f;
			for (unsigned int i = 0; i < mDelayFrames * mChannels; i++)
			{
				float level = fabsf(mDelayBuffer[i]);
				if (level > tailPeak)
					tailPeak = level;
			}
			if (tailPeak < ENVDELAY_SILENCE_LEVEL)
				mTailEnded = true;
		}

		return aSamplesToRead;
	}

	bool EnvDelayInstance::hasEnded()
	{
		return mTailEnded;
	}

	result EnvDelayInstance::rewind()
	{
		mOffset = 0;
		mSourceEnded = false;
		mTailEnded = false;
		memset(mDelayBuffer, 0, sizeof(float) * mDelayFrames * mChannels);
		return mSource->rewind();
	}

	EnvDelay::EnvDelay()
	{
		mSource = 0;
		mDelayFrames = 0;
		mFeedback = 0.5f;
		mWet = 0.5f;
	}

	EnvDelay::~EnvDelay()
	{
		stop();
	}

	result EnvDelay::setParams(AudioSource &aSource, unsigned int aDelayFrames, float aFeedback, float aWet)
	{
		// Reject illegal configurations before any instance can be created
		if (aDelayFrames == 0 || aDelayFrames > ENVDELAY_MAX_DELAY_FRAMES)
			return INVALID_PARAMETER;
		if (aFeedback < 0.0f || aFeedback >= 1.0f)
			return INVALID_PARAMETER;
		if (aWet < 0.0f || aWet > 1.0f)
			return INVALID_PARAMETER;
		if (aSource.mChannels == 0 || aSource.mChannels > MAX_CHANNELS)
			return INVALID_PARAMETER;
		if (aSource.mBaseSamplerate <= 0.0f)
			return INVALID_PARAMETER;

		mSource = &aSource;
		mDelayFrames = aDelayFrames;
		mFeedback = aFeedback;
		mWet = aWet;

		// Match the wrapped source so the engine resamples the combined
		// stream at the same rate as the dry material
		mBaseSamplerate = aSource.mBaseSamplerate;
		mChannels = aSource.mChannels;

		return SO_NO_ERROR;
	}

	AudioSourceInstance *EnvDelay::createInstance()
	{
		if (mSource == 0)
			return 0;
		return new EnvDelayInstance(this);
	}
};
