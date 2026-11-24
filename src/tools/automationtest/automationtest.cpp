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

/*********************************************************************************
*
* Regression tests for the frame-accurate gain/pan Automation audio source.
*
* Two layers are exercised:
*
*   - Unit level: AutomationInstance::getAudio() is called directly with
*     arbitrary buffer sizes. This bypasses the engine mixer (and its global
*     soft-clipper), so exact theoretical magnitudes, exact silencing of a
*     panned-away channel and exact frame boundaries can be asserted.
*
*   - Integration level: the full engine renders through the NULL driver
*     (deterministic, no hardware) with many different buffer cut points. The
*     same material rendered in one long mix and in many tiny, awkwardly sized
*     mixes must come out bit-identical, which is the actual user-facing
*     guarantee: it sounds identical however the editor splits the stream.
*
* The tests cover:
*
*   1. Buffer-split invariance (unit and integration).
*   2. Frame-accurate gain and pan events, with the exact change frame asserted.
*   3. Same-frame last-write-wins for both gain and pan.
*   4. Transactional, all-or-nothing event commits: invalid input can never
*      leave a partially updated event list behind.
*   5. Replayability and per-instance event snapshot isolation.
*
**********************************************************************************/

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "soloud.h"
#include "soloud_automation.h"
#include "soloud_wav.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static int gTests = 0;
static int gErrors = 0;

#define CHECK(x)                                                      \
	do                                                                 \
	{                                                                  \
		gTests++;                                                      \
		if (!(x))                                                      \
		{                                                              \
			gErrors++;                                                 \
			printf("FAIL line %d: %s\n", __LINE__, #x);              \
		}                                                              \
	} while (0)

#define CHECK_RES(x)                                                            \
	do                                                                          \
	{                                                                           \
		gTests++;                                                               \
		SoLoud::result _r = (x);                                               \
		if (_r != SoLoud::SO_NO_ERROR)                                         \
		{                                                                       \
			gErrors++;                                                          \
			printf("FAIL line %d: %s -> error %u\n", __LINE__, #x, _r);       \
		}                                                                       \
	} while (0)

#define CHECK_RES_FAIL(x)                                                       \
	do                                                                          \
	{                                                                           \
		gTests++;                                                               \
		SoLoud::result _r = (x);                                               \
		if (_r == SoLoud::SO_NO_ERROR)                                         \
		{                                                                       \
			gErrors++;                                                          \
			printf("FAIL line %d: %s expected an error but succeeded\n",       \
			       __LINE__, #x);                                              \
		}                                                                       \
	} while (0)

static const unsigned int SAMPLERATE = 44100;
static const unsigned int CHANNELS = 2;
static const float EPS = 1e-6f;
// Pull buffers handed to getAudio must be at least this many frames per plane.
// Kept above SAMPLE_GRANULARITY (512) so a single request can also exercise the
// instance's on-demand scratch growth for large mixer blocks.
static const unsigned int UNIT_BUF = 2048;

// Build a deterministic looping mono source: every sample is a fixed,
// non-trivial constant, so an automated gain/pan maps to an exact output level.
static void makeConstantSource(SoLoud::Wav &aWav, float aValue, unsigned int aFrames, bool aLoop)
{
	float *data = new float[aFrames];
	for (unsigned int i = 0; i < aFrames; i++)
		data[i] = aValue;
	aWav.loadRawWave(data, aFrames, (float)SAMPLERATE, 1, true, true);
	aWav.setLooping(aLoop);
}

// Unit-level render: pull the automation instance directly, one getAudio() call
// per chunk in the repeating pattern, into a planar stereo buffer (left then
// right, each total frames long). Bypasses the mixer and the soft-clipper.
static void renderUnit(SoLoud::Automation &aAutomation,
                       float *aOut,
                       unsigned int aTotalFrames,
                       const unsigned int *aPattern,
                       unsigned int aPatternLen)
{
	SoLoud::AutomationInstance *inst =
	    (SoLoud::AutomationInstance *)aAutomation.createInstance();
	CHECK(inst != 0);

	float buf[UNIT_BUF * CHANNELS];
	unsigned int done = 0;
	unsigned int pi = 0;
	while (done < aTotalFrames)
	{
		unsigned int chunk = aPattern[pi % aPatternLen];
		if (chunk > UNIT_BUF)
			chunk = UNIT_BUF;
		if (chunk > aTotalFrames - done)
			chunk = aTotalFrames - done;

		memset(buf, 0, sizeof(buf));
		unsigned int read = inst->getAudio(buf, chunk, UNIT_BUF);
		// A finished source may return fewer frames; treat the rest as silence.
		if (read < chunk)
			memset(buf + read, 0, sizeof(float) * (chunk - read) * CHANNELS);

		for (unsigned int i = 0; i < chunk; i++)
		{
			aOut[done + i] = buf[i];                            // left plane
			aOut[aTotalFrames + done + i] = buf[UNIT_BUF + i]; // right plane
		}
		done += chunk;
		pi++;
	}

	delete inst;
}

// Integration-level render through a fresh Soloud/NULL-driver, one mix() call
// per pattern chunk. Output is stored interleaved stereo.
static void renderMixer(SoLoud::Automation &aAutomation,
                        float *aOut,
                        unsigned int aTotalFrames,
                        const unsigned int *aPattern,
                        unsigned int aPatternLen)
{
	SoLoud::Soloud soloud;
	SoLoud::result res = soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF,
	                                 SoLoud::Soloud::NULLDRIVER,
	                                 SAMPLERATE, 2048, CHANNELS);
	CHECK_RES(res);

	// playBackground keeps the mixer's own channel volumes at 1.0, so only
	// the automation controls gain and pan.
	soloud.playBackground(aAutomation);

	unsigned int done = 0;
	unsigned int pi = 0;
	while (done < aTotalFrames)
	{
		unsigned int chunk = aPattern[pi % aPatternLen];
		if (chunk > 2048)
			chunk = 2048;
		if (chunk > aTotalFrames - done)
			chunk = aTotalFrames - done;
		float buf[2048 * 2];
		soloud.mix(buf, chunk);
		memcpy(aOut + done * 2, buf, sizeof(float) * chunk * 2);
		done += chunk;
		pi++;
	}

	soloud.deinit();
}

static bool closeEnough(float a, float b, float aTol)
{
	return fabsf(a - b) <= aTol;
}

static bool planarEqual(const float *a, const float *b, unsigned int aFrames)
{
	for (unsigned int p = 0; p < CHANNELS; p++)
	{
		const float *pa = a + p * aFrames;
		const float *pb = b + p * aFrames;
		for (unsigned int i = 0; i < aFrames; i++)
		{
			if (pa[i] != pb[i])
			{
				printf("    first difference at plane %u frame %u: %.9g != %.9g\n",
				       p, i, pa[i], pb[i]);
				return false;
			}
		}
	}
	return true;
}

static bool interleavedEqual(const float *a, const float *b, unsigned int aFloats)
{
	for (unsigned int i = 0; i < aFloats; i++)
	{
		if (a[i] != b[i])
		{
			printf("    first difference at float %u: %.9g != %.9g\n", i, a[i], b[i]);
			return false;
		}
	}
	return true;
}

// Equal-power pan gains used by the automation (cos((pan+1)*pi/4) / sin(...)).
static void panGains(float aPan, float &aLeft, float &aRight)
{
	float angle = (aPan + 1.0f) * (float)M_PI * 0.25f;
	aLeft = (float)cos(angle);
	aRight = (float)sin(angle);
}

// 1. Buffer-split invariance at both layers.
static void testBufferSplitInvariance()
{
	const unsigned int total = 4096;
	float *unitRef = new float[total * CHANNELS];
	float *unitCand = new float[total * CHANNELS];
	float *mixRef = new float[total * 2];
	float *mixCand = new float[total * 2];

	const unsigned int one[] = { total };
	// Awkward chunk sizes (ones, primes, powers of two) so event frames fall
	// inside, at the start and at the end of buffers.
	const unsigned int patterns[][6] = {
		{ 1, 1, 1, 1, 1, 1 },
		{ 3, 7, 5, 11, 13, 17 },
		{ 64, 128, 32, 256, 16, 96 },
		{ 100, 200, 50, 300, 77, 19 },
		{ 512, 512, 512, 512, 512, 512 },
	};

	SoLoud::Wav wav;
	makeConstantSource(wav, 0.5f, 37, true);
	SoLoud::Automation automation;
	automation.setSource(&wav);

	SoLoud::AutomationEvent events[6];
	// Gain and pan deliberately share frames; both must apply together.
	events[0] = { 0,    SoLoud::Automation::GAIN, 1.0f };
	events[1] = { 100,  SoLoud::Automation::PAN, -1.0f };
	events[2] = { 100,  SoLoud::Automation::GAIN, 0.25f };
	events[3] = { 1024, SoLoud::Automation::GAIN, 0.8f };
	events[4] = { 1024, SoLoud::Automation::PAN, 1.0f };
	events[5] = { 3000, SoLoud::Automation::PAN, 0.0f };
	CHECK_RES(automation.setEvents(events, 6));

	renderUnit(automation, unitRef, total, one, 1);
	renderMixer(automation, mixRef, total, one, 1);

	for (unsigned p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++)
	{
		renderUnit(automation, unitCand, total, patterns[p], 6);
		CHECK(planarEqual(unitRef, unitCand, total));

		renderMixer(automation, mixCand, total, patterns[p], 6);
		CHECK(interleavedEqual(mixRef, mixCand, total * 2));
	}

	delete[] unitRef;
	delete[] unitCand;
	delete[] mixRef;
	delete[] mixCand;
}

// 2. Frame-exact gain at the unit level. The level changes exactly on the
//    event frame, by the exact gain ratio, in both channels (pan stays center).
static void testFrameExactGain()
{
	const unsigned int total = 512;
	const unsigned int eventFrame = 123;
	const float src = 0.5f;
	float *out = new float[total * CHANNELS];

	SoLoud::Wav wav;
	makeConstantSource(wav, src, 31, true);
	SoLoud::Automation automation;
	automation.setSource(&wav);
	SoLoud::AutomationEvent events[2];
	events[0] = { 0,          SoLoud::Automation::GAIN, 1.0f };
	events[1] = { eventFrame, SoLoud::Automation::GAIN, 0.25f };
	CHECK_RES(automation.setEvents(events, 2));

	// Tiny chunks force the event to land mid-buffer; framing must stay exact.
	const unsigned int pattern[] = { 7, 3, 11, 5 };
	renderUnit(automation, out, total, pattern, 4);

	float lc, rc;
	panGains(0.0f, lc, rc); // centered: both at cos(pi/4)
	const float beforeL = src * 1.0f * lc;
	const float beforeR = src * 1.0f * rc;
	const float afterL = src * 0.25f * lc;
	const float afterR = src * 0.25f * rc;

	// Exact boundary frame.
	CHECK(closeEnough(out[eventFrame - 1], beforeL, EPS));
	CHECK(closeEnough(out[total + eventFrame - 1], beforeR, EPS));
	CHECK(closeEnough(out[eventFrame], afterL, EPS));
	CHECK(closeEnough(out[total + eventFrame], afterR, EPS));

	for (unsigned int f = 0; f < total; f++)
	{
		float wantL = (f < eventFrame) ? beforeL : afterL;
		float wantR = (f < eventFrame) ? beforeR : afterR;
		CHECK(closeEnough(out[f], wantL, EPS));
		CHECK(closeEnough(out[total + f], wantR, EPS));
	}

	delete[] out;
}

// 3. Frame-exact pan at the unit level: full-left silences the right channel
//    exactly on the event frame while the left carries the full signal; center
//    restores both channels.
static void testFrameExactPan()
{
	const unsigned int total = 512;
	const unsigned int leftFrame = 100;
	const unsigned int centerFrame = 250;
	const float src = 0.4f;
	float *out = new float[total * CHANNELS];

	SoLoud::Wav wav;
	makeConstantSource(wav, src, 19, true);
	SoLoud::Automation automation;
	automation.setSource(&wav);
	SoLoud::AutomationEvent events[3];
	events[0] = { 0,           SoLoud::Automation::PAN, 0.0f };
	events[1] = { leftFrame,   SoLoud::Automation::PAN, -1.0f };
	events[2] = { centerFrame, SoLoud::Automation::PAN, 0.0f };
	CHECK_RES(automation.setEvents(events, 3));

	const unsigned int pattern[] = { 13, 1, 17, 2 };
	renderUnit(automation, out, total, pattern, 4);

	float lc, rc, ll, rl;
	panGains(0.0f, lc, rc);
	panGains(-1.0f, ll, rl); // full left: ll = 1, rl = 0
	CHECK(closeEnough(ll, 1.0f, EPS) && closeEnough(rl, 0.0f, EPS));

	// Exact transition frames.
	CHECK(closeEnough(out[leftFrame - 1], src * lc, EPS));
	CHECK(closeEnough(out[total + leftFrame - 1], src * rc, EPS));
	CHECK(closeEnough(out[leftFrame], src * ll, EPS));
	CHECK(closeEnough(out[total + leftFrame], 0.0f, EPS)); // right exactly silent
	CHECK(closeEnough(out[centerFrame], src * lc, EPS));
	CHECK(closeEnough(out[total + centerFrame], src * rc, EPS));

	for (unsigned int f = 0; f < total; f++)
	{
		if (f < leftFrame || f >= centerFrame)
		{
			CHECK(closeEnough(out[f], src * lc, EPS));
			CHECK(closeEnough(out[total + f], src * rc, EPS));
		}
		else
		{
			CHECK(closeEnough(out[f], src, EPS));
			CHECK(closeEnough(out[total + f], 0.0f, EPS));
		}
	}

	delete[] out;
}

// 4. Same-frame last-write-wins; gain and pan on the shared frame both take
//    effect on that exact frame.
static void testSameFrameLastWriteWins()
{
	const unsigned int total = 300;
	const unsigned int frame = 96;
	const float src = 0.5f;
	float *out = new float[total * CHANNELS];

	SoLoud::Wav wav;
	makeConstantSource(wav, src, 17, true);
	SoLoud::Automation automation;
	automation.setSource(&wav);

	automation.beginTransaction();
	// Three gain events on the same frame out of value order; last one wins.
	CHECK_RES(automation.submitEvent(frame, SoLoud::Automation::GAIN, 0.10f));
	CHECK_RES(automation.submitEvent(frame, SoLoud::Automation::GAIN, 0.60f));
	CHECK_RES(automation.submitEvent(frame, SoLoud::Automation::GAIN, 0.30f)); // winner
	// Two pan events on the same frame; the right-pan is submitted last.
	CHECK_RES(automation.submitEvent(frame, SoLoud::Automation::PAN, -1.0f));
	CHECK_RES(automation.submitEvent(frame, SoLoud::Automation::PAN, 1.0f)); // winner
	CHECK_RES(automation.commitTransaction());

	// Coalescing leaves exactly one gain and one pan event for the frame.
	CHECK(automation.getEventCount() == 2);
	SoLoud::AutomationEvent readback;
	for (unsigned int i = 0; i < automation.getEventCount(); i++)
	{
		CHECK_RES(automation.getEvent(i, readback));
		CHECK(readback.mFrame == frame);
		if (readback.mType == SoLoud::Automation::GAIN)
			CHECK(fabsf(readback.mValue - 0.30f) < 1e-7f);
		else
			CHECK(fabsf(readback.mValue - 1.0f) < 1e-7f);
	}

	const unsigned int pattern[] = { 5, 9, 2, 13 };
	renderUnit(automation, out, total, pattern, 4);

	float lc, rc;
	panGains(0.0f, lc, rc);
	// Before the frame: defaults (gain 1, pan center).
	CHECK(closeEnough(out[frame - 1], src * lc, EPS));
	CHECK(closeEnough(out[total + frame - 1], src * rc, EPS));
	// At the frame: gain 0.30 and full-right pan together; left exactly silent.
	CHECK(closeEnough(out[frame], 0.0f, EPS));
	CHECK(closeEnough(out[total + frame], src * 0.30f * 1.0f, EPS));

	for (unsigned int f = frame; f < total; f++)
	{
		CHECK(closeEnough(out[f], 0.0f, EPS));
		CHECK(closeEnough(out[total + f], src * 0.30f, EPS));
	}

	delete[] out;
}

// 5. Transaction atomicity: bad input never changes the committed list, and a
//    failed commit keeps the transaction open for retry/rollback.
static void testTransactionAtomicity()
{
	SoLoud::Wav wav;
	makeConstantSource(wav, 0.5f, 16, true);
	SoLoud::Automation automation;
	automation.setSource(&wav);

	// Seed a known-good committed list.
	SoLoud::AutomationEvent good[2];
	good[0] = { 10, SoLoud::Automation::GAIN, 0.5f };
	good[1] = { 20, SoLoud::Automation::PAN, -0.5f };
	CHECK_RES(automation.setEvents(good, 2));
	CHECK(automation.getEventCount() == 2);

	// submitEvent rejects invalid input at the boundary: the bad event never
	// enters the staging area and the transaction stays open for correction.
	automation.beginTransaction();
	CHECK_RES_FAIL(automation.submitEvent(5, SoLoud::Automation::GAIN, -0.01f)); // negative gain
	CHECK_RES_FAIL(automation.submitEvent(5, SoLoud::Automation::GAIN, NAN));    // NaN gain
	CHECK_RES_FAIL(automation.submitEvent(5, SoLoud::Automation::PAN, 1.01f));   // pan too high
	CHECK_RES_FAIL(automation.submitEvent(5, SoLoud::Automation::PAN, -1.01f));  // pan too low
	CHECK_RES_FAIL(automation.submitEvent(5, 42, 0.5f));                         // unknown type
	// A valid event, then an invalid one that is refused. The rejected event is
	// simply absent, so committing publishes only what was accepted; to prove
	// the committed list is untouched until an explicit commit, roll back here.
	CHECK_RES(automation.submitEvent(5, SoLoud::Automation::GAIN, 0.9f));
	CHECK_RES_FAIL(automation.submitEvent(6, SoLoud::Automation::PAN, 2.0f));
	// Nothing has been published yet: the original list is still live.
	CHECK(automation.getEventCount() == 2);

	// Discard the open transaction; the committed list must be fully unchanged.
	automation.rollbackTransaction();
	CHECK(automation.getEventCount() == 2);
	SoLoud::AutomationEvent e;
	CHECK_RES(automation.getEvent(0, e));
	CHECK(e.mFrame == 10 && e.mType == SoLoud::Automation::GAIN && fabsf(e.mValue - 0.5f) < 1e-7f);
	CHECK_RES(automation.getEvent(1, e));
	CHECK(e.mFrame == 20 && e.mType == SoLoud::Automation::PAN && fabsf(e.mValue - (-0.5f)) < 1e-7f);
	CHECK_RES_FAIL(automation.getEvent(2, e));

	// rollback closed the transaction; submitting now must fail.
	CHECK_RES_FAIL(automation.submitEvent(7, SoLoud::Automation::GAIN, 1.0f));
	CHECK(automation.getEventCount() == 2); // committed list still intact

	// A fresh clean transaction commits and publishes.
	automation.beginTransaction();
	CHECK_RES(automation.submitEvent(7, SoLoud::Automation::GAIN, 1.0f));
	CHECK_RES(automation.commitTransaction());
	CHECK(automation.getEventCount() == 1);
	CHECK_RES(automation.getEvent(0, e));
	CHECK(e.mFrame == 7 && e.mType == SoLoud::Automation::GAIN);

	// Re-seed the original good list for the remaining checks.
	CHECK_RES(automation.setEvents(good, 2));
	CHECK(automation.getEventCount() == 2);

	// setEvents atomicity: one bad event in a batch means no change at all.
	SoLoud::AutomationEvent mixed[3];
	mixed[0] = { 1, SoLoud::Automation::GAIN, 0.1f };
	mixed[1] = { 2, SoLoud::Automation::PAN, 5.0f }; // invalid
	mixed[2] = { 3, SoLoud::Automation::GAIN, 0.2f };
	CHECK_RES_FAIL(automation.setEvents(mixed, 3));
	CHECK(automation.getEventCount() == 2);

	// Null pointer with non-zero count is rejected with no state change.
	CHECK_RES_FAIL(automation.setEvents(0, 4));
	CHECK(automation.getEventCount() == 2);

	// clearEvents empties everything.
	automation.clearEvents();
	CHECK(automation.getEventCount() == 0);
}

// 6. Replayability and snapshot isolation: two plays of the same Automation
//    give identical output, and a commit between plays cannot corrupt an
//    already-created instance's private event snapshot.
static void testReplayAndCommitDuringPlayback()
{
	const unsigned int total = 600;
	float *first = new float[total * CHANNELS];
	float *second = new float[total * CHANNELS];

	SoLoud::Wav wav;
	makeConstantSource(wav, 0.5f, 23, true);
	SoLoud::Automation automation;
	automation.setSource(&wav);
	SoLoud::AutomationEvent events[2];
	events[0] = { 50, SoLoud::Automation::GAIN, 0.4f };
	events[1] = { 80, SoLoud::Automation::PAN, 1.0f };
	CHECK_RES(automation.setEvents(events, 2));

	const unsigned int patternA[] = { 17, 29 };
	const unsigned int patternB[] = { 64, 8, 33 };
	renderUnit(automation, first, total, patternA, 2);

	// Re-commit a different list between plays. Instances own private event
	// copies, so this cannot disturb a replay of the first configuration.
	SoLoud::AutomationEvent other[1];
	other[0] = { 5, SoLoud::Automation::GAIN, 0.0f };
	CHECK_RES(automation.setEvents(other, 1));
	CHECK(automation.getEventCount() == 1);
	// Restore the original list and replay; output must match the first run.
	CHECK_RES(automation.setEvents(events, 2));
	renderUnit(automation, second, total, patternB, 3);
	CHECK(planarEqual(first, second, total));

	// Snapshot isolation: create an instance against the current list, commit a
	// new list, and confirm the long-lived instance keeps rendering the old one.
	SoLoud::AutomationInstance *inst = (SoLoud::AutomationInstance *)automation.createInstance();
	CHECK(inst != 0);
	float snap[UNIT_BUF * CHANNELS];
	memset(snap, 0, sizeof(snap));
	inst->getAudio(snap, UNIT_BUF, UNIT_BUF); // first 256 frames, old events
	CHECK_RES(automation.setEvents(other, 1)); // new list: gain 0 from frame 5
	// A fresh instance of the new list must differ from the long-lived one,
	// proving the old instance did not silently switch event lists.
	SoLoud::AutomationInstance *fresh = (SoLoud::AutomationInstance *)automation.createInstance();
	CHECK(fresh != 0);
	float newsnap[UNIT_BUF * CHANNELS];
	memset(newsnap, 0, sizeof(newsnap));
	fresh->getAudio(newsnap, UNIT_BUF, UNIT_BUF);
	bool differs = false;
	for (unsigned int i = 10; i < UNIT_BUF; i++)
	{
		if (snap[i] != newsnap[i] || snap[UNIT_BUF + i] != newsnap[UNIT_BUF + i])
			differs = true;
	}
	CHECK(differs);
	// The old instance still follows its own list after a rewind.
	float again[UNIT_BUF * CHANNELS];
	memset(again, 0, sizeof(again));
	inst->rewind();
	inst->getAudio(again, UNIT_BUF, UNIT_BUF);
	CHECK(planarEqual(snap, again, UNIT_BUF));
	delete inst;
	delete fresh;

	delete[] first;
	delete[] second;
}

// 7. A non-looping source ending mid-stream stays deterministic and silent
//    after the end, independent of buffer split points (mixer level).
static void testEndedSourceSilence()
{
	const unsigned int total = 300;
	const unsigned int srcFrames = 73;
	float *a = new float[total * 2];
	float *b = new float[total * 2];

	SoLoud::Wav wav;
	float *data = new float[srcFrames];
	for (unsigned int i = 0; i < srcFrames; i++)
		data[i] = 0.3f;
	wav.loadRawWave(data, srcFrames, (float)SAMPLERATE, 1, true, true); // not looping

	SoLoud::Automation automation;
	automation.setSource(&wav);
	SoLoud::AutomationEvent ev[1];
	ev[0] = { 10, SoLoud::Automation::GAIN, 0.7f };
	CHECK_RES(automation.setEvents(ev, 1));

	const unsigned int one[] = { total };
	const unsigned int pattern[] = { 9, 5, 13 };
	renderMixer(automation, a, total, one, 1);
	renderMixer(automation, b, total, pattern, 3);
	CHECK(interleavedEqual(a, b, total * 2));

	// After the source ends the output is exactly silent (the passthrough path
	// zero-fills the tail, and the mixer then auto-stops the voice).
	unsigned int nonzeroAfterEnd = 0;
	for (unsigned int f = srcFrames; f < total; f++)
	{
		if (a[f * 2 + 0] != 0.0f || a[f * 2 + 1] != 0.0f)
			nonzeroAfterEnd++;
	}
	CHECK(nonzeroAfterEnd == 0);

	delete[] a;
	delete[] b;
}

// 8. Large-block requests (beyond the initial SAMPLE_GRANULARITY pull buffer)
//    and a multi-channel looping source. This guards the on-demand buffer
//    growth and the planar handling of loop continuation: the two source
//    channels must never be swapped or lost when a pull wraps around the loop.
static void testLargeBlockAndStereoLoop()
{
	const unsigned int total = 2048; // 4x the 512-frame initial scratch
	const unsigned int srcFrames = 64;
	const float leftVal = 0.30f;
	const float rightVal = 0.60f;

	SoLoud::Wav wav;
	// loadRawWave stores planar data directly (no de-interleaving): the first
	// srcFrames floats are the left plane, the next srcFrames the right plane.
	float *data = new float[srcFrames * 2];
	for (unsigned int i = 0; i < srcFrames; i++)
		data[i] = leftVal;
	for (unsigned int i = 0; i < srcFrames; i++)
		data[srcFrames + i] = rightVal;
	wav.loadRawWave(data, srcFrames * 2, (float)SAMPLERATE, 2, true, true);
	wav.setLooping(true);

	SoLoud::Automation automation;
	automation.setSource(&wav);
	SoLoud::AutomationEvent ev[1];
	ev[0] = { 0, SoLoud::Automation::GAIN, 1.0f };
	CHECK_RES(automation.setEvents(ev, 1));

	// One huge unit-level pull (forces scratch growth) and many tiny pulls must
	// produce identical planar output.
	float *big = new float[total * CHANNELS];
	float *small = new float[total * CHANNELS];
	const unsigned int one[] = { total };
	const unsigned int pattern[] = { 5, 13, 7, 3 };
	renderUnit(automation, big, total, one, 1);
	renderUnit(automation, small, total, pattern, 4);
	CHECK(planarEqual(big, small, total));

	// The wrapped stereo source is down-mixed to mono (average of L and R),
	// then panned center. Verify the exact level on every frame, including the
	// many loop wrap points, so a broken planar loop would show up immediately.
	float lc, rc;
	panGains(0.0f, lc, rc);
	const float mono = (leftVal + rightVal) * 0.5f;
	for (unsigned int f = 0; f < total; f++)
	{
		CHECK(closeEnough(big[f], mono * lc, EPS));
		CHECK(closeEnough(big[total + f], mono * rc, EPS));
	}

	delete[] big;
	delete[] small;
}

int main(int /*argc*/, char ** /*argv*/)
{
	printf("Running SoLoud Automation regression tests...\n");

	testBufferSplitInvariance();
	testFrameExactGain();
	testFrameExactPan();
	testSameFrameLastWriteWins();
	testTransactionAtomicity();
	testReplayAndCommitDuringPlayback();
	testEndedSourceSilence();
	testLargeBlockAndStereoLoop();

	printf("\n%d checks, %d failure(s)\n", gTests, gErrors);
	return gErrors ? 1 : 0;
}
