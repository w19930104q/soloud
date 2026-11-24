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

#include <math.h>
#include "soloud_tremolo.h"

namespace SoLoud
{
	// Triangle LFO gain at phase aPhase (in cycles). Depth 1.0 yields
	// 1.0 at the cycle start, 0.0 at the half-cycle point, e.g. over a
	// 4-frame period the gains are 1.0, 0.5, 0.0, 0.5.
	static float tremoloGain(double aPhase, float aDepth)
	{
		double triangle = fabs(2.0 * aPhase - 1.0);
		return (1.0f - aDepth) + aDepth * (float)triangle;
	}

	TremoloInstance::TremoloInstance(Tremolo *aParent)
	{
		mRate = aParent->mRate;
		mDepth = aParent->mDepth;
		mChannelOffset = aParent->mChannelPhase * 0.5f;
		mPhase = 0.0;
		mInstance = 0;
		mPhaseDelta = 0.0f;
		if (aParent->mSource)
		{
			mInstance = aParent->mSource->createInstance();
			if (mInstance)
			{
				// play() calls init() on this wrapper after construction;
				// the wrapped instance has no engine of its own, so give
				// it its base parameters here.
				mInstance->init(*aParent->mSource, 0);
				mChannels = mInstance->mChannels;
				mBaseSamplerate = mInstance->mBaseSamplerate;
				mSamplerate = mInstance->mSamplerate;
				mPhaseDelta = mRate / mBaseSamplerate;
			}
		}
	}

	TremoloInstance::~TremoloInstance()
	{
		delete mInstance;
	}

	unsigned int TremoloInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		if (!mInstance)
			return 0;

		unsigned int readcount = mInstance->getAudio(aBuffer, aSamplesToRead, aBufferSize);

		// Planar layout: channel c occupies aBuffer[c * aBufferSize ..].
		// The phase accumulator advances exactly once per produced frame
		// and wraps with fmod, so irregular or split pulls produce exactly
		// the same envelope as one continuous pull.
		for (unsigned int i = 0; i < readcount; i++)
		{
			for (unsigned int c = 0; c < mChannels; c++)
			{
				double channelPhase = mPhase;
				if (c > 0)
				{
					channelPhase += mChannelOffset;
					if (channelPhase >= 1.0)
						channelPhase -= 1.0;
				}
				aBuffer[c * aBufferSize + i] *= tremoloGain(channelPhase, mDepth);
			}
			mPhase += mPhaseDelta;
			mPhase = fmod(mPhase, 1.0);
		}

		return readcount;
	}

	bool TremoloInstance::hasEnded()
	{
		if (!mInstance)
			return true;
		return mInstance->hasEnded();
	}

	result TremoloInstance::rewind()
	{
		// Always restart the LFO cycle, even if the wrapped source
		// cannot rewind; return the wrapped source's result either way.
		mPhase = 0.0;
		if (!mInstance)
			return NOT_IMPLEMENTED;
		return mInstance->rewind();
	}

	Tremolo::Tremolo()
	{
		mSource = 0;
		mRate = 1.0f;
		mDepth = 1.0f;
		mChannelPhase = 0.0f;
	}

	Tremolo::~Tremolo()
	{
		stop();
	}

	result Tremolo::setSource(AudioSource &aSource)
	{
		if (!isfinite(aSource.mBaseSamplerate) || aSource.mBaseSamplerate <= 0)
			return INVALID_PARAMETER;

		mSource = &aSource;
		mBaseSamplerate = aSource.mBaseSamplerate;
		mChannels = aSource.mChannels;
		return SO_NO_ERROR;
	}

	result Tremolo::setRate(float aRate)
	{
		if (!isfinite(aRate) || aRate <= 0)
			return INVALID_PARAMETER;
		mRate = aRate;
		return SO_NO_ERROR;
	}

	result Tremolo::setDepth(float aDepth)
	{
		if (!isfinite(aDepth) || aDepth < 0.0f || aDepth > 1.0f)
			return INVALID_PARAMETER;
		mDepth = aDepth;
		return SO_NO_ERROR;
	}

	result Tremolo::setChannelPhase(float aChannelPhase)
	{
		if (!isfinite(aChannelPhase) || aChannelPhase < 0.0f || aChannelPhase > 1.0f)
			return INVALID_PARAMETER;
		mChannelPhase = aChannelPhase;
		return SO_NO_ERROR;
	}

	result Tremolo::setParams(AudioSource &aSource, float aRate, float aDepth, float aChannelPhase)
	{
		// Validate everything before touching any state, so a rejected
		// call leaves the previous valid configuration intact.
		if (!isfinite(aSource.mBaseSamplerate) || aSource.mBaseSamplerate <= 0)
			return INVALID_PARAMETER;
		if (!isfinite(aRate) || aRate <= 0)
			return INVALID_PARAMETER;
		if (!isfinite(aDepth) || aDepth < 0.0f || aDepth > 1.0f)
			return INVALID_PARAMETER;
		if (!isfinite(aChannelPhase) || aChannelPhase < 0.0f || aChannelPhase > 1.0f)
			return INVALID_PARAMETER;

		mSource = &aSource;
		mBaseSamplerate = aSource.mBaseSamplerate;
		mChannels = aSource.mChannels;
		mRate = aRate;
		mDepth = aDepth;
		mChannelPhase = aChannelPhase;
		return SO_NO_ERROR;
	}

	AudioSourceInstance *Tremolo::createInstance()
	{
		return new TremoloInstance(this);
	}
};
