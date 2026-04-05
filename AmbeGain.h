#pragma once

// AMBE2+ bitstream gain adjustment via b2 (delta-gamma) parameter manipulation.
// Modifies the gain field directly in the 72-bit (9-byte) AMBE2+ frame without
// requiring vocoder decode/re-encode. Uses Golay(24,12) FEC for the A partition.
//
// b2 is a 5-bit field (0-31) at voice bit positions [8,9,10,11,36].
// Bits 8-11 are in the Golay-protected A partition, bit 36 is in the C partition.
// b2 indexes the AmbeDg[] delta-gamma table; reducing b2 lowers frame loudness.
//
// References:
//   - mbelib ambe3600x2450.c (b2 extraction and AmbeDg table)
//   - ETSI TS 102 361 (DMR air interface)
//   - xlxd/ambed (CODECGAIN_AMBE2PLUS = +10 dB)

#include <cstdint>
#include <cmath>
#include "Golay24128.h"

// Bit position tables for 72-bit AMBE2+ frame partitions (from urfd YSFDefines.h)
static const unsigned int AMBE_A_TABLE[] = {
	0U,  4U,  8U, 12U, 16U, 20U, 24U, 28U, 32U, 36U, 40U, 44U,
	48U, 52U, 56U, 60U, 64U, 68U,  1U,  5U,  9U, 13U, 17U, 21U
};
static const unsigned int AMBE_C_TABLE[] = {
	46U, 50U, 54U, 58U, 62U, 66U, 70U,  3U,  7U, 11U, 15U, 19U, 23U,
	27U, 31U, 35U, 39U, 43U, 47U, 51U, 55U, 59U, 63U, 67U, 71U
};

static const uint8_t AMBE_BIT_MASK[] = {0x80U, 0x40U, 0x20U, 0x10U, 0x08U, 0x04U, 0x02U, 0x01U};
#define AMBE_READ_BIT(p,i)     (((p)[(i)>>3] & AMBE_BIT_MASK[(i)&7]) != 0)
#define AMBE_WRITE_BIT(p,i,b)  (p)[(i)>>3] = (b) ? ((p)[(i)>>3] | AMBE_BIT_MASK[(i)&7]) : ((p)[(i)>>3] & ~AMBE_BIT_MASK[(i)&7])

// Convert dB offset to approximate b2 step offset.
// In the useful range (indices 8-24), each b2 step ≈ 1.0-1.4 dB.
// The DPCM formula (gamma = delta + 0.5*prev) means the steady-state
// effect is ~2x the delta change, so each b2 step ≈ 2 dB perceived.
// Empirical factor: 0.4 steps per dB → -16 dB ≈ 6 steps.
static inline int AmbeDbToSteps(int gain_db)
{
	return (int)roundf((float)gain_db * -0.4f);
}

// Adjust the b2 (delta-gamma) gain parameter in a 9-byte AMBE2+ frame.
// step_offset > 0 reduces gain (quieter), < 0 increases gain (louder).
// The A partition is Golay(24,12) protected; FEC is recomputed after modification.
static inline void AmbeAdjustGain(uint8_t *ambe, int step_offset)
{
	if (step_offset == 0) return;

	// 1. De-interleave A partition (24 bits) from 72-bit frame
	unsigned int a_bits = 0;
	for (int i = 0; i < 24; i++)
	{
		a_bits <<= 1;
		if (AMBE_READ_BIT(ambe, AMBE_A_TABLE[i]))
			a_bits |= 1;
	}

	// 2. Golay decode: extract 12 data bits (with error correction)
	unsigned int data12 = CGolay24128::decode24128(a_bits);

	// 3. Extract b2 upper 4 bits from data bits 8-11
	//    data12 bit numbering: bit 11 = MSB, bit 0 = LSB
	//    Voice bits 0-11 map to data12 bits 11..0
	//    Voice bit 8 = data12 bit 3, voice bit 9 = data12 bit 2,
	//    voice bit 10 = data12 bit 1, voice bit 11 = data12 bit 0
	unsigned int b2_upper = (data12 & 0x0F);  // bits 3..0 = voice bits 8..11

	// 4. Read b2 LSB from C partition bit 12 (voice bit 36 = 24+12)
	unsigned int b2_lsb = AMBE_READ_BIT(ambe, AMBE_C_TABLE[12]) ? 1 : 0;

	// 5. Reconstruct b2: upper4 << 1 | lsb
	int b2 = (int)((b2_upper << 1) | b2_lsb);

	// 6. Apply offset and clamp
	b2 -= step_offset;
	if (b2 < 0) b2 = 0;
	if (b2 > 31) b2 = 31;

	// 7. Write back b2 upper 4 bits into data12
	data12 = (data12 & 0xFF0) | ((unsigned int)b2 >> 1);

	// 8. Golay re-encode
	unsigned int a_new = CGolay24128::encode24128(data12);

	// 9. Re-interleave A partition back into 72-bit frame
	for (int i = 0; i < 24; i++)
	{
		AMBE_WRITE_BIT(ambe, AMBE_A_TABLE[i], (a_new >> (23 - i)) & 1);
	}

	// 10. Write b2 LSB back to C partition
	AMBE_WRITE_BIT(ambe, AMBE_C_TABLE[12], b2 & 1);
}

#undef AMBE_READ_BIT
#undef AMBE_WRITE_BIT
