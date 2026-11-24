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
#include <new>
#include <string.h>
#include "soloud_crossfade.h"

namespace SoLoud
{
	// Channel layouts the engine's panAndExpand() understands.
	static bool validChannelCount(unsigned int aChannels)
	{
		return aChannels == 1 || aChannels == 2 || aChannels == 4 ||
			aChannels == 6 || aChannels == 8;
	}

	static bool finiteTime(time aTime)
	{
		return aTime == aTime && aTime > -1e30 && aTime < 1e30;
	}

	static bool finiteFloat(float aValue)
	{
		return aValue == aValue && aValue > -1e30f && aValue < 1e30f;
	}

	CrossfadeSubVoice::CrossfadeSubVoice()
	{
		mInstance = 0;
		mOutputSamplerate = 0;
		mSourceSamplerate = 0;
		mSourceChannels = 0;
		mStartOffset = 0;
		mSeekScratch = 0;
		mSeekScratchSize = 0;
		mBlock = 0;
		mBlockCapacity = 0;
		mBlockLength = 0;
		mBlockStart = 0;
		mFracPos = 0;
		mRealFrames = 0;
		mInstanceEnded = false;
	}

	CrossfadeSubVoice::~CrossfadeSubVoice()
	{
		delete mInstance;
		delete[] mSeekScratch;
		delete[] mBlock;
	}

	result CrossfadeSubVoice::init(AudioSource &aSource, time aStartOffset, float aOutputSamplerate)
	{
		if (aSource.mBaseSamplerate <= 0 || !finiteFloat(aSource.mBaseSamplerate) ||
			aSource.mChannels < 1 || aSource.mChannels > MAX_CHANNELS ||
			aOutputSamplerate <= 0 || !finiteFloat(aOutputSamplerate) ||
			aStartOffset < 0 || !finiteTime(aStartOffset))
		{
			return INVALID_PARAMETER;
		}

		AudioSourceInstance *instance = aSource.createInstance();
		if (instance == 0)
		{
			return OUT_OF_MEMORY;
		}
		// playIndex is only used by the engine for handles; sub-instances
		// never appear in the engine's voice table.
		instance->init(aSource, 0);
		instance->mAudioSourceID = aSource.mAudioSourceID;

		// Generic "tape" seek scratch: one native block's worth per channel.
		unsigned int seekScratchSize = SAMPLE_GRANULARITY * aSource.mChannels;
		float *seekScratch = new (std::nothrow) float[seekScratchSize];
		if (seekScratch == 0)
		{
			delete instance;
			return OUT_OF_MEMORY;
		}

		if (aStartOffset > 0)
		{
			result seekres = instance->seek(aStartOffset, seekScratch, seekScratchSize);
			if (seekres != SO_NO_ERROR)
			{
				delete instance;
				delete[] seekScratch;
				return seekres;
			}
		}

		// Everything succeeded; commit to this object.
		mInstance = instance;
		mSeekScratch = seekScratch;
		mSeekScratchSize = seekScratchSize;
		mOutputSamplerate = aOutputSamplerate;
		mSourceSamplerate = aSource.mBaseSamplerate;
		mSourceChannels = aSource.mChannels;
		mStartOffset = aStartOffset;
		mBlockStart = 0;
		mBlockLength = 0;
		mFracPos = 0;
		mRealFrames = 0;
		mInstanceEnded = false;
		return SO_NO_ERROR;
	}

	result CrossfadeSubVoice::rewind()
	{
		if (mInstance == 0)
			return INVALID_PARAMETER;

		result r = mInstance->rewind();
		if (r != SO_NO_ERROR)
			return r;

		if (mStartOffset > 0)
		{
			r = mInstance->seek(mStartOffset, mSeekScratch, mSeekScratchSize);
			if (r != SO_NO_ERROR)
				return r;
		}
		mBlockStart = 0;
		mBlockLength = 0;
		mFracPos = 0;
		mRealFrames = 0;
		mInstanceEnded = false;
		return SO_NO_ERROR;
	}

	bool CrossfadeSubVoice::hasEnded()
	{
		// The sub-instance has ended, and the resampler has moved past the
		// last real native frame (everything after it is silence padding).
		return mInstanceEnded && floor(mFracPos) >= (double)mRealFrames;
	}

	void CrossfadeSubVoice::pullAppend(unsigned int aFrames)
	{
		if (aFrames == 0)
			return;

		unsigned int required = mBlockLength + aFrames;
		if (required > mBlockCapacity)
		{
			float *newbuf = new (std::nothrow) float[required * mSourceChannels];
			if (newbuf == 0)
				return;
			// Planar copy: each channel's range moves independently. Guard the
			// zero-length case so the first growth (mBlock still null) never
			// passes a null pointer to memcpy.
			if (mBlockLength > 0)
			{
				for (unsigned int c = 0; c < mSourceChannels; c++)
					memcpy(newbuf + c * required, mBlock + c * mBlockCapacity,
						sizeof(float) * mBlockLength);
			}
			delete[] mBlock;
			mBlock = newbuf;
			mBlockCapacity = required;
		}

		// The sub-instance writes planar data (channel c at c*aFrames). Pull
		// into a scratch buffer first, then scatter into the block's planes.
		float *pull = new (std::nothrow) float[aFrames * mSourceChannels];
		if (pull == 0)
			return;

		unsigned int fetched = 0;
		while (fetched < aFrames && !mInstanceEnded)
		{
			unsigned int got = mInstance->getAudio(pull, aFrames - fetched, aFrames - fetched);
			if (got == 0)
				break;
			for (unsigned int c = 0; c < mSourceChannels; c++)
				memcpy(mBlock + c * mBlockCapacity + mBlockLength + fetched,
					pull + c * (aFrames - fetched), sizeof(float) * got);
			fetched += got;
			if (mInstance->hasEnded())
			{
				mInstanceEnded = true;
				break;
			}
		}
		if (mInstance->hasEnded())
			mInstanceEnded = true;
		mRealFrames += fetched;

		// Pad the remainder with silence, as the engine does for ended voices.
		if (fetched < aFrames)
		{
			for (unsigned int c = 0; c < mSourceChannels; c++)
				memset(mBlock + c * mBlockCapacity + mBlockLength + fetched, 0,
					sizeof(float) * (aFrames - fetched));
		}
		delete[] pull;
		mBlockLength += aFrames;
	}

	void CrossfadeSubVoice::accumulate(float *aBuffer, unsigned int aFrames, unsigned int aBufferSize,
		unsigned int aChannels, float aGain)
	{
		if (mInstance == 0 || aFrames == 0)
			return;

		// Linear rate conversion driven by an absolute fractional source
		// position. mBlock always contains native frames
		// [mBlockStart, mBlockStart+mBlockLength); missing tail frames are
		// clamped to the block edge (last sample, or silence once the source
		// has ended and only padding exists).
		double step = mSourceSamplerate / mOutputSamplerate;
		double endPos = mFracPos + step * (aFrames - 1);
		// Highest source frame the interpolation reads (as f1 = frameAt(idx+1)).
		unsigned int lastIdx = (unsigned int)floor(endPos) + 1;

		// Extend the block so every frame the interpolation may touch exists.
		// blockEnd is the first frame NOT in the block, hence >= comparison.
		unsigned int blockEnd = mBlockStart + mBlockLength;
		if (lastIdx >= blockEnd)
			pullAppend(lastIdx + 1 - blockEnd);

		// An allocation failure can leave us without any data at all; emit
		// silence for this window rather than dereference a null block.
		if (mBlockLength == 0)
		{
			for (unsigned int c = 0; c < aChannels; c++)
				memset(aBuffer + c * aBufferSize, 0, sizeof(float) * aFrames);
			return;
		}

		double pos = mFracPos;
		for (unsigned int i = 0; i < aFrames; i++)
		{
			unsigned int idx = (unsigned int)floor(pos);
			float frac = (float)(pos - floor(pos));
			// Planar sample accessors. Indices are clamped to the block's
			// frame range so a short pull cannot read past the buffer.
			unsigned int local0 = idx - mBlockStart;
			unsigned int local1 = idx + 1 - mBlockStart;
			if (local0 >= mBlockLength) local0 = mBlockLength - 1;
			if (local1 >= mBlockLength) local1 = mBlockLength - 1;

			for (unsigned int outCh = 0; outCh < aChannels; outCh++)
			{
				// Map the output channel onto a sub-source channel. Upmix
				// duplicates channels round-robin, downmix averages a band.
				float s0, s1;
				if (mSourceChannels == aChannels)
				{
					s0 = mBlock[outCh * mBlockCapacity + local0];
					s1 = mBlock[outCh * mBlockCapacity + local1];
				}
				else if (mSourceChannels < aChannels)
				{
					unsigned int srcCh = (unsigned int)((unsigned long long)outCh * mSourceChannels / aChannels);
					s0 = mBlock[srcCh * mBlockCapacity + local0];
					s1 = mBlock[srcCh * mBlockCapacity + local1];
				}
				else
				{
					unsigned int first = (unsigned int)((unsigned long long)outCh * mSourceChannels / aChannels);
					unsigned int last = (unsigned int)((unsigned long long)(outCh + 1) * mSourceChannels / aChannels);
					if (last <= first)
						last = first + 1;
					unsigned int span = last - first;
					float sum0 = 0, sum1 = 0;
					for (unsigned int k = first; k < last; k++)
					{
						sum0 += mBlock[k * mBlockCapacity + local0];
						sum1 += mBlock[k * mBlockCapacity + local1];
					}
					s0 = sum0 / (float)span;
					s1 = sum1 / (float)span;
				}
				aBuffer[i + outCh * aBufferSize] += (s0 + (s1 - s0) * frac) * aGain;
			}
			pos += step;
		}

		// Compact: drop native frames that no future interpolation can reach.
		// floor(nextPos) is the first frame an f0 lookup may still request, so
		// it becomes the new block start. A zero-length block is valid.
		double nextPos = mFracPos + step * aFrames;
		unsigned int newStart = (unsigned int)floor(nextPos);
		mFracPos = nextPos;
		if (newStart > mBlockStart && mBlockLength > 0)
		{
			if (newStart > mBlockStart + mBlockLength)
				newStart = mBlockStart + mBlockLength;
			unsigned int keep = mBlockStart + mBlockLength - newStart;
			if (keep > 0)
			{
				unsigned int drop = newStart - mBlockStart;
				for (unsigned int c = 0; c < mSourceChannels; c++)
					memmove(mBlock + c * mBlockCapacity,
						mBlock + c * mBlockCapacity + drop,
						sizeof(float) * keep);
			}
			mBlockLength = keep;
			mBlockStart = newStart;
		}
	}

	CrossfadeInstance::CrossfadeInstance(Crossfade *aParent)
	{
		mParent = aParent;
		mFrom = 0;
		mTo = 0;
		mFadePosition = 0;
		// Adopt the configured output format immediately: sub-voices are built
		// before the engine's own AudioSourceInstance::init() runs.
		mBaseSamplerate = aParent->mBaseSamplerate;
		mSamplerate = aParent->mBaseSamplerate;
		mChannels = aParent->mChannels;
		// Same protection policy as Queue: the composite instance itself is
		// managed explicitly rather than being a candidate for voice stealing.
		mFlags |= PROTECTED;
	}

	CrossfadeInstance::~CrossfadeInstance()
	{
		delete mFrom;
		delete mTo;
	}

	result CrossfadeInstance::init()
	{
		CrossfadeSubVoice *from = new (std::nothrow) CrossfadeSubVoice();
		CrossfadeSubVoice *to = new (std::nothrow) CrossfadeSubVoice();
		if (from == 0 || to == 0)
		{
			delete from;
			delete to;
			return OUT_OF_MEMORY;
		}

		result r = from->init(*mParent->mFromSource, mParent->mFromOffset, mBaseSamplerate);
		if (r != SO_NO_ERROR)
		{
			delete from;
			delete to;
			return r;
		}
		r = to->init(*mParent->mToSource, mParent->mToOffset, mBaseSamplerate);
		if (r != SO_NO_ERROR)
		{
			delete from;
			delete to;
			return r;
		}

		// Commit only after both sub-voices are fully built, so a failed
		// configuration never leaves a half-initialized instance behind.
		mFrom = from;
		mTo = to;
		mFadePosition = 0;
		return SO_NO_ERROR;
	}

	unsigned int CrossfadeInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		// Unconfigured or failed source: emit silence, never garbage.
		if (mFrom == 0 || mTo == 0)
		{
			for (unsigned int c = 0; c < mChannels; c++)
				memset(aBuffer + c * aBufferSize, 0, sizeof(float) * aSamplesToRead);
			return aSamplesToRead;
		}

		unsigned int fadeFrames = (unsigned int)(mBaseSamplerate * mParent->mFadeTime + 0.5f);
		if (fadeFrames < 1)
			fadeFrames = 1;

		for (unsigned int c = 0; c < mChannels; c++)
			memset(aBuffer + c * aBufferSize, 0, sizeof(float) * aSamplesToRead);

		unsigned int done = 0;
		while (done < aSamplesToRead)
		{
			unsigned int chunk = aSamplesToRead - done;
			float fromGain, toGain;

			if (mFadePosition >= fadeFrames)
			{
				// Post-fade tail: only the "to" source remains audible.
				fromGain = mParent->mFromEndGain;
				toGain = 1.0f - mParent->mFromEndGain;
				// Keep ticking an inaudible but still-running sub-voice so its
				// end detection stays accurate; only skip fully drained ones.
				if (fromGain != 0.0f || !mFrom->hasEnded())
					mFrom->accumulate(aBuffer + done, chunk, aBufferSize, mChannels, fromGain);
				if (toGain != 0.0f || !mTo->hasEnded())
					mTo->accumulate(aBuffer + done, chunk, aBufferSize, mChannels, toGain);
				done += chunk;
				continue;
			}

			unsigned int fadeLeft = fadeFrames - mFadePosition;
			if (chunk > fadeLeft)
				chunk = fadeLeft;

			// Endpoint-inclusive linear ramp. Frame 0 lands exactly on the
			// start gains and the last fade frame on the end gains.
			for (unsigned int i = 0; i < chunk; i++)
			{
				float t = (float)(mFadePosition + i) / (float)(fadeFrames - 1);
				fromGain = mParent->mFromStartGain +
					(mParent->mFromEndGain - mParent->mFromStartGain) * t;
				toGain = 1.0f - fromGain;

				// One output frame at a time keeps both sub-voices
				// sample-accurate against the ramp.
				if (fromGain != 0.0f || !mFrom->hasEnded())
					mFrom->accumulate(aBuffer + done + i, 1, aBufferSize, mChannels, fromGain);
				if (toGain != 0.0f || !mTo->hasEnded())
					mTo->accumulate(aBuffer + done + i, 1, aBufferSize, mChannels, toGain);
			}
			mFadePosition += chunk;
			done += chunk;
		}

		return aSamplesToRead;
	}

	bool CrossfadeInstance::hasEnded()
	{
		if (mFrom == 0 || mTo == 0)
			return false;
		return mFrom->hasEnded() && mTo->hasEnded();
	}

	result CrossfadeInstance::rewind()
	{
		if (mFrom == 0 || mTo == 0)
			return NOT_IMPLEMENTED;
		result r = mFrom->rewind();
		if (r != SO_NO_ERROR)
			return r;
		r = mTo->rewind();
		if (r != SO_NO_ERROR)
			return r;
		mFadePosition = 0;
		mStreamPosition = 0.0f;
		return SO_NO_ERROR;
	}

	Crossfade::Crossfade()
	{
		mFromSource = 0;
		mToSource = 0;
		mFadeTime = 0;
		mFromStartGain = 1.0f;
		mFromEndGain = 0.0f;
		mFromOffset = 0;
		mToOffset = 0;
		// Unconfigured until setParams() succeeds.
		mBaseSamplerate = 0;
		mChannels = 0;
	}

	Crossfade::~Crossfade()
	{
	}

	result Crossfade::setParams(AudioSource &aFromSource, AudioSource &aToSource,
		time aFadeTime,
		float aFromStartGain, float aFromEndGain,
		time aFromOffset, time aToOffset)
	{
		// Validate everything before touching any member, so a failed call
		// leaves the previous configuration (if any) completely intact.
		if (&aFromSource == &aToSource)
			return INVALID_PARAMETER;
		if (aFadeTime <= 0 || !finiteTime(aFadeTime))
			return INVALID_PARAMETER;
		if (!finiteFloat(aFromStartGain) || !finiteFloat(aFromEndGain))
			return INVALID_PARAMETER;
		if (aFromOffset < 0 || !finiteTime(aFromOffset) ||
			aToOffset < 0 || !finiteTime(aToOffset))
			return INVALID_PARAMETER;
		if (aFromSource.mBaseSamplerate <= 0 || !finiteFloat(aFromSource.mBaseSamplerate) ||
			aFromSource.mChannels < 1 || aFromSource.mChannels > MAX_CHANNELS)
			return INVALID_PARAMETER;
		if (aToSource.mBaseSamplerate <= 0 || !finiteFloat(aToSource.mBaseSamplerate) ||
			aToSource.mChannels < 1 || aToSource.mChannels > MAX_CHANNELS)
			return INVALID_PARAMETER;

		// Default output format follows the "to" source unless overridden via
		// setOutputParams() before this call.
		float outRate = (mBaseSamplerate > 0) ? mBaseSamplerate : aToSource.mBaseSamplerate;
		unsigned int outChannels = (mChannels > 0) ? mChannels : aToSource.mChannels;
		if (outRate <= 0 || !finiteFloat(outRate) || !validChannelCount(outChannels))
			return INVALID_PARAMETER;

		mFromSource = &aFromSource;
		mToSource = &aToSource;
		mFadeTime = aFadeTime;
		mFromStartGain = aFromStartGain;
		mFromEndGain = aFromEndGain;
		mFromOffset = aFromOffset;
		mToOffset = aToOffset;
		mBaseSamplerate = outRate;
		mChannels = outChannels;
		return SO_NO_ERROR;
	}

	result Crossfade::setOutputParams(float aSamplerate, unsigned int aChannels)
	{
		if (aSamplerate <= 0 || !finiteFloat(aSamplerate) || !validChannelCount(aChannels))
			return INVALID_PARAMETER;
		mBaseSamplerate = aSamplerate;
		mChannels = aChannels;
		return SO_NO_ERROR;
	}

	CrossfadeInstance * Crossfade::createInstance()
	{
		if (mFromSource == 0 || mToSource == 0)
		{
			// Refuse to build a half-working instance from an unconfigured
			// source. The engine treats a null instance as allocation failure.
			return 0;
		}

		CrossfadeInstance *instance = new (std::nothrow) CrossfadeInstance(this);
		if (instance == 0)
			return 0;

		result r = instance->init();
		if (r != SO_NO_ERROR)
		{
			delete instance;
			return 0;
		}
		return instance;
	}
};
