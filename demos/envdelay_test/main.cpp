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

// Regression tests for the EnvDelay feedback environment delay source.
//
// Two layers are exercised:
//   1. EnvDelayInstance directly (frame-exact DSP checks, no engine
//      resampler or volume ramps in the way).
//   2. The full engine through the null driver (tail survival, voice
//      lifecycle, stereo routing).
// Returns non-zero on failure.

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "soloud.h"
#include "soloud_audiosource.h"
#include "soloud_envdelay.h"
#include "soloud_wav.h"

namespace
{
	int gFailures = 0;
	int gChecks = 0;

	void check(bool aCondition, const char *aMessage)
	{
		gChecks++;
		if (!aCondition)
		{
			gFailures++;
			printf("FAIL: %s\n", aMessage);
		}
	}

	bool closeEnough(float a, float b, float aEpsilon = 0.0001f)
	{
		return fabsf(a - b) <= aEpsilon;
	}

	// Deterministic test material: aMaterialFrames long, with the first
	// aPulseFrames frames at aAmplitude and the rest silent. The instance
	// reports hasEnded() once the material is fully consumed, exactly like
	// a finite Wav source, so EnvDelay has to keep the tail alive itself.
	class PulseSource;

	class PulseInstance : public SoLoud::AudioSourceInstance
	{
	public:
		PulseInstance(PulseSource *aParent);
		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
		virtual bool hasEnded();
		virtual SoLoud::result rewind();

	private:
		PulseSource *mParent;
		unsigned int mOffset;
	};

	class PulseSource : public SoLoud::AudioSource
	{
	public:
		unsigned int mMaterialFrames;
		unsigned int mPulseFrames;
		float mAmplitude;

		PulseSource(unsigned int aMaterialFrames, unsigned int aPulseFrames, float aAmplitude, unsigned int aChannels = 1)
		{
			mMaterialFrames = aMaterialFrames;
			mPulseFrames = aPulseFrames;
			mAmplitude = aAmplitude;
			mBaseSamplerate = 8000; // tiny rate keeps the test blocks frame-accurate
			mChannels = aChannels;
		}

		virtual SoLoud::AudioSourceInstance *createInstance()
		{
			return new PulseInstance(this);
		}
	};

	PulseInstance::PulseInstance(PulseSource *aParent)
	{
		mParent = aParent;
		mOffset = 0;
	}

	unsigned int PulseInstance::getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		unsigned int copylen = aSamplesToRead;
		if (mOffset + copylen > mParent->mMaterialFrames)
			copylen = mParent->mMaterialFrames - mOffset;

		for (unsigned int ch = 0; ch < mChannels; ch++)
		{
			float *out = aBuffer + ch * aBufferSize;
			for (unsigned int i = 0; i < aSamplesToRead; i++)
			{
				unsigned int frame = mOffset + i;
				out[i] = (frame < mParent->mPulseFrames) ? mParent->mAmplitude : 0.0f;
			}
		}
		mOffset += copylen;
		return copylen;
	}

	bool PulseInstance::hasEnded()
	{
		return mOffset >= mParent->mMaterialFrames;
	}

	SoLoud::result PulseInstance::rewind()
	{
		mOffset = 0;
		return SoLoud::SO_NO_ERROR;
	}

	// Pull one frame at a time directly from an EnvDelayInstance, bypassing
	// the engine entirely.
	float pullFrame(SoLoud::EnvDelayInstance &aInstance)
	{
		float buf[SAMPLE_GRANULARITY];
		aInstance.getAudio(buf, 1, SAMPLE_GRANULARITY);
		return buf[0];
	}
}

// Representative sample, verified frame-by-frame at the DSP level:
// 5 frame material (single-frame pulse then silence), delay 2 frames,
// feedback 0.5, wet 0.5. Frame 2 must carry a 0.5 echo and frame 4 a 0.25
// echo, with the dry signal and every reflection continuous, and the tail
// must keep ringing after the 5 frame material ends.
static void testRepresentativeSample()
{
	PulseSource pulse(5, 1, 1.0f);
	SoLoud::EnvDelay env;
	SoLoud::result r = env.setParams(pulse, 2, 0.5f, 0.5f);
	check(r == SoLoud::SO_NO_ERROR, "setParams representative sample");

	SoLoud::EnvDelayInstance *inst = (SoLoud::EnvDelayInstance *)env.createInstance();
	check(inst != 0, "instance created");

	// Dry level is 1 - wet = 0.5; echoes are the geometric series
	// wet * feedback^k = 0.5, 0.25, 0.125, ... every 2 frames
	check(closeEnough(pullFrame(*inst), 0.5f), "frame 0: dry 0.5, no echo yet");
	check(closeEnough(pullFrame(*inst), 0.0f), "frame 1: silence");
	check(closeEnough(pullFrame(*inst), 0.5f), "frame 2: first echo 0.5");
	check(closeEnough(pullFrame(*inst), 0.0f), "frame 3: silence between taps");
	check(closeEnough(pullFrame(*inst), 0.25f), "frame 4: feedback echo 0.25");

	// The 5 frame material ends here, but the tail must continue seamlessly
	check(closeEnough(pullFrame(*inst), 0.0f), "frame 5: silence between taps");
	check(closeEnough(pullFrame(*inst), 0.125f), "frame 6: tail echo 0.125 after material ended");
	check(closeEnough(pullFrame(*inst), 0.0f), "frame 7: silence between taps");
	check(closeEnough(pullFrame(*inst), 0.0625f), "frame 8: tail echo 0.0625");
	check(closeEnough(pullFrame(*inst), 0.0f), "frame 9: silence between taps");
	check(closeEnough(pullFrame(*inst), 0.03125f), "frame 10: tail echo 0.03125");

	// The instance must report itself alive while the tail rings
	check(!inst->hasEnded(), "instance alive while tail is audible");

	delete inst;
}

// Wet-only and dry-only mix configurations at the DSP level.
static void testMixLevels()
{
	// Wet only: no dry leak, first reflection at frame 2.
	{
		PulseSource pulse(1, 1, 1.0f);
		SoLoud::EnvDelay env;
		check(env.setParams(pulse, 2, 0.5f, 1.0f) == SoLoud::SO_NO_ERROR, "wet-only config");
		SoLoud::EnvDelayInstance *inst = (SoLoud::EnvDelayInstance *)env.createInstance();
		check(closeEnough(pullFrame(*inst), 0.0f), "wet-only: silence at frame 0");
		check(closeEnough(pullFrame(*inst), 0.0f), "wet-only: silence at frame 1");
		check(closeEnough(pullFrame(*inst), 1.0f), "wet-only: first echo 1.0 at frame 2");
		check(closeEnough(pullFrame(*inst), 0.0f), "wet-only: silence at frame 3");
		check(closeEnough(pullFrame(*inst), 0.5f), "wet-only: feedback echo 0.5 at frame 4");
		check(closeEnough(pullFrame(*inst), 0.0f), "wet-only: silence at frame 5");
		check(closeEnough(pullFrame(*inst), 0.25f), "wet-only: feedback echo 0.25 at frame 6");
		delete inst;
	}

	// Dry only: the pulse passes through untouched, no echoes.
	{
		PulseSource pulse(1, 1, 1.0f);
		SoLoud::EnvDelay env;
		check(env.setParams(pulse, 2, 0.5f, 0.0f) == SoLoud::SO_NO_ERROR, "dry-only config");
		SoLoud::EnvDelayInstance *inst = (SoLoud::EnvDelayInstance *)env.createInstance();
		check(closeEnough(pullFrame(*inst), 1.0f), "dry-only: pulse at frame 0");
		check(closeEnough(pullFrame(*inst), 0.0f), "dry-only: silence at frame 1");
		check(closeEnough(pullFrame(*inst), 0.0f), "dry-only: no echo at frame 2");
		check(closeEnough(pullFrame(*inst), 0.0f), "dry-only: no echo at frame 4");
		delete inst;
	}
}

// Stereo material at the DSP level: independent planar delay lines.
static void testStereoDSP()
{
	PulseSource pulse(5, 1, 1.0f, 2);
	SoLoud::EnvDelay env;
	check(env.setParams(pulse, 2, 0.5f, 0.5f) == SoLoud::SO_NO_ERROR, "stereo config accepted");
	SoLoud::EnvDelayInstance *inst = (SoLoud::EnvDelayInstance *)env.createInstance();

	float buf[SAMPLE_GRANULARITY * 2];
	inst->getAudio(buf, 3, SAMPLE_GRANULARITY);
	// Frame 0: dry 0.5 on both channels
	check(closeEnough(buf[0], 0.5f) && closeEnough(buf[SAMPLE_GRANULARITY], 0.5f), "stereo frame 0 dry both channels");
	// Frame 2: echo 0.5 (no dry, the pulse was only one frame) on both channels
	check(closeEnough(buf[2], 0.5f) && closeEnough(buf[SAMPLE_GRANULARITY + 2], 0.5f), "stereo frame 2 echo both channels");
	delete inst;
}

// Invalid delay lengths (and other bad params) must be rejected at
// configuration time, before playback.
static void testInvalidParametersRejected()
{
	PulseSource pulse(5, 1, 1.0f);
	SoLoud::EnvDelay env;

	check(env.setParams(pulse, 0, 0.5f, 0.5f) == SoLoud::INVALID_PARAMETER, "zero delay rejected");
	check(env.setParams(pulse, 10000001, 0.5f, 0.5f) == SoLoud::INVALID_PARAMETER, "huge delay rejected");
	check(env.setParams(pulse, 2, -0.1f, 0.5f) == SoLoud::INVALID_PARAMETER, "negative feedback rejected");
	check(env.setParams(pulse, 2, 1.0f, 0.5f) == SoLoud::INVALID_PARAMETER, "feedback >= 1 rejected");
	check(env.setParams(pulse, 2, 0.5f, -0.1f) == SoLoud::INVALID_PARAMETER, "negative wet rejected");
	check(env.setParams(pulse, 2, 0.5f, 1.1f) == SoLoud::INVALID_PARAMETER, "wet > 1 rejected");

	// An unconfigured EnvDelay must not produce an instance
	check(env.createInstance() == 0, "unconfigured EnvDelay creates no instance");

	// A valid configuration after rejected ones still works
	check(env.setParams(pulse, 2, 0.5f, 0.5f) == SoLoud::SO_NO_ERROR, "valid config after rejected ones");
	SoLoud::AudioSourceInstance *instance = env.createInstance();
	check(instance != 0, "configured EnvDelay creates an instance");
	delete instance;
}

// Engine-level check: the feedback tail must survive the wrapped source
// ending (no truncation when the "camera cuts away"), and the voice must
// eventually stop by itself once the tail decays.
static void testTailSurvivesSourceEnd()
{
	SoLoud::Soloud soloud;
	SoLoud::result r = soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, 8000, 512, 2);
	check(r == SoLoud::SO_NO_ERROR, "engine init for tail test");
	soloud.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);

	// 5 frame material; the wrapped source reports hasEnded() at frame 5
	PulseSource pulse(5, 1, 1.0f);
	SoLoud::EnvDelay env;
	env.setParams(pulse, 2, 0.5f, 0.5f);
	soloud.play(env);

	const unsigned int maxFrames = 4096;
	unsigned int frame = 0;
	bool sawTailAfterSource = false;
	unsigned int lastAudibleFrame = 0;

	while (soloud.getActiveVoiceCount() > 0 && frame < maxFrames)
	{
		float buf[512];
		soloud.mix(buf, 256);
		for (unsigned int i = 0; i < 256; i++, frame++)
		{
			float v = buf[i * 2];
			// The wrapped material ends at frame 5; anything audible later
			// is the EnvDelay tail, not the source itself
			if (frame > 5 && fabsf(v) > 0.001f)
			{
				sawTailAfterSource = true;
				lastAudibleFrame = frame;
			}
			if (frame >= maxFrames)
				break;
		}
	}

	check(sawTailAfterSource, "tail keeps ringing after source material ends");
	check(frame < maxFrames, "voice ends by itself after tail decays");
	check(lastAudibleFrame > 5, "last audible sample is part of the tail");
	soloud.deinit();
}

// Engine-level stereo routing: both output channels stay live through the
// whole dry + echo sequence.
static void testStereoEngine()
{
	SoLoud::Soloud soloud;
	soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, 8000, 512, 2);
	soloud.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);

	PulseSource pulse(5, 1, 1.0f, 2);
	SoLoud::EnvDelay env;
	check(env.setParams(pulse, 2, 0.5f, 0.5f) == SoLoud::SO_NO_ERROR, "stereo engine config");
	soloud.play(env);

	float buf[512];
	soloud.mix(buf, 8);
	bool leftAudible = false, rightAudible = false;
	float leftAt0 = 0.0f, rightAt0 = 0.0f;
	float leftAt2 = 0.0f, rightAt2 = 0.0f;
	for (unsigned int i = 0; i < 8; i++)
	{
		if (fabsf(buf[i * 2 + 0]) > 0.1f)
			leftAudible = true;
		if (fabsf(buf[i * 2 + 1]) > 0.1f)
			rightAudible = true;
		if (i == 0)
		{
			leftAt0 = buf[i * 2 + 0];
			rightAt0 = buf[i * 2 + 1];
		}
		if (i == 2)
		{
			leftAt2 = buf[i * 2 + 0];
			rightAt2 = buf[i * 2 + 1];
		}
	}
	check(leftAudible && rightAudible, "stereo engine: both channels audible");
	// The engine ramps channel volume up over the first buffer, so compare
	// the frame 2 echo against the frame 0 dry level of the same channel:
	// wet/dry = 0.5/0.5 = 1.0, i.e. the echo equals the dry reference.
	check(closeEnough(leftAt2, leftAt0, 0.02f) && closeEnough(rightAt2, rightAt0, 0.02f), "stereo engine: frame 2 echo matches the dry level on both channels");
	soloud.deinit();
}

// End-to-end with a real Wav source: the wrapped Wav ends after its data,
// but EnvDelay must keep the voice alive with a continuous feedback tail.
static void testWavSourceEndToEnd()
{
	SoLoud::Soloud soloud;
	soloud.init(SoLoud::Soloud::CLIP_ROUNDOFF, SoLoud::Soloud::NULLDRIVER, 8000, 512, 2);
	soloud.setMainResampler(SoLoud::Soloud::RESAMPLER_POINT);

	// 5 frame Wav: one unit sample then four silent frames
	float data[5] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	SoLoud::Wav wav;
	check(wav.loadRawWave(data, 5, 8000, 1, true, false) == SoLoud::SO_NO_ERROR, "wav loadRawWave");

	SoLoud::EnvDelay env;
	check(env.setParams(wav, 2, 0.5f, 0.5f) == SoLoud::SO_NO_ERROR, "envdelay wraps a real Wav");
	soloud.play(env);

	// Collect frames beyond the 5 frame material; the tail (frames 6, 8, ...)
	// must be present even though the Wav itself has ended.
	const unsigned int N = 24;
	float out[N];
	unsigned int done = 0;
	while (done < N)
	{
		float buf[64];
		unsigned int n = N - done;
		if (n > 32)
			n = 32;
		soloud.mix(buf, n);
		for (unsigned int i = 0; i < n; i++)
			out[done + i] = buf[i * 2];
		done += n;
	}

	// Frames 2/4/6 carry echoes; frame 6 is strictly after the Wav ended.
	check(out[2] > 0.1f, "wav e2e: echo at frame 2");
	check(out[4] > 0.05f, "wav e2e: feedback echo at frame 4");
	check(out[6] > 0.02f, "wav e2e: tail echo at frame 6 after Wav ended");
	check(out[8] > 0.005f, "wav e2e: decaying tail at frame 8");
	// Successive taps decay (feedback 0.5), proving the loop runs
	check(out[4] < out[2] && out[6] < out[4] && out[8] < out[6], "wav e2e: taps decay geometrically");
	soloud.deinit();
}

// A delay longer than the engine's SAMPLE_GRANULARITY block must still
// produce frame-exact, continuous echoes across block boundaries.
static void testDelayLargerThanBlock()
{
	PulseSource pulse(3, 1, 1.0f);
	SoLoud::EnvDelay env;
	const unsigned int delay = SAMPLE_GRANULARITY + 88; // spans > one block
	check(env.setParams(pulse, delay, 0.5f, 0.5f) == SoLoud::SO_NO_ERROR, "long delay config accepted");
	SoLoud::EnvDelayInstance *inst = (SoLoud::EnvDelayInstance *)env.createInstance();

	int firstEcho = -1;
	int secondEcho = -1;
	for (unsigned int f = 0; f < delay * 3; f++)
	{
		float v = pullFrame(*inst);
		if (firstEcho < 0 && f > 0 && closeEnough(v, 0.5f))
			firstEcho = (int)f;
		else if (firstEcho >= 0 && secondEcho < 0 && closeEnough(v, 0.25f))
			secondEcho = (int)f;
		if (inst->hasEnded())
			break;
	}
	check(firstEcho == (int)delay, "first echo lands exactly one delay later across blocks");
	check(secondEcho == (int)(delay * 2), "second echo lands exactly two delays later across blocks");
	delete inst;
}

int main()
{
	testRepresentativeSample();
	testMixLevels();
	testStereoDSP();
	testInvalidParametersRejected();
	testTailSurvivesSourceEnd();
	testStereoEngine();
	testWavSourceEndToEnd();
	testDelayLargerThanBlock();

	printf("%d checks, %d failures\n", gChecks, gFailures);
	return gFailures == 0 ? 0 : 1;
}
