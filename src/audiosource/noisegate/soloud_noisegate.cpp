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
#include "soloud.h"
#include "soloud_noisegate.h"

namespace SoLoud
{
	NoiseGateInstance::NoiseGateInstance(NoiseGate *aParent)
	{
		mParent = aParent;
		mSource = 0;
		mGateOpen = false;
		mHoldSamplesLeft = 0;
		if (mParent->mSource != 0)
		{
			mSource = mParent->mSource->createInstance();
			if (mSource != 0)
			{
				mSource->init(*mParent->mSource, 0);
			}
		}
	}

	NoiseGateInstance::~NoiseGateInstance()
	{
		delete mSource;
	}

	unsigned int NoiseGateInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		if (mSource == 0)
		{
			return 0;
		}

		unsigned int samples = mSource->getAudio(aBuffer, aSamplesToRead, aBufferSize);
		if (samples == 0)
		{
			return 0;
		}

		// Hold time in samples at the current playback rate
		unsigned int holdSamples = (unsigned int)floor(mParent->mHoldTime * mSamplerate + 0.5f);
		float openThreshold = mParent->mOpenThreshold;
		float closeThreshold = mParent->mCloseThreshold;

		unsigned int ch, i;
		for (i = 0; i < samples; i++)
		{
			// Linked detection: the loudest channel of the frame decides
			// for all channels, so the gate never opens or closes the
			// channels independently.
			float level = 0;
			for (ch = 0; ch < mChannels; ch++)
			{
				float v = (float)fabs(aBuffer[ch * aBufferSize + i]);
				if (v > level)
				{
					level = v;
				}
			}

			if (level >= openThreshold)
			{
				// Loud enough: open (or keep open) and re-arm the hold timer
				mGateOpen = true;
				mHoldSamplesLeft = holdSamples;
			}
			else if (level < closeThreshold)
			{
				// Below close threshold: run down the hold time before
				// closing, so weak tails still pass through while holding
				if (mHoldSamplesLeft > 0)
				{
					mHoldSamplesLeft--;
				}
				else
				{
					mGateOpen = false;
				}
			}
			// Between the close and open thresholds the current state is
			// kept (hysteresis), so the gate doesn't chatter.

			if (!mGateOpen)
			{
				for (ch = 0; ch < mChannels; ch++)
				{
					aBuffer[ch * aBufferSize + i] = 0;
				}
			}
		}

		return samples;
	}

	bool NoiseGateInstance::hasEnded()
	{
		if (mSource == 0)
		{
			return true;
		}
		return mSource->hasEnded();
	}

	result NoiseGateInstance::rewind()
	{
		if (mSource == 0)
		{
			return NOT_IMPLEMENTED;
		}
		mGateOpen = false;
		mHoldSamplesLeft = 0;
		return mSource->rewind();
	}

	NoiseGate::NoiseGate()
	{
		mSource = 0;
		mOpenThreshold = 0.1f;
		mCloseThreshold = 0.05f;
		mHoldTime = 0.1f;
	}

	NoiseGate::NoiseGate(AudioSource *aSource)
	{
		mSource = 0;
		mOpenThreshold = 0.1f;
		mCloseThreshold = 0.05f;
		mHoldTime = 0.1f;
		setSource(aSource);
	}

	NoiseGate::~NoiseGate()
	{
		stop();
	}

	result NoiseGate::setSource(AudioSource *aSource)
	{
		if (aSource == 0 || aSource == this)
		{
			return INVALID_PARAMETER;
		}
		mSource = aSource;
		// The gate passes the source through, so it inherits its
		// channel count and base sample rate.
		mChannels = aSource->mChannels;
		mBaseSamplerate = aSource->mBaseSamplerate;
		return SO_NO_ERROR;
	}

	void NoiseGate::setThresholds(float aOpenThreshold, float aCloseThreshold)
	{
		if (aOpenThreshold < 0)
		{
			aOpenThreshold = 0;
		}
		if (aCloseThreshold < 0)
		{
			aCloseThreshold = 0;
		}
		if (aCloseThreshold > aOpenThreshold)
		{
			aCloseThreshold = aOpenThreshold;
		}
		mOpenThreshold = aOpenThreshold;
		mCloseThreshold = aCloseThreshold;
	}

	void NoiseGate::setHoldTime(float aHoldTime)
	{
		if (aHoldTime < 0)
		{
			aHoldTime = 0;
		}
		mHoldTime = aHoldTime;
	}

	AudioSourceInstance *NoiseGate::createInstance()
	{
		return new NoiseGateInstance(this);
	}
};
