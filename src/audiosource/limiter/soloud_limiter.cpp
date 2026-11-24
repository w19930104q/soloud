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

#include <string.h>
#include <math.h>
#include "soloud_limiter.h"
#include "soloud_error.h"

// Processing block size; the delay buffer holds the lookahead window
// plus one block, so the window stays full while a block is emitted.
#define LIMITER_BLOCK 512

namespace SoLoud
{
	LimiterInstance::LimiterInstance(Limiter *aParent, AudioSourceInstance *aSource)
	{
		mParent = aParent;
		mSource = aSource;
		mDelayBuf = 0;
		mDelaySize = 0;
		mLookaheadSamples = 0;
		mDelayHead = 0;
		mDelayCount = 0;
		mGain = 1.0f;
		mReleaseStep = 1.0f;
		mAttackStep = 1.0f;
		mSourceEnded = (mSource == 0);
		if (mSource)
		{
			// Looping is handled at the limiter level (the core seeks
			// this instance, which rewinds the child); the child must
			// report its end honestly so the delay buffer can drain.
			mSource->mFlags &= ~AudioSourceInstance::LOOPING;
		}
	}

	LimiterInstance::~LimiterInstance()
	{
		delete mSource;
		delete[] mDelayBuf;
	}

	void LimiterInstance::initBuffer()
	{
		float sr = mSamplerate;
		if (sr < 1.0f)
			sr = 1.0f;
		unsigned int lookahead = (unsigned int)floor(mParent->mLookahead * sr + 0.5f);
		if (lookahead < 1)
			lookahead = 1;
		mLookaheadSamples = lookahead;
		mDelaySize = mLookaheadSamples + LIMITER_BLOCK;
		mDelayBuf = new float[mDelaySize * mChannels];
		mDelayHead = 0;
		mDelayCount = 0;
		// Release: gain ramps linearly from 0 back to 1 over the window.
		float releasesamples = mParent->mRelease * sr;
		mReleaseStep = (releasesamples > 1.0f) ? (1.0f / releasesamples) : 1.0f;
		// Attack: with the lookahead window the gain may ramp from 1 down
		// to 0 over the window, so it always reaches the needed gain
		// exactly when the peak leaves the delay line.
		mAttackStep = 1.0f / (float)mLookaheadSamples;
	}

	void LimiterInstance::topUp()
	{
		if (mSourceEnded || mSource == 0)
			return;
		if (mDelayHead > 0)
		{
			unsigned int ch;
			for (ch = 0; ch < mChannels; ch++)
			{
				memmove(mDelayBuf + ch * mDelaySize,
						mDelayBuf + ch * mDelaySize + mDelayHead,
						mDelayCount * sizeof(float));
			}
			mDelayHead = 0;
		}
		while (mDelayCount < mDelaySize)
		{
			// The delay buffer is planar with mDelaySize samples per
			// channel, so the child can write straight into the free
			// tail of every channel at once.
			unsigned int got = mSource->getAudio(mDelayBuf + mDelayCount, mDelaySize - mDelayCount, mDelaySize);
			if (got == 0)
			{
				if (mSource->hasEnded())
					mSourceEnded = true;
				break;
			}
			mDelayCount += got;
			if (mSource->hasEnded())
			{
				mSourceEnded = true;
				break;
			}
		}
	}

	unsigned int LimiterInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		if (mDelayBuf == 0)
			initBuffer();

		float ceiling = mParent->mCeiling;
		unsigned int produced = 0;

		while (produced < aSamplesToRead)
		{
			topUp();
			if (mDelayCount == 0)
				break;

			// Number of samples we may emit while keeping a full
			// lookahead window. At the stream tail the window is
			// allowed to shrink as the buffer drains.
			unsigned int chunk = mDelayCount;
			if (!mSourceEnded)
			{
				if (mDelayCount <= mLookaheadSamples)
					break; // window not full yet; wait for more input
				chunk = mDelayCount - mLookaheadSamples;
			}
			if (chunk > aSamplesToRead - produced)
				chunk = aSamplesToRead - produced;
			if (chunk == 0)
				break;

			unsigned int i, j;
			unsigned int ch;
			for (i = 0; i < chunk; i++)
			{
				unsigned int pos = mDelayHead + i;
				// Lookahead window: pos .. pos + lookahead (clipped at
				// the end of the valid data, which only happens at the
				// stream tail).
				unsigned int wend = pos + mLookaheadSamples;
				if (wend >= mDelayHead + mDelayCount)
					wend = mDelayHead + mDelayCount - 1;

				// Gain may rise by at most the release slope...
				float g = mGain + mReleaseStep;
				if (g > 1.0f)
					g = 1.0f;
				// ...and must be low enough that, ramping down at the
				// attack slope, every peak inside the window is met at
				// or below its needed gain.
				for (j = pos; j <= wend; j++)
				{
					float det = 0;
					for (ch = 0; ch < mChannels; ch++)
					{
						float v = (float)fabs(mDelayBuf[ch * mDelaySize + j]);
						if (v > det)
							det = v;
					}
					if (det > ceiling)
					{
						float lim = ceiling / det + (float)(j - pos) * mAttackStep;
						if (lim < g)
							g = lim;
					}
				}
				mGain = g;

				// One shared gain for all channels: the stereo image
				// stays put.
				for (ch = 0; ch < mChannels; ch++)
				{
					aBuffer[ch * aBufferSize + produced] = mDelayBuf[ch * mDelaySize + pos] * g;
				}
				produced++;
			}
			mDelayHead += chunk;
			mDelayCount -= chunk;
			if (mDelayCount == 0)
				mDelayHead = 0;
		}
		return produced;
	}

	bool LimiterInstance::hasEnded()
	{
		if (mFlags & AudioSourceInstance::LOOPING)
			return false;
		return mSourceEnded && mDelayCount == 0;
	}

	result LimiterInstance::rewind()
	{
		if (mSource)
		{
			result res = mSource->rewind();
			if (res != SO_NO_ERROR)
				return res;
		}
		mDelayHead = 0;
		mDelayCount = 0;
		mGain = 1.0f;
		mSourceEnded = false;
		mStreamPosition = 0.0f;
		return SO_NO_ERROR;
	}

	Limiter::Limiter()
	{
		mSource = 0;
		mCeiling = 0.8f;
		mLookahead = 0.005f;
		mRelease = 0.1f;
	}

	Limiter::Limiter(AudioSource *aSource, float aCeiling, float aLookahead, float aRelease)
	{
		mSource = 0;
		mCeiling = 0.8f;
		mLookahead = 0.005f;
		mRelease = 0.1f;
		setParams(aCeiling, aLookahead, aRelease);
		setSource(aSource);
	}

	Limiter::~Limiter()
	{
		stop();
	}

	result Limiter::setSource(AudioSource *aSource)
	{
		if (aSource == 0 || aSource == this)
			return INVALID_PARAMETER;
		stop();
		mSource = aSource;
		// Mirror the wrapped source so instances get the right channel
		// count and sample rate.
		mChannels = aSource->mChannels;
		mBaseSamplerate = aSource->mBaseSamplerate;
		return SO_NO_ERROR;
	}

	result Limiter::setParams(float aCeiling, float aLookahead, float aRelease)
	{
		if (aCeiling <= 0.0f || aLookahead < 0.0f || aRelease < 0.0f)
			return INVALID_PARAMETER;
		setCeiling(aCeiling);
		setLookahead(aLookahead);
		setRelease(aRelease);
		return SO_NO_ERROR;
	}

	void Limiter::setCeiling(float aCeiling)
	{
		if (aCeiling > 0.0f)
			mCeiling = aCeiling;
	}

	void Limiter::setLookahead(float aLookahead)
	{
		if (aLookahead >= 0.0f)
			mLookahead = aLookahead;
	}

	void Limiter::setRelease(float aRelease)
	{
		if (aRelease >= 0.0f)
			mRelease = aRelease;
	}

	LimiterInstance *Limiter::createInstance()
	{
		AudioSourceInstance *src = 0;
		if (mSource)
		{
			// Keep mirroring the wrapped source in case it was
			// reconfigured after setSource().
			mChannels = mSource->mChannels;
			mBaseSamplerate = mSource->mBaseSamplerate;
			src = mSource->createInstance();
			if (src)
				src->init(*mSource, 0);
		}
		LimiterInstance *inst = new LimiterInstance(this, src);
		// Make the instance usable even without the core init() dance
		// (e.g. when pulling samples straight from the instance).
		if (mSource)
		{
			inst->mChannels = mSource->mChannels;
			inst->mBaseSamplerate = mSource->mBaseSamplerate;
			inst->mSamplerate = mSource->mBaseSamplerate;
		}
		return inst;
	}
};
