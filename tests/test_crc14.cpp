/**
 * Test: compare our custom crc14 vs boost::augmented_crc<14, 0x2757>
 */
#include <boost/crc.hpp>
#include <cstdio>
#include <cstring>

extern "C" {
    short crc14(unsigned char const * data, int length);
}

int main() {
    // Test vector: 12 bytes (same layout as encode174_91)
    unsigned char data[12] = {};
    // Fill with a test pattern: "CQ K1ABC FN42" packed as 77 bits + 3 pad + 16 zeros
    // For simplicity, use a known byte pattern
    unsigned char test1[12] = {0xA5, 0x3C, 0x7E, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0xDE, 0x00, 0x00};
    unsigned char test2[12] = {0xFF, 0x00, 0xAA, 0x55, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC, 0x00, 0x00};
    unsigned char test3[12] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};

    struct { unsigned char *data; const char *name; } tests[] = {
        {test1, "test1"}, {test2, "test2"}, {test3, "test3"}
    };

    int pass = 0, total = 0;
    for (auto &t : tests) {
        short ours = crc14(t.data, 12);
        short boost_crc = boost::augmented_crc<14, 0x2757>(t.data, 12);
        total++;
        bool ok = (ours == boost_crc);
        if (ok) pass++;
        std::printf("%s: ours=%d boost=%d %s\n", t.name, ours, boost_crc, ok ? "PASS" : "FAIL");
    }
    std::printf("\n%d/%d passed\n", pass, total);
    return (pass == total) ? 0 : 1;
}
