/**************************************/
//! ulc-codec: Ultra-Low-Complexity Audio Codec
//! Copyright (C) 2021, Ruben Nunez (Aikku; aik AT aol DOT com DOT au)
//! Refer to the project README file for license terms.
/**************************************/
#include <math.h>
#include <stdint.h>
/**************************************/
#include "Fourier.h"
#include "ulcEncoder.h"
/**************************************/
#if ULC_USE_NOISE_CODING
# include "ulcEncoder_NoiseFill.h"
#endif
/**************************************/

//! The target memory is always aligned to 64 bytes, so just
//! use whatever is most performant on the target architecture
typedef uint32_t BitStream_t; //! <- MUST BE UNSIGNED
#define BISTREAM_NBITS (8u*sizeof(BitStream_t))

/**************************************/

ULC_FORCED_INLINE void Block_Encode_WriteNybble(BitStream_t x, BitStream_t **Dst, int *Size) {
	//! Push nybble
	*(*Dst) >>= 4;
	*(*Dst)  |= x << (BISTREAM_NBITS - 4);
	*Size += 4;

	//! Next byte?
	if((*Size)%BISTREAM_NBITS == 0) (*Dst)++;
}
ULC_FORCED_INLINE void Block_Encode_WriteQuantizer(int qi, BitStream_t **DstBuffer, int *Size, int Lead) {
	int s = qi - 5;
	if(Lead) {
		Block_Encode_WriteNybble(0x8, DstBuffer, Size);
		Block_Encode_WriteNybble(0x0, DstBuffer, Size);
	}
	if(s < 0xE) {
		//! 8h,0h,0h..Dh: Quantizer change
		Block_Encode_WriteNybble(s, DstBuffer, Size);
	} else {
		//! 8h,0h,Eh,0h..Ch: Quantizer change (extended precision)
		Block_Encode_WriteNybble(  0xE, DstBuffer, Size);
		Block_Encode_WriteNybble(s-0xE, DstBuffer, Size);
	}
}

/**************************************/

//! Build quantizer from weighted average
//! FIXME: This is NOT OPTIMAL. Large amount of error
//! can result from this method (relative to the real
//! optimal quantizer).
static inline int Block_Encode_BuildQuantizer(float Scale) {
	//! NOTE: `q` will always be greater than 5 due to the bias so
	//! the quantizer code syntax is biased accordingly.
	int q = (int)ceilf(4.0f - 0x1.715476p0f*logf(Scale)); //! 0x1.715476p0 == 1/Ln[2] for change of base
	if(q < 5) q = 5; //! Sometimes happens because of overflow?
	if(q > 5 + 0xE + 0xC) q = 5 + 0xE + 0xC; //! 5+Eh+Ch = Maximum extended-precision quantizer value (including a bias of 5)
	return q;
}

/**************************************/

//! Encode a range of coefficients
static inline int Block_Encode_EncodePass_WriteQuantizerZone(
	int           CurIdx,
	int           EndIdx,
	float         Quant,
	const float  *Coef,
#if ULC_USE_NOISE_CODING
	const float  *CoefNoise,
#endif
	const int    *CoefIdx,
	int           NextCodedIdx,
	int          *PrevQuant,
	int           nOutCoef,
	BitStream_t **DstBuffer,
	int          *Size
) {
	//! Write/update the quantizer
	float q; {
		int qi = Block_Encode_BuildQuantizer(Quant);
		q = (float)(1u << qi);
		if(qi != *PrevQuant) {
			Block_Encode_WriteQuantizer(qi, DstBuffer, Size, *PrevQuant != -1);
			*PrevQuant = qi;
		}
	}

	//! Write the coefficients
	do {
		//! Code the zero runs
		int n, v, zR = CurIdx - NextCodedIdx;
		while(zR) {
#if ULC_USE_NOISE_CODING
			//! Determine the quantized coefficient for noise-fill mode
			//! NOTE: The range of noise samples is coded as a trade-off
			//! between noise-fill commands and coded coefficients, as
			//! noise-fill competes with coefficients we /want/ to code
			//! for the bit bandwidth available. Setting the start range
			//! too low favours noise-fill over psychoacoustically
			//! significant coefficients, but setting it too high can
			//! cause degradation in small block sizes as well as when
			//! we get many short runs.
			//! TODO: Is there a better min-number-of-coefficients?
			//! Too low and this fights too much with the coefficients
			//! we are actually trying to code (reducing the coding
			//! rate of them, and increasing the coding rate of noise).
			//! However, with small BlockSize (or SubBlockSize when
			//! decimating), smaller runs are useful.
			int NoiseQ = 0;
			if(zR >= 16) {
				v = zR - 16; if(v > 0x1FF) v = 0x1FF;
				n = v  + 16;
				NoiseQ = Block_Encode_EncodePass_GetNoiseQ(CoefNoise, NextCodedIdx, n, q);
			}
			if(NoiseQ) {
				//! 0h,Zh,Yh,Xh: 16 .. 527 noise fill (Xh.bit[1..3] != 0)
				Block_Encode_WriteNybble(0x0,    DstBuffer, Size);
				Block_Encode_WriteNybble(v>>5,   DstBuffer, Size);
				Block_Encode_WriteNybble(v>>1,   DstBuffer, Size);
				Block_Encode_WriteNybble((v&1) | NoiseQ<<1, DstBuffer, Size);
			} else {
#endif
				//! Determine which run type to use and get the number of zeros coded
				//! NOTE: A short run takes 2 nybbles, and a long run takes 4 nybbles.
				//! So two short runs of maximum length code up to 30 zeros with the
				//! same efficiency as a long run, meaning that long runs start with
				//! 31 zeros.
				if(zR < 31) {
					//! 8h,1h..Fh: Zeros fill (1 .. 15 coefficients)
					v = zR - 0; if(v > 0xF) v = 0xF;
					n = v  + 0;
					Block_Encode_WriteNybble(0x8, DstBuffer, Size);
					Block_Encode_WriteNybble(v,   DstBuffer, Size);
				} else {
					//! 0h,Zh,Yh,Xh: 31 .. 542 zeros fill (Xh.bit[1..3] == 0)
					v = zR - 31; if(v > 0x1FF) v = 0x1FF;
					n = v  + 31;
					Block_Encode_WriteNybble(0x0,  DstBuffer, Size);
					Block_Encode_WriteNybble(v>>5, DstBuffer, Size);
					Block_Encode_WriteNybble(v>>1, DstBuffer, Size);
					Block_Encode_WriteNybble(v&1,  DstBuffer, Size);
				}
#if ULC_USE_NOISE_CODING
			}
#endif
			//! Skip the zeros
			NextCodedIdx += n;
			zR           -= n;
		}

		//! -7h..-1h, +1h..+7h: Normal coefficient
		int Qn = ULC_ClippedCompandedQuantizeCoefficient(Coef[CurIdx]*q);
		Block_Encode_WriteNybble(Qn, DstBuffer, Size);
		NextCodedIdx++;

		//! Move to the next coefficient
		do CurIdx++; while(CurIdx < EndIdx && CoefIdx[CurIdx] >= nOutCoef);
	} while(CurIdx < EndIdx);
	return NextCodedIdx;
}

//! Encode a [sub]block
static inline void Block_Encode_EncodePass_WriteSubBlock(
	int           Idx,
	int           SubBlockSize,
	const float  *Coef,
#if ULC_USE_NOISE_CODING
	const float  *CoefNoise,
#endif
	const int    *CoefIdx,
	int           nOutCoef,
	BitStream_t **DstBuffer,
	int          *Size
) {
	//! Encode direct coefficients
	int   EndIdx        = Idx+SubBlockSize;
	int   NextCodedIdx  = Idx;
	int   PrevQuant     = -1;
	int   QuantStartIdx = -1;
	float QuantSum      = 0.0f;
	float QuantWeight   = 0.0f;
	do {
		//! Seek the next coefficient
		while(Idx < EndIdx && CoefIdx[Idx] >= nOutCoef) Idx++;

		//! Read coefficient and set the first quantizer's first coefficient index
		//! NOTE: Set BandCoef=0.0 upon reaching the end. This causes the range
		//! check to fail in the next step, causing the final quantizer zone to dump
		//! if we had any data (if we don't, the check "passes" because QuantSum==0).
		float BandCoef = 0.0f;
		if(Idx < EndIdx) {
			BandCoef = ABS(Coef[Idx]);
			if(QuantStartIdx == -1) QuantStartIdx = Idx;
		}

		//! Level out of range in this quantizer zone?
		const float MaxRangeLo = 8.0f;
		const float MaxRangeHi = 2.0f;
		if(
			MaxRangeLo*BandCoef*QuantWeight < QuantSum || //! BandCoef*MaxRangeLo < QuantSum/QuantWeight
			BandCoef*QuantWeight > MaxRangeHi*QuantSum    //! BandCoef/MaxRangeHi > QuantSum/QuantWeight
		) {
			//! Write the quantizer zone we just searched through
			//! and start a new one from this coefficient
			NextCodedIdx = Block_Encode_EncodePass_WriteQuantizerZone(
				QuantStartIdx,
				Idx,
				QuantSum/QuantWeight,
				Coef,
#if ULC_USE_NOISE_CODING
				CoefNoise,
#endif
				CoefIdx,
				NextCodedIdx,
				&PrevQuant,
				nOutCoef,
				DstBuffer,
				Size
			);
			QuantStartIdx = Idx;
			QuantSum    = 0.0f;
			QuantWeight = 0.0f;
		}

		//! Accumulate to the current quantizer zone.
		//! This uses a contraharmonic mean as the quantizer step size.
		QuantSum    += BandCoef * BandCoef;
		QuantWeight += BandCoef;
	} while(++Idx <= EndIdx); //! Idx will go over by 1 or 2 here to dump the last quantizer zone, but doesn't matter; not used after this loop

	//! Decide what to do about the tail coefficients
	//! If we're at the edge of the block, it might work better to just fill with 0h
	int n = EndIdx - NextCodedIdx;
	if(n > 4) {
		//! If we coded anything, then we must specify the lead sequence
		if(PrevQuant != -1) {
			Block_Encode_WriteNybble(0x8, DstBuffer, Size);
			Block_Encode_WriteNybble(0x0, DstBuffer, Size);
		}

		//! Analyze the remaining data for noise-fill mode
#if ULC_USE_NOISE_CODING
		int NoiseQ = 0, NoiseDecay = 0;
		if(PrevQuant != -1 && n >= 16) { //! Don't use noise-fill for ultra-short tails
			Block_Encode_EncodePass_GetHFExtParams(
				CoefNoise,
				NextCodedIdx,
				n,
				(float)(1u << PrevQuant),
				&NoiseQ,
				&NoiseDecay
			);
		}
		if(NoiseQ) {
			//! 8h,0h,Fh,Zh,Yh[,Xh]: Noise fill (to end; exp-decay)
			NoiseQ--; //! Biased by 1
			Block_Encode_WriteNybble(0xF, DstBuffer, Size);
			if(NoiseDecay > 0xF) {
				Block_Encode_WriteNybble(NoiseQ<<1 | 1, DstBuffer, Size);
				Block_Encode_WriteNybble(NoiseDecay>>4, DstBuffer, Size);
				Block_Encode_WriteNybble(NoiseDecay,    DstBuffer, Size);
			} else {
				Block_Encode_WriteNybble(NoiseQ<<1 | 0, DstBuffer, Size);
				Block_Encode_WriteNybble(NoiseDecay,    DstBuffer, Size);
			}
		} else {
#endif
			//! 8h,0h,Eh,Fh: Stop
			Block_Encode_WriteNybble(0xE, DstBuffer, Size);
			Block_Encode_WriteNybble(0xF, DstBuffer, Size);
#if ULC_USE_NOISE_CODING
		}
#endif
	} else if(n > 0) {
		//! If we have less than 4 coefficients, it's cheaper to
		//! store a zero run than to do anything else.
		Block_Encode_WriteNybble(0x8, DstBuffer, Size);
		Block_Encode_WriteNybble(n,   DstBuffer, Size);
	}
}

/**************************************/

//! Returns the block size (in bits) and the number of coded (non-zero) coefficients
static inline int Block_Encode_EncodePass(const struct ULC_EncoderState_t *State, void *_DstBuffer, int nOutCoef) {
	int BlockSize   = State->BlockSize;
	int Chan, nChan = State->nChan;
	const float *Coef      = State->TransformBuffer;
#if ULC_USE_NOISE_CODING
	const float *CoefNoise = State->TransformNoise;
#endif
	const int   *CoefIdx   = State->TransformIndex;
	BitStream_t *DstBuffer = _DstBuffer;

	//! Begin coding
	int Idx  = 0;
	int Size = 0; //! Block size (in bits)
	int WindowCtrl = State->WindowCtrl; {
		Block_Encode_WriteNybble(WindowCtrl, &DstBuffer, &Size);
		if(WindowCtrl & 0x8) Block_Encode_WriteNybble(WindowCtrl >> 4, &DstBuffer, &Size);
	}
	for(Chan=0;Chan<nChan;Chan++) {
		ULC_SubBlockDecimationPattern_t DecimationPattern = ULC_SubBlockDecimationPattern(WindowCtrl);
		do {
			int SubBlockSize = BlockSize >> (DecimationPattern&0x7);
			Block_Encode_EncodePass_WriteSubBlock(
				Idx,
				SubBlockSize,
				Coef,
#if ULC_USE_NOISE_CODING
				CoefNoise,
#endif
				CoefIdx,
				nOutCoef,
				&DstBuffer,
				&Size
			);
			Idx += SubBlockSize;
		} while(DecimationPattern >>= 4);
	}

	//! Align the output stream and pad size to bytes
	*DstBuffer >>= (-Size) % BISTREAM_NBITS;
	Size = (Size+7) &~ 7;
	return Size;
}

/**************************************/

#undef BISTREAM_NBITS

/**************************************/
//! EOF
/**************************************/
