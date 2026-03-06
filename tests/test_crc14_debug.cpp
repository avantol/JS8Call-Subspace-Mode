/**
 * Debug: step through CRC computation to find the discrepancy
 */
#include <boost/crc.hpp>
#include <cstdio>
#include <cstdint>

// Our implementation (from crc14.cpp)
unsigned int our_crc14(unsigned char const * data, int length) {
    constexpr unsigned int POLY = 0x2757;
    constexpr int CRC_BITS = 14;
    unsigned int crc = 0;
    for (int i = 0; i < length; ++i) {
        crc ^= static_cast<unsigned int>(data[i]) << (CRC_BITS - 8);
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & (1u << (CRC_BITS - 1)))
                crc = (crc << 1) ^ POLY;
            else
                crc <<= 1;
            crc &= (1u << CRC_BITS) - 1;
        }
    }
    return crc;
}

// Boost's augmented CRC processes differently - it's a "raw" remainder computation
// The key: boost::augmented_crc<14, 0x2757> treats data as a big-endian bit stream
// and computes the remainder. But 14-bit CRC with 8-bit bytes means the register
// is wider than a byte, requiring careful alignment.

int main() {
    unsigned char data[4] = {0xA5, 0x00, 0x00, 0x00};

    unsigned int ours = our_crc14(data, 4);
    short boost_val = boost::augmented_crc<14, 0x2757>(data, 4);

    printf("ours: 0x%04X (%d)\n", ours, ours);
    printf("boost: 0x%04X (%d)\n", (unsigned short)boost_val, boost_val);

    // Try different shift amounts
    for (int shift = 0; shift <= 8; shift++) {
        unsigned int crc = 0;
        for (int i = 0; i < 4; ++i) {
            crc ^= static_cast<unsigned int>(data[i]) << shift;
            for (int bit = 0; bit < 8; ++bit) {
                if (crc & (1u << 13))
                    crc = (crc << 1) ^ 0x2757;
                else
                    crc <<= 1;
                crc &= 0x3FFF;
            }
        }
        printf("shift=%d: 0x%04X (%d) %s\n", shift, crc, crc,
               crc == (unsigned short)boost_val ? "MATCH!" : "");
    }

    return 0;
}
