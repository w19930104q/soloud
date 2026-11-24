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

#ifndef SOLOUD_AUTOMATION_H
#define SOLOUD_AUTOMATION_H

#include "soloud.h"

namespace SoLoud
{
	class Automation;

	// One frame-timed automation event.
	struct AutomationEvent
	{
		// Frame at which the event becomes effective (0-based).
		unsigned int mFrame;
		// Automation::EVENT_TYPE selector; other values are rejected.
		unsigned int mType;
		// New gain (Automation::GAIN) or pan (Automation::PAN) value.
		float mValue;
	};

	// Reusable, frame-accurate gain (volume) and pan automation playback source.
	//
	// Automation wraps another audio source and applies a list of frame-timed
	// events to its output. Events are indexed in output frames (relative to the
	// start of the instance, frame 0 being the first rendered sample), so the
	// rendered result is independent of how the mixer splits the stream into
	// buffers: rendering the material in one go or in many small pieces produces
	// bit-identical output.
	//
	// Events take effect at the beginning of the frame they are scheduled on,
	// i.e. an event at frame N affects output frame N. When several events of
	// the same parameter target the same frame, the event submitted last wins.
	// Gain and pan events scheduled on the same frame always take effect on the
	// same frame.
	//
	// The event list is edited through an explicit transaction
	// (beginTransaction / submitEvent / commitTransaction). A transaction is a
	// single replayable, all-or-nothing commit: submitEvent() refuses invalid
	// input before it can enter the staging area, and if the engine runs out of
	// memory commitTransaction() fails without publishing anything, so the
	// automation keeps serving the previously committed event list unchanged.
	// No partial state is ever visible.
	//
	// The automation source always outputs stereo (2 channels). To keep all
	// gain and panning inside the automation, play it with playBackground() or
	// leave volume at 1.0; applying external setPan()/setVolume() on the voice
	// would multiply on top of the automated values.
	class AutomationInstance : public AudioSourceInstance
	{
	public:
		AutomationInstance(Automation *aParent);
		virtual ~AutomationInstance();

		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
		virtual bool hasEnded();
		virtual result rewind();

	private:
		friend class Automation;
		// Grow the planar scratch/pull buffers so each plane holds at least
		// aFrames. Returns false on allocation failure.
		bool ensureCapacity(unsigned int aFrames);
		// Wrapped source instance, owned by this instance.
		AudioSourceInstance *mSource;
		// Number of output frames already rendered (frame counter for events).
		unsigned int mFrame;
		// Currently active gain multiplier.
		float mGain;
		// Currently active pan (-1 left .. 1 right).
		float mPan;
		// Index of the next event candidate in the event snapshot.
		unsigned int mEventIndex;
		// Private copy of the committed event list for this instance. Each
		// instance owns its snapshot, so a later commit on the Automation
		// source cannot invalidate events a running instance is reading.
		const AutomationEvent *mEvents;
		unsigned int mEventCount;
		// Scratch used to hold the source frames for the current getAudio()
		// block, planar layout (mScratchSize frames per source plane).
		float *mScratch;
		unsigned int mScratchSize;
		// Dedicated pull buffer handed to the wrapped source. It always starts
		// at frame 0 of each plane, so the source's planar writes land at the
		// correct offsets. Separate from mScratch because the latter accumulates
		// the whole block. mCont is the same kind of plane-0 buffer used for the
		// pulls that resume a source after it loops.
		float *mPull;
		unsigned int mPullSize;
		float *mCont;
		unsigned int mContSize;
	};

	class Automation : public AudioSource
	{
	public:
		// Event parameter selectors.
		enum EVENT_TYPE
		{
			// Multiplies the source signal. Any non-negative value is valid.
			GAIN = 0,
			// Stereo pan, -1 (full left) .. 1 (full right), 0 center.
			PAN = 1
		};

		Automation();
		virtual ~Automation();

		// Attach the audio source to be automated. Does not take ownership;
		// the caller must keep aSource alive for as long as the automation is
		// in use. Must be called before play(). Passing NULL clears it.
		void setSource(AudioSource *aSource);

		// Open an editing transaction. Ends any transaction already in
		// progress by discarding its uncommitted events.
		void beginTransaction();
		// Queue an event into the open transaction. Returns SO_NO_ERROR, or
		// INVALID_PARAMETER if no transaction is open or the event is invalid
		// (unknown type, negative gain, pan outside [-1, 1]); an invalid event
		// is refused at the boundary and never enters the staging area, while
		// the transaction stays open so the caller can correct and retry.
		result submitEvent(unsigned int aFrame, unsigned int aType, float aValue);
		// Validate, sort and atomically publish the queued events. On success
		// the transaction is closed and the new list replaces the old one in a
		// single swap. On failure (out of memory) nothing is published: the
		// previously committed list is untouched and the transaction stays open
		// with its staged events preserved, so it can be retried or discarded
		// with rollbackTransaction(). (Each event is also validated up front by
		// submitEvent(), so an invalid value can never reach this point.)
		result commitTransaction();
		// Throw away the open transaction without changing the committed
		// events. Closes the transaction.
		void rollbackTransaction();

		// One-shot convenience: replace the whole event list atomically. The
		// committed list only changes when every input event is valid and the
		// required memory can be allocated; otherwise the old list survives.
		result setEvents(const AutomationEvent *aEvents, unsigned int aEventCount);
		// Drop all committed events (and any open transaction).
		void clearEvents();

		// Number of events in the currently committed list.
		unsigned int getEventCount() const;
		// Read back a committed event. Returns INVALID_PARAMETER if aIndex is
		// out of range.
		result getEvent(unsigned int aIndex, AutomationEvent &aEvent) const;

		virtual AudioSourceInstance *createInstance();

	private:
		friend class AutomationInstance;
		// Validate one event in isolation.
		static result validateEvent(const AutomationEvent &aEvent);
		// Sort by (frame, type, submission order) and collapse same-frame /
		// same-type duplicates keeping the last submission. Writes the result
		// into aSorted; returns the resulting event count. aSorted must hold
		// at least aCount entries.
		static unsigned int sortAndCoalesce(const AutomationEvent *aEvents, unsigned int aCount, AutomationEvent *aSorted);

		// Wrapped source, not owned.
		AudioSource *mSource;

		// Committed, immutable-at-runtime event list (owned).
		AutomationEvent *mEvents;
		unsigned int mEventCount;

		// Transaction staging area (owned only while a transaction is open).
		AutomationEvent *mPendingEvents;
		unsigned int mPendingCount;
		unsigned int mPendingCapacity;
		bool mTransactionOpen;
	};
};

#endif
