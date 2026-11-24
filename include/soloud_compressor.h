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

#ifndef SOLOUD_COMPRESSOR_H
#define SOLOUD_COMPRESSOR_H

#include "soloud.h"

namespace SoLoud
{
	class Compressor;

	// Audio source instance that applies dynamic range compression
	// to another audio source. All channels share a single gain, so
	// the stereo image is preserved.
	class CompressorInstance : public AudioSourceInstance
	{
	public:
		CompressorInstance(Compressor *aParent);
		virtual ~CompressorInstance();
		virtual unsigned int getAudio(float *aBuffer, unsigned int aSamplesToRead, unsigned int aBufferSize);
		virtual bool hasEnded();
		virtual result rewind();

	private:
		// Recalculate the attack/release smoothing coefficients for the current samplerate
		void updateCoeffs();
		// Instance-local configuration snapshot; reconfiguring the source does not
		// disturb an already playing instance.
		float mThreshold;
		float mRatio;
		float mAttack;
		float mRelease;
		float mMakeupGain;
		// Attack/release one-pole coefficients
		float mAttackCoeff;
		float mReleaseCoeff;
		float mSamplerateCache;
		// Envelope follower state (linear peak level)
		float mEnvelope;
		// The wrapped source instance; owned and deleted by this instance
		AudioSourceInstance *mSource;
		// Scratch buffer for pulling the wrapped source (planar, MAX_CHANNELS)
		AlignedFloatBuffer mScratch;
		unsigned int mScratchSize;
	};

	// Dynamic range compressor audio source.
	// Wraps another AudioSource so it can be played through the normal
	// Soloud play chain. Signals below the threshold pass through untouched
	// (apart from makeup gain); signals above it are reduced according to
	// the ratio, with separate attack/release ballistics so that sudden
	// loud transients only reduce gain for their own duration instead of
	// flattening the whole stream. A single linked gain is applied to
	// every channel.
	class Compressor : public AudioSource
	{
	public:
		Compressor();
		virtual ~Compressor();

		// Wrap an audio source. The source must remain alive for as long as
		// the compressor plays; the compressor does not take ownership.
		// Passing NULL (or an invalid source) returns INVALID_PARAMETER and
		// leaves the previously configured source untouched.
		result setSource(AudioSource *aSource);

		// (Re)configure the compressor. All parameters are validated first;
		// if any of them is invalid nothing is changed, both on the source
		// and on the snapshots already taken by playing instances.
		// aThreshold   level above which compression starts, in 0..1 (e.g. 0.5)
		// aRatio       input:output ratio above threshold, >= 1 (e.g. 4.0)
		// aAttack      seconds for gain reduction to reach 63% of target, >= 0
		// aRelease     seconds for gain to recover 63%, > 0
		// aMakeupGain  linear gain applied after compression, >= 0 (1.0 = off)
		result setParams(float aThreshold, float aRatio, float aAttack = 0.01f,
		                 float aRelease = 0.2f, float aMakeupGain = 1.0f);

		virtual AudioSourceInstance *createInstance();

		AudioSource *mSource;
		float mThreshold;
		float mRatio;
		float mAttack;
		float mRelease;
		float mMakeupGain;
	};
};

#endif
