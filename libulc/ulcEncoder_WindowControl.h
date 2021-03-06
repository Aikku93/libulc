/**************************************/
//! ulc-codec: Ultra-Low-Complexity Audio Codec
//! Copyright (C) 2021, Ruben Nunez (Aikku; aik AT aol DOT com DOT au)
//! Refer to the project README file for license terms.
/**************************************/
#pragma once
/**************************************/
#include <math.h>
/**************************************/
#include "ulcEncoder.h"
#include "ulcHelper.h"
/**************************************/

//! Get optimal log base-2 overlap and window scalings for transients
//! The idea is that if a transient is relatively centered with the
//! transition region of a subblock, then we can just set the overlap
//! amount to account for it and avoid reducing the window size too
//! much, preserving the quality gains of a larger transform. At the
//! same time, we also need to /make/ the transient sit within a
//! transition region to take advantage of this, and so we combine
//! the overlap scaling and window-switching strategies.
//! NOTE: StepBuffer must be at least BlockSize/2 in size.
//! NOTE: Bit codes for transient region coding, and their window sizes:
//!  First nybble:
//!   0xxx: No decimation. xxx = Overlap scaling
//!   1xxx: Decimate. xxx = Overlap scaling for the transient subblock
//!  Second nybble (when first nybble is 1xxx; otherwise, this is implicitly 0001):
//!   1xxx: Decimation by 1/8: Position = 0~7
//!    1000: N/8*,N/8,N/4,N/2
//!    1001: N/8,N/8*,N/4,N/2
//!    1010: N/4,N/8*,N/8,N/2
//!    1011: N/4,N/8,N/8*,N/2
//!    1100: N/2,N/8*,N/8,N/4
//!    1101: N/2,N/8,N/8*,N/4
//!    1110: N/2,N/4,N/8*,N/8
//!    1111: N/2,N/4,N/8,N/8*
//!   01xx: Decimation by 1/4: Position = 0~3
//!    0100: N/4*,N/4,N/2
//!    0101: N/4,N/4*,N/2
//!    0110: N/2,N/4*,N/4
//!    0111: N/2,N/4,N/4*
//!   001x: Decimation by 1/2: Position = 0~1
//!    0010: N/2*,N/2
//!    0011: N/2,N/2*
//!   0001: No decimation (not coded in the bitstream)
//!    0001: N/1*
struct Block_Transform_GetWindowCtrl_TransientFiltering_Sum_t {
	float Sum, SumW;
};
static inline void Block_Transform_GetWindowCtrl_TransientFiltering(
	const float *Data,
	const float *LastBlockData,
	      float *TransientWindow,
	      float *StepBuffer,
	      float *SmoothingTaps,
	int BlockSize,
	int nChan
) {
	int n, Chan;

	//! Restore old block's filtered data
	for(n=0;n<BlockSize/4;n++) StepBuffer[n] = TransientWindow[n];

	//! Perform a bandpass filter to isolate the energy that is
	//! important to transient detection. Generally, LF energy
	//! and HF energy are 'unimportant', and it's the MF energy
	//! that has most of the information we're interested in.
	//! Transfer function:
	//!  H(z) = z^1 - z^-1
	//! NOTE: This filter does not have unity gain, as doing so
	//! would add some multiplications that reduce performance.
	//! NOTE: We end up losing the last sample of the new block,
	//! but this shouldn't affect things. More importantly, we
	//! do NOT fix the last sample of the last subblock because
	//! this screws things up on transients from silence.
	//! NOTE: We decimate by a factor of 4 to reduce complexity
	//! and also reduce jitter a little.
	//! NOTE: BPFILT() accepts z^-1,1,z^1 for flexibility if we
	//! ever need to change the filter formula.
	//! NOTE: The operations have been arranged to minimize
	//! register usage on platforms where this will matter.
	for(n=0;n<BlockSize/4;n++) StepBuffer[n+BlockSize/4] = 0.0f;
	for(Chan=0;Chan<nChan;Chan++) {
#define BPFILT(zM1, z0, z1) ((zM1) - (z1))
		float *Dst = StepBuffer + BlockSize/4;
		const float *SrcOld = LastBlockData + Chan*BlockSize + BlockSize-1;
		const float *SrcNew = Data          + Chan*BlockSize;
		{
			*Dst += SQR(BPFILT(SrcOld[ 0], SrcNew[0], SrcNew[1]));
			*Dst += SQR(BPFILT(SrcNew[ 1], SrcNew[2], SrcNew[3]));
			*Dst += SQR(BPFILT(SrcNew[ 0], SrcNew[1], SrcNew[2]));
			*Dst += SQR(BPFILT(SrcNew[ 2], SrcNew[3], SrcNew[4]));
			Dst++, SrcNew += 4;
		}
		for(n=1;n<BlockSize/4-1;n++) {
			*Dst += SQR(BPFILT(SrcNew[-1], SrcNew[0], SrcNew[1]));
			*Dst += SQR(BPFILT(SrcNew[ 1], SrcNew[2], SrcNew[3]));
			*Dst += SQR(BPFILT(SrcNew[ 0], SrcNew[1], SrcNew[2]));
			*Dst += SQR(BPFILT(SrcNew[ 2], SrcNew[3], SrcNew[4]));
			Dst++, SrcNew += 4;
		}
		{
			*Dst += SQR(BPFILT(SrcNew[-1], SrcNew[0], SrcNew[1]));
			*Dst += SQR(BPFILT(SrcNew[ 1], SrcNew[2], SrcNew[3]));
			*Dst += SQR(BPFILT(SrcNew[ 0], SrcNew[1], SrcNew[2]));
			//*Dst += 0.0f; //! z^1 unavailable
			Dst++, SrcNew += 4;
		}
#undef BPFILT
	}
	StepBuffer[BlockSize/2-1] *= 4/3.0f; //! z^1 @ N=BlockSize/4-1 was unavailable, so use the average

	//! Apply a lowpass filter to the energy signal, and then
	//! apply DC removal.
	//! Theory:
	//!  Transients result in pulses close to DC, so we try to
	//!  remove harmonic reflections in the higher freqs. We
	//!  then apply another filter to remove DC content, as this
	//!  causes biasing of the signal analysis.
	//! NOTE: It's important to keep the smoothing taps accurate,
	//! so we save it across blocks and buffer the filtered data.
	//! NOTE: We perform the filtering in a companded domain,
	//! as this emphasizes the transient structure far better.
	//! NOTE: Slightly refactored to remove a multiplication.
	{
		float v, *Buf = StepBuffer + BlockSize/4;
		float LPTap = SmoothingTaps[0], LPDecay = 240/256.0f, OneMinusLPDecay = 1.0f - LPDecay;
		float DCTap = SmoothingTaps[1], DCDecay = 252/256.0f, OneMinusDCDecay = 1.0f - DCDecay;
		float DCGain = OneMinusDCDecay / OneMinusLPDecay;
		for(n=0;n<BlockSize/4;n++) {
			v = sqrtf(sqrtf(*Buf));
			LPTap += v;
			DCTap += v * DCGain;
			*Buf++ = SQR(LPTap - DCTap);
			LPTap *= LPDecay;
			DCTap *= DCDecay;
		}
		SmoothingTaps[0] = LPTap;
		SmoothingTaps[1] = DCTap;
	}

	//! Save new block's filtered data
	for(n=0;n<BlockSize/4;n++) TransientWindow[n] = StepBuffer[BlockSize/4+n];

	//! Accumulate energy segments
	int AnalysisIntervalMask = BlockSize/(ULC_MAX_BLOCK_DECIMATION_FACTOR*8) - 1; //! Break up into LL/L/M/R (*4), BlockSize*2 / MAX_DECIMATION*N_SEGMENTS
	float *Dst = StepBuffer, Tmp = 0.0f;
	for(n=0;n<BlockSize/2;n++) {
		//! Because everything would be summed up in the search loop
		//! of Block_Transform_GetWindowCtrl(), we sum as much as we
		//! can here to reuse as many computations as possible.
		float v = *StepBuffer++;
		Tmp += SQR(v);

		//! Wrapping around to next segment?
		//! This awkward construct is used to avoid overwriting
		//! the data we're reading in for the summation.
		if(((n+1) & AnalysisIntervalMask) == 0) *Dst++ = Tmp, Tmp = 0.0f;
	}
}
static inline float Block_Transform_GetWindowCtrl_Log2DecimationRatio(float Ratio2, int Log2SubBlockSize) {
	//! Full, unsimplified expression:
	//!  LogRatio2         = Log[Ratio^2] = 2*Log[Ratio]
	//!  OverlapSamples    = E^(-2*Log[Ratio]) * 10000; experimentally determined
	//!  OverlapDecimation = Log2[SubBlockSize / OverlapSamples] = Log2[SubBlockSize] - Log2[10000] + Log2[Ratio^2]
	return Log2SubBlockSize - 0x1.A934F1p3f + 0x1.715476p0f*logf(Ratio2);
}
static inline int Block_Transform_GetWindowCtrl(
	const float *Data,
	const float *LastBlockData,
	      float *TransientWindow,
	      float *StepBuffer,
	      float *SmoothingTaps,
	int BlockSize,
	int nChan
) {
	int n;

	//! Perform filtering to obtain pre-echo analysis
	Block_Transform_GetWindowCtrl_TransientFiltering(Data, LastBlockData, TransientWindow, StepBuffer, SmoothingTaps, BlockSize, nChan);

	//! Begin binary search for transient segment until it stops
	//! on the R side, at which point the largest ratio is stored
	float DecimationRatio;
	int Decimation  = 0b0001;
	int AnalysisLen = ULC_MAX_BLOCK_DECIMATION_FACTOR;
	int Log2SubBlockSize = 31 - __builtin_clz(BlockSize);
	for(StepBuffer += AnalysisLen;;) { //! MDCT transition region begins -BlockSize/2 samples from the new block (ie. L segment, in LL/L/M/R notation)
		//! Find the peak ratio within each segment (L/M/R)
		enum { POS_L, POS_M, POS_R};
		float Ratio;
		int RatioPos; {
			//! Get the energy of each segment (LL/L/M/R)
			//! NOTE: Do not use FLT_MIN as the bias, as we need
			//! some room for the ratio to grow into upon division.
			float LL = 0x1.0p-64f;
			float L  = 0x1.0p-64f;
			float M  = 0x1.0p-64f;
			float R  = 0x1.0p-64f;
			for(n=0;n<AnalysisLen;n++) {
				LL += StepBuffer[-1*AnalysisLen + n];
				L  += StepBuffer[ 0*AnalysisLen + n];
				M  += StepBuffer[+1*AnalysisLen + n];
				R  += StepBuffer[+2*AnalysisLen + n];
			}

			//! Get the ratios between the segments
			float RatioL = L / LL;
			float RatioM = M / L;
			float RatioR = R / M;

			//! Select the largest ratio of L/M/R
			                   RatioPos = POS_L, Ratio = RatioL;
			if(RatioM > Ratio) RatioPos = POS_M, Ratio = RatioM;
			if(RatioR > Ratio) RatioPos = POS_R, Ratio = RatioR;

			//! If we can't decimate, select R (but keep the largest ratio).
			//! NOTE: Minimum subblock size of 64 samples.
			//! NOTE: Checking AnalysisLen should be better than checking
			//! Decimation directly, as then we can change the maximum allowed
			//! decimation without changing this code.
			if(!ULC_USE_WINDOW_SWITCHING || AnalysisLen <= 1 || Log2SubBlockSize <= 6) {
				RatioPos = POS_R;
			}
		}

		//! If the transient is not in the transition region and
		//! is still significant, decimate the subblock further
		DecimationRatio = Block_Transform_GetWindowCtrl_Log2DecimationRatio(Ratio, Log2SubBlockSize);
		if(RatioPos != POS_R && DecimationRatio > 1.0f) {
			//! Update the decimation pattern and continue
			if(RatioPos == POS_L)
				Decimation  = (Decimation<<1) | 0;
			else
				Decimation  = (Decimation<<1) | 1,
				StepBuffer += AnalysisLen;
			AnalysisLen /= 2;
			Log2SubBlockSize--;
			continue;
		}

		//! No more decimation - break out of the decimation loop
		break;
	}

	//! Determine overlap size from the ratio
	//! NOTE: Round down the scaling, rather than round off.
	int OverlapScale = (DecimationRatio < 1.0f) ? 0 : (DecimationRatio >= 7.0f) ? 7 : (int)DecimationRatio;
	if(Log2SubBlockSize-OverlapScale < 4) OverlapScale = Log2SubBlockSize-4; //! Minimum 16-sample overlap

	//! Return the combined overlap+window switching parameters
	return OverlapScale + 0x8*(Decimation != 1) + 0x10*Decimation;
}

/**************************************/
//! EOF
/**************************************/
