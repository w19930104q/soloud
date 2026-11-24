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
#include <new>
#include <string.h>
#include "soloud_automation.h"
#include "soloud_internal.h"

namespace SoLoud
{
	// Fixed stereo output of the automation source.
	static const unsigned int AUTOMATION_CHANNELS = 2;

	AutomationInstance::AutomationInstance(Automation *aParent)
	{
		mSource = 0;
		mFrame = 0;
		mGain = 1.0f;
		mPan = 0.0f;
		mEventIndex = 0;
		mEvents = 0;
		mEventCount = 0;
		mScratch = 0;
		mScratchSize = 0;
		mPull = 0;
		mPullSize = 0;
		mCont = 0;
		mContSize = 0;

		// Mirror the wrapped source's rate so source frames and output frames
		// line up one-to-one and frame-accurate events never drift against the
		// source playhead.
		mChannels = AUTOMATION_CHANNELS;
		float baseRate = aParent->mSource ? aParent->mSource->mBaseSamplerate : 44100.0f;
		mBaseSamplerate = baseRate;
		mSamplerate = baseRate;

		if (aParent->mSource)
		{
			mSource = aParent->mSource->createInstance();
			if (mSource)
			{
				// init() copies base rate, channels and loop flags from the source.
				mSource->init(*aParent->mSource, 0);
				// Consume source frames at their native rate, one for one, so
				// an event at output frame N lands on source frame N.
				mSource->mBaseSamplerate = baseRate;
				mSource->mSamplerate = baseRate;
				mSource->mSetRelativePlaySpeed = 1.0f;
				mSource->mOverallRelativePlaySpeed = 1.0f;
			}
		}

		// Take a private copy of the committed event list. Each instance is
		// therefore self-contained and replay-safe: later commits or
		// clearEvents() on the source replace the source's array and can never
		// invalidate a running instance's events mid-render.
		mEvents = 0;
		mEventCount = aParent->mEventCount;
		if (aParent->mEventCount && aParent->mEvents)
		{
			mEvents = new(std::nothrow) AutomationEvent[aParent->mEventCount];
			if (mEvents)
				memcpy((void *)mEvents, aParent->mEvents, sizeof(AutomationEvent) * aParent->mEventCount);
			else
				mEventCount = 0; // allocation failure: play the source unautomated
		}

		// Pull/working buffers hold at least a full resampler block, planar,
		// across the maximum channel count.
		mScratchSize = SAMPLE_GRANULARITY;
		mScratch = new(std::nothrow) float[mScratchSize * MAX_CHANNELS];
		if (mScratch)
			memset(mScratch, 0, sizeof(float) * mScratchSize * MAX_CHANNELS);

		// The source pull buffer is handed to getAudio() with planes starting
		// at offset 0, so a single pull never spans more than one block.
		mPullSize = SAMPLE_GRANULARITY;
		mPull = new(std::nothrow) float[mPullSize * MAX_CHANNELS];
		if (mPull)
			memset(mPull, 0, sizeof(float) * mPullSize * MAX_CHANNELS);

		// Continuation buffer for the pulls after a loop point seek.
		mContSize = SAMPLE_GRANULARITY;
		mCont = new(std::nothrow) float[mContSize * MAX_CHANNELS];
		if (mCont)
			memset(mCont, 0, sizeof(float) * mContSize * MAX_CHANNELS);
	}

	AutomationInstance::~AutomationInstance()
	{
		delete mSource;
		delete[] mScratch;
		delete[] mPull;
		delete[] mCont;
		delete[] mEvents;
	}

	result AutomationInstance::rewind()
	{
		mFrame = 0;
		mGain = 1.0f;
		mPan = 0.0f;
		mEventIndex = 0;
		if (mSource)
			return mSource->rewind();
		return SO_NO_ERROR;
	}

	bool AutomationInstance::hasEnded()
	{
		if (mSource)
			return mSource->hasEnded();
		return true;
	}

	bool AutomationInstance::ensureCapacity(unsigned int aFrames)
	{
		if (aFrames <= mScratchSize && mScratch && mPull && mCont)
			return true;

		// Grow geometrically to avoid repeated reallocation on large blocks.
		unsigned int newSize = mScratchSize ? mScratchSize : SAMPLE_GRANULARITY;
		while (newSize < aFrames)
			newSize *= 2;

		float *newScratch = new(std::nothrow) float[newSize * MAX_CHANNELS];
		float *newPull = new(std::nothrow) float[newSize * MAX_CHANNELS];
		float *newCont = new(std::nothrow) float[newSize * MAX_CHANNELS];
		if (newScratch == 0 || newPull == 0 || newCont == 0)
		{
			// Keep the existing (smaller) buffers intact on failure.
			delete[] newScratch;
			delete[] newPull;
			delete[] newCont;
			return false;
		}

		memset(newScratch, 0, sizeof(float) * newSize * MAX_CHANNELS);
		memset(newPull, 0, sizeof(float) * newSize * MAX_CHANNELS);
		memset(newCont, 0, sizeof(float) * newSize * MAX_CHANNELS);

		delete[] mScratch;
		delete[] mPull;
		delete[] mCont;
		mScratch = newScratch;
		mPull = newPull;
		mCont = newCont;
		mScratchSize = newSize;
		mPullSize = newSize;
		mContSize = newSize;
		return true;
	}

	unsigned int AutomationInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		if (mSource == 0 || mScratch == 0 || mPull == 0 || mCont == 0 ||
		    !ensureCapacity(aSamplesToRead))
		{
			unsigned int i;
			for (i = 0; i < aSamplesToRead * AUTOMATION_CHANNELS; i++)
				aBuffer[i] = 0;
			return aSamplesToRead;
		}

		// Sources use planar layout: channel planes of mScratchSize floats each.
		unsigned int sourceChannels = mSource->mChannels;
		if (sourceChannels > MAX_CHANNELS)
			sourceChannels = MAX_CHANNELS;

		// Pull the wrapped source for exactly aSamplesToRead frames into mScratch.
		// A raw AudioSourceInstance::getAudio() stops at the end of its data; the
		// engine normally restarts a looping voice by seek()ing it back to its
		// loop point. Performing the same continuation inside the wrapper keeps a
		// direct AutomationInstance::getAudio() identical to a mixer-driven render
		// and makes the output independent of where callers split their buffers.
		unsigned int pulled = 0;
		while (pulled < aSamplesToRead)
		{
			unsigned int want = aSamplesToRead - pulled;
			if (want > mPullSize)
				want = mPullSize;

			unsigned int got = 0;
			if (!mSource->hasEnded() || (mSource->mFlags & AudioSourceInstance::LOOPING))
				got = mSource->getAudio(mPull, want, mPullSize);

			// Looping source reached its end inside this pull: seek back to the
			// loop point and keep pulling into the same plane-0 buffer until the
			// request is satisfied, exactly like the mixer's passthrough loop.
			while (got < want && (mSource->mFlags & AudioSourceInstance::LOOPING) &&
			       mSource->seek(mSource->mLoopPoint, mCont, mContSize * sourceChannels) == SO_NO_ERROR)
			{
				mSource->mLoopCount++;
				unsigned int inc = mSource->getAudio(mPull + got, want - got, mPullSize);
				got += inc;
				if (inc == 0)
					break;
			}

			if (got)
			{
				// Append the pulled planes at the running frame offset.
				unsigned int c;
				for (c = 0; c < sourceChannels; c++)
				{
					memcpy(mScratch + c * mScratchSize + pulled,
					       mPull + c * mPullSize,
					       sizeof(float) * got);
				}
				pulled += got;
			}

			if (got < want)
			{
				// Non-looping source exhausted (or no progress): silence the tail.
				unsigned int c;
				for (c = 0; c < sourceChannels; c++)
					memset(mScratch + c * mScratchSize + pulled, 0,
					       sizeof(float) * (aSamplesToRead - pulled));
				break;
			}
		}

		const AutomationEvent *events = mEvents;
		unsigned int eventCount = mEventCount;

		unsigned int i;
		for (i = 0; i < aSamplesToRead; i++)
		{
			unsigned int absoluteFrame = mFrame + i;

			// Apply every event due on this frame. The list is sorted by
			// (frame, type, submission order), with same-frame/type duplicates
			// already coalesced, so advancing the cursor applies both the gain
			// and the pan event scheduled for this very frame, and the last
			// write for each parameter wins.
			while (mEventIndex < eventCount && events[mEventIndex].mFrame <= absoluteFrame)
			{
				if (events[mEventIndex].mType == Automation::GAIN)
					mGain = events[mEventIndex].mValue;
				else
					mPan = events[mEventIndex].mValue;
				mEventIndex++;
			}

			// Equal-power pan, matching Soloud::setVoicePan_internal():
			// left = cos((pan+1)*pi/4), right = sin((pan+1)*pi/4).
			float angle = (mPan + 1.0f) * (float)M_PI * 0.25f;
			float leftPan = (float)cos(angle);
			float rightPan = (float)sin(angle);

			// Down- or up-mix the planar source channels to mono first. This
			// mirrors panAndExpand()'s mono target summation.
			float mono = 0.0f;
			unsigned int c;
			for (c = 0; c < sourceChannels; c++)
				mono += mScratch[c * mScratchSize + i];
			if (sourceChannels > 1)
				mono /= (float)sourceChannels;

			aBuffer[i] = mono * mGain * leftPan;
			aBuffer[aBufferSize + i] = mono * mGain * rightPan;
		}

		mFrame += aSamplesToRead;
		return aSamplesToRead;
	}

	Automation::Automation()
	{
		mSource = 0;
		mEvents = 0;
		mEventCount = 0;
		mPendingEvents = 0;
		mPendingCount = 0;
		mPendingCapacity = 0;
		mTransactionOpen = false;
		// Automation renders stereo gain/pan automation.
		mChannels = AUTOMATION_CHANNELS;
		mBaseSamplerate = 44100.0f;
	}

	Automation::~Automation()
	{
		stop();
		delete[] mEvents;
		delete[] mPendingEvents;
	}

	void Automation::setSource(AudioSource *aSource)
	{
		mSource = aSource;
		if (mSource)
		{
			mBaseSamplerate = mSource->mBaseSamplerate;
		}
	}

	result Automation::validateEvent(const AutomationEvent &aEvent)
	{
		if (aEvent.mType == GAIN)
		{
			// Negative gain would flip phase and is not a volume multiplier.
			// NaN also fails this comparison.
			if (!(aEvent.mValue >= 0.0f))
				return INVALID_PARAMETER;
			return SO_NO_ERROR;
		}
		if (aEvent.mType == PAN)
		{
			if (aEvent.mValue < -1.0f || aEvent.mValue > 1.0f)
				return INVALID_PARAMETER;
			return SO_NO_ERROR;
		}
		return INVALID_PARAMETER;
	}

	void Automation::beginTransaction()
	{
		// Discard any previously open, uncommitted transaction.
		delete[] mPendingEvents;
		mPendingEvents = 0;
		mPendingCount = 0;
		mPendingCapacity = 0;
		mTransactionOpen = true;
	}

	result Automation::submitEvent(unsigned int aFrame, unsigned int aType, float aValue)
	{
		if (!mTransactionOpen)
			return INVALID_PARAMETER;

		AutomationEvent e;
		e.mFrame = aFrame;
		e.mType = aType;
		e.mValue = aValue;
		result res = validateEvent(e);
		if (res != SO_NO_ERROR)
			return res;

		if (mPendingCount == mPendingCapacity)
		{
			unsigned int newCapacity = mPendingCapacity ? mPendingCapacity * 2 : 8;
			AutomationEvent *grown = new(std::nothrow) AutomationEvent[newCapacity];
			if (grown == 0)
				return OUT_OF_MEMORY;
			if (mPendingCount)
				memcpy(grown, mPendingEvents, sizeof(AutomationEvent) * mPendingCount);
			delete[] mPendingEvents;
			mPendingEvents = grown;
			mPendingCapacity = newCapacity;
		}

		// Submission order is the array order; it is retained as the final
		// tie-break during sorting, which makes same-frame last-write-win work
		// without storing a separate sequence number.
		mPendingEvents[mPendingCount] = e;
		mPendingCount++;
		return SO_NO_ERROR;
	}

	unsigned int Automation::sortAndCoalesce(const AutomationEvent *aEvents, unsigned int aCount, AutomationEvent *aSorted)
	{
		// Stable insertion sort on (frame, type). Input order (submission
		// order) is retained for equal keys, so after coalescing the last
		// submission survives. Per-transaction event counts are modest; an
		// insertion sort keeps this dependency-free and predictable.
		unsigned int i;
		aSorted[0] = aEvents[0];
		for (i = 1; i < aCount; i++)
		{
			AutomationEvent key = aEvents[i];
			int j = (int)i - 1;
			while (j >= 0 &&
				(aSorted[j].mFrame > key.mFrame ||
					(aSorted[j].mFrame == key.mFrame && aSorted[j].mType > key.mType)))
			{
				aSorted[j + 1] = aSorted[j];
				j--;
			}
			aSorted[j + 1] = key;
		}

		// Collapse same-frame/same-type events. The list is now ordered by
		// (frame, type, submission order), so within a run of equal keys the
		// final element is the last submission and wins.
		unsigned int out = 0;
		for (i = 0; i < aCount; i++)
		{
			if (i + 1 < aCount &&
				aSorted[i + 1].mFrame == aSorted[i].mFrame &&
				aSorted[i + 1].mType == aSorted[i].mType)
			{
				continue; // overwritten by a later submission on the same frame
			}
			aSorted[out++] = aSorted[i];
		}
		return out;
	}

	result Automation::commitTransaction()
	{
		if (!mTransactionOpen)
			return INVALID_PARAMETER;

		// Validate every staged event first. Nothing is mutated until the whole
		// batch passes, so a bad input can never leave half a state behind. On
		// failure the transaction stays open and the staged events are retained,
		// letting the caller either fix and retry or rollbackTransaction().
		unsigned int i;
		for (i = 0; i < mPendingCount; i++)
		{
			result res = validateEvent(mPendingEvents[i]);
			if (res != SO_NO_ERROR)
				return res;
		}

		AutomationEvent *newEvents = 0;
		unsigned int newCount = 0;
		if (mPendingCount)
		{
			newEvents = new(std::nothrow) AutomationEvent[mPendingCount];
			if (newEvents == 0)
			{
				// Keep both the committed list and the open transaction intact.
				return OUT_OF_MEMORY;
			}
			newCount = sortAndCoalesce(mPendingEvents, mPendingCount, newEvents);
		}

		// All validation and allocation succeeded past this point: publish.
		// The old committed array is released only after the new one is fully
		// built, and in-flight instances own private event copies, so the
		// commit is effectively a single atomic pointer swap.
		AutomationEvent *oldEvents = mEvents;
		mEvents = newEvents;
		mEventCount = newCount;
		delete[] oldEvents;

		// Close the transaction and free the staging area.
		delete[] mPendingEvents;
		mPendingEvents = 0;
		mPendingCount = 0;
		mPendingCapacity = 0;
		mTransactionOpen = false;
		return SO_NO_ERROR;
	}

	void Automation::rollbackTransaction()
	{
		delete[] mPendingEvents;
		mPendingEvents = 0;
		mPendingCount = 0;
		mPendingCapacity = 0;
		mTransactionOpen = false;
	}

	result Automation::setEvents(const AutomationEvent *aEvents, unsigned int aEventCount)
	{
		if (aEventCount && aEvents == 0)
			return INVALID_PARAMETER;

		beginTransaction();
		unsigned int i;
		for (i = 0; i < aEventCount; i++)
		{
			result res = validateEvent(aEvents[i]);
			if (res != SO_NO_ERROR)
			{
				rollbackTransaction();
				return res;
			}
			res = submitEvent(aEvents[i].mFrame, aEvents[i].mType, aEvents[i].mValue);
			if (res != SO_NO_ERROR)
			{
				rollbackTransaction();
				return res;
			}
		}
		result commitResult = commitTransaction();
		if (commitResult != SO_NO_ERROR)
		{
			// commitTransaction() leaves a failed transaction open by design;
			// a one-shot setEvents must not leak that staging state.
			rollbackTransaction();
		}
		return commitResult;
	}

	void Automation::clearEvents()
	{
		// Drop any open transaction first so no staged events survive.
		rollbackTransaction();
		delete[] mEvents;
		mEvents = 0;
		mEventCount = 0;
	}

	unsigned int Automation::getEventCount() const
	{
		return mEventCount;
	}

	result Automation::getEvent(unsigned int aIndex, AutomationEvent &aEvent) const
	{
		if (aIndex >= mEventCount)
			return INVALID_PARAMETER;
		aEvent = mEvents[aIndex];
		return SO_NO_ERROR;
	}

	AudioSourceInstance *Automation::createInstance()
	{
		AutomationInstance *instance = new(std::nothrow) AutomationInstance(this);
		return instance;
	}
};
