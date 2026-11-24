/*
SoLoud audio engine
Copyright (c) 2024 Jari Komppa

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
#include <string.h>
#include "soloud.h"
#include "soloud_compressor.h"

namespace SoLoud
{
	static bool isFinite(float aValue)
	{
		return aValue == aValue && aValue > -1e30f && aValue < 1e30f;
	}

	CompressorInstance::CompressorInstance(Compressor *aParent)
	{
		// Snapshot the configuration. Reconfiguring (or even deleting) the
		// parent source later does not affect this already running instance.
		mThreshold = aParent->mThreshold;
		mRatio = aParent->mRatio;
		mAttack = aParent->mAttack;
		mRelease = aParent->mRelease;
		mMakeupGain = aParent->mMakeupGain;
		mEnvelope = 0.0f;
		mAttackCoeff = 0.0f;
		mReleaseCoeff = 0.0f;
		mSamplerateCache = 0.0f;
		mSource = 0;
		mScratchSize = SAMPLE_GRANULARITY;
		mScratch.init(mScratchSize * MAX_CHANNELS);
		if (aParent->mSource)
		{
			mSource = aParent->mSource->createInstance();
			if (mSource)
			{
				// The wrapped instance is driven directly, so initialize it
				// the same way Soloud::play() would.
				mSource->init(*aParent->mSource, 0);
				mSource->mAudioSourceID = aParent->mSource->mAudioSourceID;
			}
		}
	}

	CompressorInstance::~CompressorInstance()
	{
		delete mSource;
	}

	void CompressorInstance::updateCoeffs()
	{
		// One-pole time constant coefficient: exp(-1 / (time * samplerate)).
		// Attack time of 0 means instant response (coefficient 0).
		mAttackCoeff = mAttack > 0.0f ? (float)exp(-1.0 / (mAttack * mSamplerate)) : 0.0f;
		mReleaseCoeff = (float)exp(-1.0 / (mRelease * mSamplerate));
		mSamplerateCache = mSamplerate;
	}

	unsigned int CompressorInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		if (mSamplerate != mSamplerateCache)
			updateCoeffs();

		unsigned int channels = mChannels;
		if (channels < 1)
			channels = 1;
		if (channels > MAX_CHANNELS)
			channels = MAX_CHANNELS;

		if (mSource == 0)
		{
			// No wrapped source: emit silence rather than reading garbage.
			for (unsigned int c = 0; c < channels; c++)
				memset(aBuffer + c * aBufferSize, 0, sizeof(float) * aSamplesToRead);
			return aSamplesToRead;
		}

		// Process in scratch-sized chunks so callers may ask for buffers of
		// any length (the engine normally asks for SAMPLE_GRANULARITY frames,
		// but generic seeks and direct users may ask for more).
		unsigned int chunk = mScratchSize / channels;
		unsigned int produced = 0;

		while (produced < aSamplesToRead)
		{
			unsigned int wanted = aSamplesToRead - produced;
			if (wanted > chunk)
				wanted = chunk;

			unsigned int read = mSource->getAudio(mScratch.mData, wanted, wanted);
			if (read > wanted)
				read = wanted;

			// Silence any part of this chunk the wrapped source did not fill.
			for (unsigned int c = 0; c < channels; c++)
				memset(mScratch.mData + c * chunk + read, 0, sizeof(float) * (wanted - read));

			for (unsigned int i = 0; i < wanted; i++)
			{
				// Linked level detector: one shared level across all channels,
				// taken from the loudest channel of this sample frame.
				float level = 0.0f;
				for (unsigned int c = 0; c < channels; c++)
				{
					float v = (float)fabs(mScratch.mData[c * chunk + i]);
					if (v > level)
						level = v;
				}

				// Peak envelope follower with separate attack/release ballistics.
				if (level > mEnvelope)
					mEnvelope += (1.0f - mAttackCoeff) * (level - mEnvelope);
				else
					mEnvelope += (1.0f - mReleaseCoeff) * (level - mEnvelope);

				// Static compression curve: unity gain at or below the threshold,
				// 1/ratio slope above it. Steady-state output for an envelope E is
				// T + (E - T) / R, e.g. 0.5 + (1.0 - 0.5) / 4 = 0.625.
				float gain;
				if (mEnvelope > mThreshold)
					gain = (mThreshold + (mEnvelope - mThreshold) / mRatio) / mEnvelope;
				else
					gain = 1.0f;
				gain *= mMakeupGain;

				// One and the same gain is applied to every channel, keeping
				// the inter-channel balance (and the stereo image) intact.
				for (unsigned int c = 0; c < channels; c++)
					aBuffer[c * aBufferSize + produced + i] =
						mScratch.mData[c * chunk + i] * gain;
			}

			produced += wanted;
			if (read < wanted)
				break; // wrapped source has ended
		}

		return produced;
	}

	bool CompressorInstance::hasEnded()
	{
		if (mSource == 0)
			return false;
		return mSource->hasEnded();
	}

	result CompressorInstance::rewind()
	{
		if (mSource == 0)
			return NOT_IMPLEMENTED;
		result r = mSource->rewind();
		if (r == SO_NO_ERROR)
			mEnvelope = 0.0f;
		return r;
	}

	Compressor::Compressor()
	{
		mSource = 0;
		mThreshold = 0.5f;
		mRatio = 4.0f;
		mAttack = 0.01f;
		mRelease = 0.2f;
		mMakeupGain = 1.0f;
		mChannels = 2;
	}

	Compressor::~Compressor()
	{
		stop();
	}

	result Compressor::setSource(AudioSource *aSource)
	{
		if (aSource == 0)
			return INVALID_PARAMETER;
		if (aSource->mChannels < 1 || aSource->mChannels > MAX_CHANNELS)
			return INVALID_PARAMETER;
		if (!isFinite(aSource->mBaseSamplerate) || aSource->mBaseSamplerate <= 0.0f)
			return INVALID_PARAMETER;
		mSource = aSource;
		mChannels = aSource->mChannels;
		mBaseSamplerate = aSource->mBaseSamplerate;
		return SO_NO_ERROR;
	}

	result Compressor::setParams(float aThreshold, float aRatio, float aAttack,
	                             float aRelease, float aMakeupGain)
	{
		// Validate everything before touching any state, so a failed call
		// leaves both this source and its playing instances untouched.
		if (!isFinite(aThreshold) || aThreshold <= 0.0f || aThreshold > 1.0f)
			return INVALID_PARAMETER;
		if (!isFinite(aRatio) || aRatio < 1.0f)
			return INVALID_PARAMETER;
		if (!isFinite(aAttack) || aAttack < 0.0f || aAttack > 10.0f)
			return INVALID_PARAMETER;
		if (!isFinite(aRelease) || aRelease <= 0.0f || aRelease > 10.0f)
			return INVALID_PARAMETER;
		if (!isFinite(aMakeupGain) || aMakeupGain < 0.0f)
			return INVALID_PARAMETER;

		mThreshold = aThreshold;
		mRatio = aRatio;
		mAttack = aAttack;
		mRelease = aRelease;
		mMakeupGain = aMakeupGain;
		return SO_NO_ERROR;
	}

	AudioSourceInstance *Compressor::createInstance()
	{
		// Always return an instance: the play chain does not expect NULL.
		// A compressor without a wrapped source simply plays silence.
		return new CompressorInstance(this);
	}
};
