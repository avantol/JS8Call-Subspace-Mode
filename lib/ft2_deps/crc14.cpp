/**
 * CRC-14 implementation for FT2 LDPC codec.
 * Uses boost::augmented_crc<14, 0x2757> (matching WSJT-X original).
 */

#include <boost/crc.hpp>

extern "C"
{
   short crc14 (unsigned char const * data, int length);
   bool crc14_check (unsigned char const * data, int length);
}

#define POLY 0x2757

namespace
{
  unsigned long constexpr TRUNCATED_POLYNOMIAL = POLY;
}

// assumes CRC is last 14 bits of the data and is set to zero
// caller should assign the returned CRC into the message in big endian byte order
short crc14 (unsigned char const * data, int length)
{
    return boost::augmented_crc<14, TRUNCATED_POLYNOMIAL> (data, length);
}

bool crc14_check (unsigned char const * data, int length)
{
   return !boost::augmented_crc<14, TRUNCATED_POLYNOMIAL> (data, length);
}
