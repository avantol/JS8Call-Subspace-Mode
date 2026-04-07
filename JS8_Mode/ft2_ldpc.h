#pragma once
#ifdef JS8_ENABLE_FT2
#include <cstdint>

namespace FT2 {

// CRC-14 for LDPC codec (augmented CRC, polynomial 0x2757)
uint16_t crc14(const uint8_t *data, int len);
bool     crc14_check(const uint8_t *data, int len);

// Encode 77 message bits → 174-bit LDPC codeword
// Adds 14-bit CRC to form 91-bit message, then multiplies by parity matrix.
// Input:  msg77[77]     – 0/1 values (XOR with rvec already applied by caller)
// Output: codeword[174] – 0/1 values: [message91 (91)] ++ [parity (83)]
void encode174_91(const int8_t *msg77, int8_t *codeword);

// Belief-Propagation LDPC decoder for (174, 91) code.
// Input:  llr[174]       – soft log-likelihood ratios (positive → bit is 1)
// Output: message91[91]  – hard-decision information bits (0/1)
//         cw[174]        – full hard-decision codeword
//         nharderror     – number of bit errors vs channel hard decisions
//                          (-1 if decode failed, ≥0 on success)
// Returns true on success (valid codeword with CRC14 check passing).
bool decode174_91_bp(const float *llr, int8_t *message91, int8_t *cw,
                     int *nharderror);

} // namespace FT2
#endif // JS8_ENABLE_FT2
