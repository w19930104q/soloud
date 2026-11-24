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

// Limiter source test: feeds the representative sample pattern
// (a 1.0 peak followed by 0.5 and 0.25, plus low-level material)
// through a 0.8-ceiling Limiter and verifies that
//  - low-level content passes through untouched,
//  - the 1.0 peak is held exactly at the 0.8 ceiling (lookahead means
//    the spike is never let through and then slammed),
//  - the 0.5 and 0.25 after the peak are still attenuated by the
//    release window,
//  - both stereo channels share one gain (image stays locked),
//  - rewinding replays the exact same limited signal.

#include <stdio.h>
#include <math.h>
#include <string.h>

#include "soloud.h"
#include "soloud_limiter.h"

#define TEST_SR 44100
#define TEST_LEN 8192
#define PEAK_POS 1000

// Procedural stereo test source: 0.1 bed, 1.0/0.6 peak at PEAK_POS,
// then 0.5, 0.25, back to 0.1, silence at the tail.
class TestSourceInstance : public SoLoud::AudioSourceInstance
{
	unsigned int mOffset;
public:
	TestSourceInstance()
	{
		mOffset = 0;
	}

	static void gen(unsigned int aIndex, float &aL, float &aR)
	{
		aL = 0.1f;
		aR = 0.1f;
		if (aIndex == PEAK_POS) { aL = 1.0f; aR = 0.6f; }
		else if (aIndex == PEAK_POS + 1) { aL = 0.5f; aR = -0.5f; }
		else if (aIndex == PEAK_POS + 2) { aL = 0.25f; aR = 0.25f; }
		if (aIndex >= 6000) { aL = 0.0f; aR = 0.0f; }
	}

	virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize)
	{
		unsigned int count = aSamplesToRead;
		if (mOffset + count > TEST_LEN)
			count = TEST_LEN - mOffset;
		unsigned int i;
		for (i = 0; i < count; i++)
		{
			float l, r;
			gen(mOffset + i, l, r);
			aBuffer[i] = l;
			aBuffer[aBufferSize + i] = r;
		}
		mOffset += count;
		return count;
	}

	virtual bool hasEnded()
	{
		return mOffset >= TEST_LEN;
	}

	virtual SoLoud::result rewind()
	{
		mOffset = 0;
		mStreamPosition = 0.0f;
		return SoLoud::SO_NO_ERROR;
	}
};

class TestSource : public SoLoud::AudioSource
{
public:
	TestSource()
	{
		mChannels = 2;
		mBaseSamplerate = TEST_SR;
	}
	virtual SoLoud::AudioSourceInstance *createInstance()
	{
		return new TestSourceInstance();
	}
};

static int gFailures = 0;

static void check(int aCond, const char *aWhat)
{
	if (aCond)
	{
		printf("PASS: %s\n", aWhat);
	}
	else
	{
		printf("FAIL: %s\n", aWhat);
		gFailures++;
	}
}

static unsigned int pullAll(SoLoud::AudioSourceInstance *aInst, float *aOut, unsigned int aMax)
{
	unsigned int total = 0;
	while (total < aMax)
	{
		unsigned int got = aInst->getAudio(aOut + total, aMax - total, aMax);
		if (got == 0)
			break;
		total += got;
	}
	return total;
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;

	TestSource src;
	SoLoud::Limiter limiter;
	limiter.setSource(&src);
	limiter.setParams(0.8f, 0.005f, 0.1f);

	SoLoud::AudioSourceInstance *inst = limiter.createInstance();
	if (inst == 0)
	{
		printf("FAIL: could not create limiter instance\n");
		return 1;
	}

	float *out = new float[TEST_LEN * 2];
	float *outL = out;
	float *outR = out + TEST_LEN;
	unsigned int total = pullAll(inst, out, TEST_LEN);

	check(total == TEST_LEN, "limiter outputs the full stream (lookahead tail is flushed)");
	check(inst->hasEnded(), "stream has ended after draining");

	// 1. Low-level content far from the peak is untouched.
	check(outL[500] == 0.1f && outR[500] == 0.1f, "low level before the peak is untouched");
	check(outL[3000] == 0.1f && outR[3000] == 0.1f, "low level after the release window is untouched");

	// 2. The 1.0 peak is held exactly at the 0.8 ceiling.
	check(fabsf(outL[PEAK_POS] - 0.8f) < 1e-6f, "1.0 peak is held at the 0.8 ceiling");

	// 3. No sample anywhere exceeds the ceiling.
	float peak = 0;
	unsigned int i;
	for (i = 0; i < total; i++)
	{
		float v = fabsf(outL[i]);
		if (v > peak) peak = v;
		v = fabsf(outR[i]);
		if (v > peak) peak = v;
	}
	check(peak <= 0.8f + 1e-6f, "no output sample exceeds the ceiling");

	// 4. The 0.5 and 0.25 after the peak are still attenuated by the
	//    release window (gain ramps from 0.8 back to 1 over 0.1 s).
	float g1 = 0.8f + 1.0f / (0.1f * TEST_SR);
	float g2 = 0.8f + 2.0f / (0.1f * TEST_SR);
	check(fabsf(outL[PEAK_POS + 1] - 0.5f * g1) < 1e-4f, "0.5 after the peak is attenuated by the release window");
	check(fabsf(outL[PEAK_POS + 2] - 0.25f * g2) < 1e-4f, "0.25 after the peak is attenuated by the release window");
	check(outL[PEAK_POS + 1] < 0.5f && outL[PEAK_POS + 2] < 0.25f, "post-peak samples are below their input level");

	// 5. Stereo channels share one gain: L/R ratio is preserved
	//    everywhere, so the stereo image does not jump.
	int linked = 1;
	for (i = 0; i < total; i++)
	{
		float inL, inR;
		TestSourceInstance::gen(i, inL, inR);
		if (fabsf(inL) > 1e-6f && fabsf(inR) > 1e-6f)
		{
			float gL = outL[i] / inL;
			float gR = outR[i] / inR;
			if (fabsf(gL - gR) > 1e-5f)
				linked = 0;
		}
	}
	check(linked, "stereo channels share one gain (image locked)");
	check(fabsf(outR[PEAK_POS] - 0.6f * 0.8f) < 1e-6f, "right channel peak scaled by the same gain");
	check(fabsf(outR[PEAK_POS + 1] + 0.5f * g1) < 1e-4f, "negative right channel peak scaled by the same gain");

	// 6. Rewind and replay: identical output the second time.
	SoLoud::result res = inst->rewind();
	check(res == SoLoud::SO_NO_ERROR, "limiter supports rewind");
	float *out2 = new float[TEST_LEN * 2];
	unsigned int total2 = pullAll(inst, out2, TEST_LEN);
	check(total2 == total, "rewound stream has the same length");
	check(memcmp(out, out2, total * 2 * sizeof(float)) == 0, "rewound stream replays identically");

	// 7. Seek backwards works through the generic tape seek.
	res = inst->seek(0.0, out2, TEST_LEN);
	check(res == SoLoud::SO_NO_ERROR, "limiter supports seek (via rewind)");
	float one[2 * 16];
	unsigned int got = inst->getAudio(one, 16, 16);
	check(got == 16 && fabsf(one[0] - outL[0]) < 1e-6f, "seek to 0 restarts the limited stream");

	delete[] out2;
	delete[] out;
	delete inst;

	if (gFailures)
	{
		printf("%d check(s) FAILED\n", gFailures);
		return 1;
	}
	printf("All limiter checks passed\n");
	return 0;
}
