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

   3. This notice may not be removed or altered from any source distribution.
*/

#include <math.h>
#include <string.h>
#include "soloud.h"
#include "soloud_channelmixer.h"

namespace SoLoud
{
	// Same fixed-point convention as the engine's own resamplers
	// (see resample_linear() in soloud.cpp).
	#define CHANNELMIXER_FP_BITS 20
	#define CHANNELMIXER_FP_MUL (1 << CHANNELMIXER_FP_BITS)
	#define CHANNELMIXER_FP_MASK (CHANNELMIXER_FP_MUL - 1)

	// Layouts supported by the engine (soloud.h: 1)mono, 2)stereo,
	// 4)quad, 6)5.1, 8)7.1).
	static bool validLayout(unsigned int aChannels)
	{
		return aChannels == 1 || aChannels == 2 || aChannels == 4 ||
			aChannels == 6 || aChannels == 8;
	}

	// Equal-power gain used when a stereo pair is folded onto a single
	// speaker. With L=0.2 and R=-0.4 this gives (0.2-0.4)/sqrt(2) = -0.141421.
	static float equalPower()
	{
		return 1.0f / (float)sqrt(2.0f);
	}

	// Build the default mapping matrix for the given layouts. Every scalar
	// below is either:
	//   1.0  - identical speaker pass-through (e.g. 5.1 front-left -> front-left),
	//   1/sqrt(2) - the equal-power stereo fold given by the reference sample,
	//   or one of the coefficients already used by the engine's public
	//   panAndExpand() mixer (0.5 / 0.3 / 0.25 / 0.7 / 0.2).
	// LFE (index 3 in the 5.1/7.1 layouts) only ever maps to an LFE output.
	static void buildDefaultMapping(float aMapping[MAX_CHANNELS][MAX_CHANNELS],
		unsigned int aSrcChannels, unsigned int aDstChannels)
	{
		memset(aMapping, 0, sizeof(float) * MAX_CHANNELS * MAX_CHANNELS);

		const float ep = equalPower();

		if (aDstChannels == 1)
		{
			if (aSrcChannels == 1)
			{
				// mono -> mono is an identical-speaker passthrough
				aMapping[0][0] = 1.0f;
				return;
			}
			// Multi-channel material folds onto the mono speaker. A stereo
			// pair is combined at equal power; additional pairs are averaged
			// in first the same way the engine's mono path averages its
			// extra channels.
			for (unsigned int s = 0; s < aSrcChannels; s++)
				aMapping[0][s] = ep;
			if (aSrcChannels >= 4)
			{
				// quad/5.1/7.1: back/side pairs are averaged in at 0.5, matching
				// the engine's 0.5 family-merge coefficients.
				for (unsigned int s = 4; s < aSrcChannels; s++)
					aMapping[0][s] = 0.5f * ep;
			}
			if (aSrcChannels >= 6)
			{
				// LFE never contributes to a full-range speaker.
				aMapping[0][ChannelMixer::LFE] = 0.0f;
				// Center is a single channel; keep it at equal-power level.
				aMapping[0][ChannelMixer::FRONT_CENTER] = ep;
			}
			if (aSrcChannels == 8)
			{
				// 7.1 side pair, averaged like the back pair.
				aMapping[0][ChannelMixer::SIDE_LEFT] = 0.5f * ep;
				aMapping[0][ChannelMixer::SIDE_RIGHT] = 0.5f * ep;
			}
			return;
		}

		if (aDstChannels == 2)
		{
			switch (aSrcChannels)
			{
			case 1: // mono -> both speakers, full volume
				aMapping[0][0] = 1.0f;
				aMapping[1][0] = 1.0f;
				break;
			case 2: // stereo passthrough
				aMapping[0][0] = 1.0f;
				aMapping[1][1] = 1.0f;
				break;
			case 4: // engine 4->2: L = 0.5*(FL+BL), R = 0.5*(FR+BR)
				aMapping[0][0] = 0.5f; aMapping[0][2] = 0.5f;
				aMapping[1][1] = 0.5f; aMapping[1][3] = 0.5f;
				break;
			case 6:
				// Engine 6->2 coefficients, minus the LFE leak: LFE row stays 0.
				// L = 0.3*(FL+FC+BL), R = 0.3*(FR+FC+BR)
				aMapping[0][0] = 0.3f; aMapping[0][2] = 0.3f; aMapping[0][4] = 0.3f;
				aMapping[1][1] = 0.3f; aMapping[1][2] = 0.3f; aMapping[1][5] = 0.3f;
				break;
			case 8:
				// Engine 8->2 coefficients, minus the LFE leak: LFE row stays 0.
				// L = 0.2*(FL+FC+BL+SL), R = 0.2*(FR+FC+BR+SR)
				aMapping[0][0] = 0.2f; aMapping[0][2] = 0.2f;
				aMapping[0][4] = 0.2f; aMapping[0][6] = 0.2f;
				aMapping[1][1] = 0.2f; aMapping[1][2] = 0.2f;
				aMapping[1][5] = 0.2f; aMapping[1][7] = 0.2f;
				break;
			}
			return;
		}

		if (aDstChannels == 4)
		{
			switch (aSrcChannels)
			{
			case 1: // mono -> all four speakers
				for (unsigned int d = 0; d < 4; d++)
					aMapping[d][0] = 1.0f;
				break;
			case 2:
				// engine 2->4: front pair copied, back pair copied
				aMapping[0][0] = 1.0f; aMapping[1][1] = 1.0f;
				aMapping[2][0] = 1.0f; aMapping[3][1] = 1.0f;
				break;
			case 4: // quad passthrough
				for (unsigned int i = 0; i < 4; i++)
					aMapping[i][i] = 1.0f;
				break;
			case 6:
				// engine 6->4: fronts pass through, backs pass through;
				// center mixed into both fronts at 0.7; LFE dropped (no LFE speaker).
				aMapping[0][0] = 1.0f; aMapping[0][2] = 0.7f;
				aMapping[1][1] = 1.0f; aMapping[1][2] = 0.7f;
				aMapping[2][4] = 1.0f;
				aMapping[3][5] = 1.0f;
				break;
			case 8:
				// engine 8->4: fronts + center(0.7), back/side pairs averaged 0.5;
				// LFE dropped.
				aMapping[0][0] = 1.0f; aMapping[0][2] = 0.7f;
				aMapping[1][1] = 1.0f; aMapping[1][2] = 0.7f;
				aMapping[2][4] = 0.5f; aMapping[2][6] = 0.5f;
				aMapping[3][5] = 0.5f; aMapping[3][7] = 0.5f;
				break;
			}
			return;
		}

		if (aDstChannels == 6)
		{
			switch (aSrcChannels)
			{
			case 1: // mono -> every full-range speaker, LFE excluded
				for (unsigned int d = 0; d < 6; d++)
					aMapping[d][0] = 1.0f;
				aMapping[ChannelMixer::LFE][0] = 0.0f;
				break;
			case 2:
				// engine 2->6: fronts copied, center/LFE = 0.5*(L+R), backs copied
				aMapping[0][0] = 1.0f;
				aMapping[1][1] = 1.0f;
				aMapping[2][0] = 0.5f; aMapping[2][1] = 0.5f;
				// LFE stays silent: a stereo source carries no LFE content.
				aMapping[4][0] = 1.0f;
				aMapping[5][1] = 1.0f;
				break;
			case 4:
				// Derived from the engine's 4->6 mixer: fronts/backs copied and
				// center = 0.5*(L+R). The engine synthesizes an LFE from all
				// four full-range channels, but LFE must only carry real LFE
				// content, so the LFE row is left silent instead.
				aMapping[0][0] = 1.0f;
				aMapping[1][1] = 1.0f;
				aMapping[2][0] = 0.5f; aMapping[2][1] = 0.5f;
				aMapping[4][2] = 1.0f;
				aMapping[5][3] = 1.0f;
				break;
			case 6: // 5.1 passthrough, including LFE
				for (unsigned int i = 0; i < 6; i++)
					aMapping[i][i] = 1.0f;
				break;
			case 8:
				// engine 8->6: shared speakers pass through; back/side pairs
				// averaged into the 5.1 backs; LFE passes straight through.
				for (unsigned int i = 0; i < 4; i++)
					aMapping[i][i] = 1.0f;
				aMapping[4][4] = 0.5f; aMapping[4][6] = 0.5f;
				aMapping[5][5] = 0.5f; aMapping[5][7] = 0.5f;
				break;
			}
			return;
		}

		if (aDstChannels == 8)
		{
			switch (aSrcChannels)
			{
			case 1: // mono -> every full-range speaker, LFE excluded
				for (unsigned int d = 0; d < 8; d++)
					aMapping[d][0] = 1.0f;
				aMapping[ChannelMixer::LFE][0] = 0.0f;
				break;
			case 2:
				// Derived from the engine's 2->8 mixer: fronts and back/side
				// pairs copied, center = 0.5*(L+R). The engine also routes the
				// averaged pair into LFE, but LFE must only carry real LFE
				// content, so that row stays silent.
				aMapping[0][0] = 1.0f;
				aMapping[1][1] = 1.0f;
				aMapping[2][0] = 0.5f; aMapping[2][1] = 0.5f;
				// aMapping[3][*] (LFE) intentionally left at zero
				aMapping[4][0] = 1.0f;
				aMapping[5][1] = 1.0f;
				aMapping[6][0] = 1.0f;
				aMapping[7][1] = 1.0f;
				break;
			case 4:
				// engine 4->8, with the engine's synthesized LFE removed.
				aMapping[0][0] = 1.0f;
				aMapping[1][1] = 1.0f;
				aMapping[2][0] = 0.5f; aMapping[2][1] = 0.5f;
				aMapping[4][0] = 0.5f; aMapping[4][2] = 0.5f;
				aMapping[5][1] = 0.5f; aMapping[5][3] = 0.5f;
				aMapping[6][2] = 1.0f;
				aMapping[7][3] = 1.0f;
				break;
			case 6:
				// engine 6->8: shared speakers pass through; backs feed both the
				// 7.1 back and side positions (averaged with the front for backs);
				// LFE passes straight through.
				for (unsigned int i = 0; i < 4; i++)
					aMapping[i][i] = 1.0f;
				aMapping[4][0] = 0.5f; aMapping[4][4] = 0.5f;
				aMapping[5][1] = 0.5f; aMapping[5][5] = 0.5f;
				aMapping[6][4] = 1.0f;
				aMapping[7][5] = 1.0f;
				break;
			case 8: // 7.1 passthrough
				for (unsigned int i = 0; i < 8; i++)
					aMapping[i][i] = 1.0f;
				break;
			}
		}
	}

	ChannelMixerInstance::ChannelMixerInstance(ChannelMixer *aParent)
	{
		mParent = aParent;
		mInner = aParent->mSource->createInstance();
		mInner->init(*aParent->mSource, 0);
		// Inner source plays at its own native rate; this instance resamples.
		mMapped = new float[SAMPLE_GRANULARITY * MAX_CHANNELS];
		mInnerBlock = new float[SAMPLE_GRANULARITY * MAX_CHANNELS];
		memset(mMapped, 0, sizeof(float) * SAMPLE_GRANULARITY * MAX_CHANNELS);
		memset(mInnerBlock, 0, sizeof(float) * SAMPLE_GRANULARITY * MAX_CHANNELS);
		memset(mLastFrame, 0, sizeof(mLastFrame));
		mSrcOffset = 0;
		mMappedFrames = 0;
		mBlocksSeen = 0;
	}

	ChannelMixerInstance::~ChannelMixerInstance()
	{
		delete mInner;
		delete[] mMapped;
		delete[] mInnerBlock;
	}

	bool ChannelMixerInstance::hasEnded()
	{
		return mInner->hasEnded() &&
			(mMappedFrames == 0 || mSrcOffset >= mMappedFrames * CHANNELMIXER_FP_MUL);
	}

	result ChannelMixerInstance::rewind()
	{
		result r = mInner->rewind();
		mSrcOffset = 0;
		mMappedFrames = 0;
		mBlocksSeen = 0;
		memset(mLastFrame, 0, sizeof(mLastFrame));
		return r;
	}

	// Pull one inner-source block, map it and cache the planar result. The
	// fixed-point playhead is kept continuous: on a block change it is merely
	// rebased by one block length, preserving any fractional/position leftover
	// so no samples are skipped or repeated at the seam.
	void ChannelMixerInstance::refillBlock()
	{
		unsigned int srcChannels = mParent->mSource->mChannels;
		unsigned int dstChannels = mParent->mChannels;

		// Save the final frame of the block we are about to replace so the
		// linear resampler can interpolate across the block boundary.
		if (mMappedFrames > 0)
		{
			for (unsigned int c = 0; c < dstChannels; c++)
				mLastFrame[c] = mMapped[c * SAMPLE_GRANULARITY + mMappedFrames - 1];
		}

		unsigned int readcount = mInner->getAudio(mInnerBlock, SAMPLE_GRANULARITY, SAMPLE_GRANULARITY);
		mMappedFrames = readcount;

		// Apply the mapping matrix. Output is planar, like every engine buffer.
		for (unsigned int d = 0; d < dstChannels; d++)
		{
			float *dst = mMapped + d * SAMPLE_GRANULARITY;
			for (unsigned int i = 0; i < readcount; i++)
			{
				float acc = 0.0f;
				for (unsigned int s = 0; s < srcChannels; s++)
				{
					float gain = mParent->mMapping[d][s];
					if (gain != 0.0f)
						acc += mInnerBlock[s * SAMPLE_GRANULARITY + i] * gain;
				}
				dst[i] = acc;
			}
		}

		if (mBlocksSeen == 0 && readcount > 0)
		{
			// First block of the stream: seed the "previous sample" with the
			// first mapped frame so the opening output sample is exact instead
			// of fading in from zero.
			for (unsigned int c = 0; c < dstChannels; c++)
				mLastFrame[c] = mMapped[c * SAMPLE_GRANULARITY];
		}
		mBlocksSeen++;

		// Rebase the continuous playhead onto the start of the new block.
		// Any leftover position (fractional phase or overshoot) is retained.
		if (mSrcOffset >= SAMPLE_GRANULARITY * CHANNELMIXER_FP_MUL)
			mSrcOffset -= SAMPLE_GRANULARITY * CHANNELMIXER_FP_MUL;
	}

	unsigned int ChannelMixerInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		unsigned int dstChannels = mParent->mChannels;
		unsigned int srcChannels = mParent->mSource->mChannels;

		bool identity = (dstChannels == srcChannels);
		if (identity)
		{
			for (unsigned int d = 0; d < dstChannels && identity; d++)
				for (unsigned int s = 0; s < srcChannels; s++)
					if (mParent->mMapping[d][s] != (d == s ? 1.0f : 0.0f))
						identity = false;
		}
		bool sameRate = (mParent->mOutputSamplerate == 0.0f) ||
			(mParent->mOutputSamplerate == mParent->mSource->mBaseSamplerate);

		// Original material passes through untouched: identity layout at the
		// native rate forwards the inner source's samples verbatim.
		if (identity && sameRate)
		{
			return mInner->getAudio(aBuffer, aSamplesToRead, aBufferSize);
		}

		// Output frames per inner-source frame.
		float step = mBaseSamplerate / (float)mParent->mSource->mBaseSamplerate;
		unsigned int stepFixed = (unsigned int)floor(step * CHANNELMIXER_FP_MUL);
		if (stepFixed == 0)
			stepFixed = 1;

		unsigned int written = 0;
		while (written < aSamplesToRead)
		{
			if (mMappedFrames == 0 ||
				(mSrcOffset >> CHANNELMIXER_FP_BITS) >= mMappedFrames)
			{
				refillBlock();
				if (mMappedFrames == 0)
				{
					// Inner source ended before filling the request. Zero the
					// remaining frames across every output channel but keep
					// draining so the reported sample count matches the
					// request, matching the engine's stream contract.
					for (unsigned int c = 0; c < dstChannels; c++)
						memset(aBuffer + c * aBufferSize + written, 0,
							sizeof(float) * (aSamplesToRead - written));
					written = aSamplesToRead;
					break;
				}
			}

			unsigned int p = mSrcOffset >> CHANNELMIXER_FP_BITS;
			float ff = (float)(mSrcOffset & CHANNELMIXER_FP_MASK);
			for (unsigned int c = 0; c < dstChannels; c++)
			{
				const float *src = mMapped + c * SAMPLE_GRANULARITY;
				// At the first sample of a block the "previous" sample is the
				// final frame of the previous block (mLastFrame); everywhere
				// else it is the preceding frame inside the current block.
				float s1 = (p == 0) ? mLastFrame[c] : src[p - 1];
				float s2 = src[p];
				aBuffer[c * aBufferSize + written] =
					s1 + (s2 - s1) * ff * (1.0f / CHANNELMIXER_FP_MUL);
			}
			mSrcOffset += stepFixed;
			written++;
		}
		return written;
	}

	ChannelMixer::ChannelMixer()
	{
		mSource = 0;
		mOutputSamplerate = 0.0f;
		memset(mMapping, 0, sizeof(mMapping));
	}

	ChannelMixer::~ChannelMixer()
	{
		stop();
	}

	result ChannelMixer::configure(AudioSource *aSource, unsigned int aChannels, float aSamplerate)
	{
		// Validate the whole transaction before changing any state.
		AudioSource *src = aSource ? aSource : mSource;
		if (!src)
			return INVALID_PARAMETER;
		if (!validLayout(aChannels) || !validLayout(src->mChannels))
			return INVALID_PARAMETER;
		if (aSamplerate < 0.0f)
			return INVALID_PARAMETER;
		if (aSamplerate != 0.0f && aSamplerate != aSamplerate) // NaN guard
			return INVALID_PARAMETER;

		float newMapping[MAX_CHANNELS][MAX_CHANNELS];
		buildDefaultMapping(newMapping, src->mChannels, aChannels);

		float newRate = (aSamplerate == 0.0f) ? src->mBaseSamplerate : aSamplerate;

		// Commit the transaction atomically.
		mSource = src;
		mChannels = aChannels;
		memcpy(mMapping, newMapping, sizeof(mMapping));
		mOutputSamplerate = (aSamplerate == 0.0f) ? 0.0f : aSamplerate;
		mBaseSamplerate = newRate;
		return SO_NO_ERROR;
	}

	result ChannelMixer::setSource(AudioSource *aSource)
	{
		if (!aSource || !validLayout(aSource->mChannels))
			return INVALID_PARAMETER;
		if (!mSource || aSource->mChannels != mSource->mChannels)
			return INVALID_PARAMETER;
		mSource = aSource;
		if (mOutputSamplerate == 0.0f)
			mBaseSamplerate = aSource->mBaseSamplerate;
		return SO_NO_ERROR;
	}

	result ChannelMixer::setMapping(unsigned int aDstChannel, unsigned int aSrcChannel, float aGain)
	{
		if (!mSource)
			return INVALID_PARAMETER;
		if (aDstChannel >= mChannels || aSrcChannel >= mSource->mChannels)
			return INVALID_PARAMETER;
		if (aGain != aGain) // NaN guard
			return INVALID_PARAMETER;
		mMapping[aDstChannel][aSrcChannel] = aGain;
		return SO_NO_ERROR;
	}

	float ChannelMixer::getMapping(unsigned int aDstChannel, unsigned int aSrcChannel) const
	{
		if (!mSource || aDstChannel >= mChannels || aSrcChannel >= mSource->mChannels)
			return 0.0f;
		return mMapping[aDstChannel][aSrcChannel];
	}

	result ChannelMixer::resetMapping()
	{
		if (!mSource)
			return INVALID_PARAMETER;
		buildDefaultMapping(mMapping, mSource->mChannels, mChannels);
		return SO_NO_ERROR;
	}

	unsigned int ChannelMixer::getSourceChannels() const
	{
		return mSource ? mSource->mChannels : 0;
	}

	float ChannelMixer::getOutputSamplerate() const
	{
		return mOutputSamplerate == 0.0f ?
			(mSource ? mSource->mBaseSamplerate : 0.0f) : mOutputSamplerate;
	}

	AudioSourceInstance *ChannelMixer::createInstance()
	{
		if (!mSource)
			return 0;
		return new ChannelMixerInstance(this);
	}
};
