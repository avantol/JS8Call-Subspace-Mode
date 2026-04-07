/**
 * @file ft2_ldpc.cpp
 * @brief LDPC(174,91) codec for FT2/Subspace mode.
 *
 * Direct C++ port of the Fortran encode174_91.f90 / decode174_91.f90 routines
 * from the JS8Call-Subspace-Mode project. Generator and parity-check matrices
 * are identical to those used in WSJT-X FT8 (same (174,91) LDPC code).
 *
 * Codec parameters:
 *   N   = 174   total codeword bits
 *   K   = 91    information bits (77 message + 14 CRC14)
 *   M   = 83    parity-check bits (= N − K)
 *   ncw = 3     checks per variable node (constant for this code)
 *   max nrw = 7 variable nodes per check node
 */

#ifdef JS8_ENABLE_FT2

#include "ft2_ldpc.h"

#include <boost/crc.hpp>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <mutex>

namespace FT2 {

/******************************************************************************/
// CRC-14
/******************************************************************************/

uint16_t crc14(const uint8_t *data, int len)
{
    return boost::augmented_crc<14, 0x2757u>(data, static_cast<std::size_t>(len));
}

bool crc14_check(const uint8_t *data, int len)
{
    return boost::augmented_crc<14, 0x2757u>(data, static_cast<std::size_t>(len)) == 0;
}

/******************************************************************************/
// Generator matrix G[83][91]
// Each row is a 23-character hex string encoding 91 bits, MSB first.
// (Last hex char encodes only 3 bits since 22*4+3 = 91.)
/******************************************************************************/

static const char *s_gen_hex[83] = {
    "8329ce11bf31eaf509f27fc", "761c264e25c259335493132",
    "dc265902fb277c6410a1bdc", "1b3f417858cd2dd33ec7f62",
    "09fda4fee04195fd034783a", "077cccc11b8873ed5c3d48a",
    "29b62afe3ca036f4fe1a9da", "6054faf5f35d96d3b0c8c3e",
    "e20798e4310eed27884ae90", "775c9c08e80e26ddae56318",
    "b0b811028c2bf997213487c", "18a0c9231fc60adf5c5ea32",
    "76471e8302a0721e01b12b8", "ffbccb80ca8341fafb47b2e",
    "66a72a158f9325a2bf67170", "c4243689fe85b1c51363a18",
    "0dff739414d1a1b34b1c270", "15b48830636c8b99894972e",
    "29a89c0d3de81d665489b0e", "4f126f37fa51cbe61bd6b94",
    "99c47239d0d97d3c84e0940", "1919b75119765621bb4f1e8",
    "09db12d731faee0b86df6b8", "488fc33df43fbdeea4eafb4",
    "827423ee40b675f756eb5fe", "abe197c484cb74757144a9a",
    "2b500e4bc0ec5a6d2bdbdd0", "c474aa53d70218761669360",
    "8eba1a13db3390bd6718cec", "753844673a27782cc42012e",
    "06ff83a145c37035a5c1268", "3b37417858cc2dd33ec3f62",
    "9a4a5a28ee17ca9c324842c", "bc29f465309c977e89610a4",
    "2663ae6ddf8b5ce2bb29488", "46f231efe457034c1814418",
    "3fb2ce85abe9b0c72e06fbe", "de87481f282c153971a0a2e",
    "fcd7ccf23c69fa99bba1412", "f0261447e9490ca8e474cec",
    "4410115818196f95cdd7012", "088fc31df4bfbde2a4eafb4",
    "b8fef1b6307729fb0a078c0", "5afea7acccb77bbc9d99a90",
    "49a7016ac653f65ecdc9076", "1944d085be4e7da8d6cc7d0",
    "251f62adc4032f0ee714002", "56471f8702a0721e00b12b8",
    "2b8e4923f2dd51e2d537fa0", "6b550a40a66f4755de95c26",
    "a18ad28d4e27fe92a4f6c84", "10c2e586388cb82a3d80758",
    "ef34a41817ee02133db2eb0", "7e9c0c54325a9c15836e000",
    "3693e572d1fde4cdf079e86", "bfb2cec5abe1b0c72e07fbe",
    "7ee18230c583cccc57d4b08", "a066cb2fedafc9f52664126",
    "bb23725abc47cc5f4cc4cd2", "ded9dba3bee40c59b5609b4",
    "d9a7016ac653e6decdc9036", "9ad46aed5f707f280ab5fc4",
    "e5921c77822587316d7d3c2", "4f14da8242a8b86dca73352",
    "8b8b507ad467d4441df770e", "22831c9cf1169467ad04b68",
    "213b838fe2ae54c38ee7180", "5d926b6dd71f085181a4e12",
    "66ab79d4b29ee6e69509e56", "958148682d748a38dd68baa",
    "b8ce020cf069c32a723ab14", "f4331d6d461607e95752746",
    "6da23ba424b9596133cf9c8", "a636bcbc7b30c5fbeae67fe",
    "5cb0d86a07df654a9089a20", "f11f106848780fc9ecdd80a",
    "1fbb5364fb8d2c9d730d5ba", "fcb86bc70a50c9d02a5d034",
    "a534433029eac15f322e34c", "c989d9c7c3d3b8c55d75130",
    "7bb38b2f0186d46643ae962", "2644ebadeb44b9467d1f42c",
    "608cc857594bfbb55d69600"
};

static int8_t s_gen[83][91];
static bool   s_gen_inited = false;
static std::mutex s_gen_mutex;

static void initGen()
{
    std::lock_guard<std::mutex> lk(s_gen_mutex);
    if (s_gen_inited) return;
    std::memset(s_gen, 0, sizeof(s_gen));

    for (int i = 0; i < 83; ++i) {
        const char *row = s_gen_hex[i];
        for (int j = 0; j < 23; ++j) {
            char c = row[j];
            int v = (c >= '0' && c <= '9') ? (c - '0')         :
                    (c >= 'a' && c <= 'f') ? (c - 'a' + 10)    :
                    (c >= 'A' && c <= 'F') ? (c - 'A' + 10)    : 0;
            int ibmax = (j == 22) ? 3 : 4;            // last char: 3 bits
            for (int jj = 1; jj <= ibmax; ++jj) {
                int col = j * 4 + (jj - 1);           // 0-based column 0..90
                if ((v >> (4 - jj)) & 1)
                    s_gen[i][col] = 1;
            }
        }
    }
    s_gen_inited = true;
}

/******************************************************************************/
// Parity-check matrix
//
// Mn[j][3]  – the 3 check nodes connected to variable node j   (0-based)
// Nm[i][7]  – variable nodes in check i (0-based); -1 = unused slot
// nrw[i]    – number of valid variable nodes in check i
/******************************************************************************/

// clang-format off
static const int16_t Mn[174][3] = {
    {15,44,72},{24,50,61},{32,57,77},{0,43,44},{1,6,60},{2,5,53},
    {3,34,47},{4,12,20},{7,55,78},{8,63,68},{9,18,65},{10,35,59},
    {11,36,57},{13,31,42},{14,62,79},{16,27,76},{17,73,82},{21,52,80},
    {22,29,33},{23,30,39},{25,40,75},{26,56,69},{28,48,64},{2,37,77},
    {4,38,81},{45,49,72},{50,51,73},{54,70,71},{43,66,71},{42,67,77},
    {0,31,58},{1,5,70},{3,15,53},{6,64,66},{7,29,41},{8,21,30},
    {9,17,75},{10,22,81},{11,27,60},{12,51,78},{13,49,50},{14,80,82},
    {16,28,59},{18,32,63},{19,25,72},{20,33,39},{23,26,76},{24,54,57},
    {34,52,65},{35,47,67},{36,45,74},{37,44,46},{38,56,68},{40,55,61},
    {19,48,52},{45,51,62},{44,69,74},{26,34,79},{0,14,29},{1,67,79},
    {2,35,50},{3,27,50},{4,30,55},{5,19,36},{6,39,81},{7,59,68},
    {8,9,48},{10,43,56},{11,38,58},{12,23,54},{13,20,64},{15,70,77},
    {16,29,75},{17,24,79},{18,60,82},{21,37,76},{22,40,49},{6,25,57},
    {28,31,80},{32,39,72},{17,33,47},{12,41,63},{4,25,42},{46,68,71},
    {53,54,69},{44,61,67},{9,62,66},{13,65,71},{21,59,73},{34,38,78},
    {0,45,63},{0,23,65},{1,4,69},{2,30,64},{3,48,57},{0,3,4},
    {5,59,66},{6,31,74},{7,47,81},{8,34,40},{9,38,61},{10,13,60},
    {11,70,73},{12,22,77},{10,34,54},{14,15,78},{6,8,15},{16,53,62},
    {17,49,56},{18,29,46},{19,63,79},{20,27,68},{21,24,42},{12,21,36},
    {1,46,50},{22,53,73},{25,33,71},{26,35,36},{20,35,62},{28,39,43},
    {18,25,56},{2,45,81},{13,14,57},{32,51,52},{29,42,51},{5,8,51},
    {26,32,64},{24,68,72},{37,54,82},{19,38,76},{17,28,55},{31,47,70},
    {41,50,58},{27,43,78},{33,59,61},{30,44,60},{45,67,76},{5,23,75},
    {7,9,77},{39,40,69},{16,49,52},{41,65,67},{3,21,71},{35,63,80},
    {12,28,46},{1,7,80},{55,66,72},{4,37,49},{11,37,63},{58,71,79},
    {2,25,78},{44,75,80},{0,64,73},{6,17,76},{10,55,58},{13,38,53},
    {15,36,65},{9,27,54},{14,59,69},{16,24,81},{19,29,30},{11,66,67},
    {22,74,79},{26,31,61},{23,68,74},{18,20,70},{33,52,60},{34,45,46},
    {32,58,75},{39,42,82},{40,41,62},{48,74,82},{19,43,47},{41,48,56}
};

static const int16_t Nm[83][7] = {
    {3,30,58,90,91,95,152},{4,31,59,92,114,145,-1},{5,23,60,93,121,150,-1},
    {6,32,61,94,95,142,-1},{7,24,62,82,92,95,147},{5,31,63,96,125,137,-1},
    {4,33,64,77,97,106,153},{8,34,65,98,138,145,-1},{9,35,66,99,106,125,-1},
    {10,36,66,86,100,138,157},{11,37,67,101,104,154,-1},{12,38,68,102,148,161,-1},
    {7,39,69,81,103,113,144},{13,40,70,87,101,122,155},{14,41,58,105,122,158,-1},
    {0,32,71,105,106,156,-1},{15,42,72,107,140,159,-1},{16,36,73,80,108,130,153},
    {10,43,74,109,120,165,-1},{44,54,63,110,129,160,172},{7,45,70,111,118,165,-1},
    {17,35,75,88,112,113,142},{18,37,76,103,115,162,-1},{19,46,69,91,137,164,-1},
    {1,47,73,112,127,159,-1},{20,44,77,82,116,120,150},{21,46,57,117,126,163,-1},
    {15,38,61,111,133,157,-1},{22,42,78,119,130,144,-1},{18,34,58,72,109,124,160},
    {19,35,62,93,135,160,-1},{13,30,78,97,131,163,-1},{2,43,79,123,126,168,-1},
    {18,45,80,116,134,166,-1},{6,48,57,89,99,104,167},{11,49,60,117,118,143,-1},
    {12,50,63,113,117,156,-1},{23,51,75,128,147,148,-1},{24,52,68,89,100,129,155},
    {19,45,64,79,119,139,169},{20,53,76,99,139,170,-1},{34,81,132,141,170,173,-1},
    {13,29,82,112,124,169,-1},{3,28,67,119,133,172,-1},{0,3,51,56,85,135,151},
    {25,50,55,90,121,136,167},{51,83,109,114,144,167,-1},{6,49,80,98,131,172,-1},
    {22,54,66,94,171,173,-1},{25,40,76,108,140,147,-1},{1,26,40,60,61,114,132},
    {26,39,55,123,124,125,-1},{17,48,54,123,140,166,-1},{5,32,84,107,115,155,-1},
    {27,47,69,84,104,128,157},{8,53,62,130,146,154,-1},{21,52,67,108,120,173,-1},
    {2,12,47,77,94,122,-1},{30,68,132,149,154,168,-1},{11,42,65,88,96,134,158},
    {4,38,74,101,135,166,-1},{1,53,85,100,134,163,-1},{14,55,86,107,118,170,-1},
    {9,43,81,90,110,143,148},{22,33,70,93,126,152,-1},{10,48,87,91,141,156,-1},
    {28,33,86,96,146,161,-1},{29,49,59,85,136,141,161},{9,52,65,83,111,127,164},
    {21,56,84,92,139,158,-1},{27,31,71,102,131,165,-1},{27,28,83,87,116,142,149},
    {0,25,44,79,127,146,-1},{16,26,88,102,115,152,-1},{50,56,97,162,164,171,-1},
    {20,36,72,137,151,168,-1},{15,46,75,129,136,153,-1},{2,23,29,71,103,138,-1},
    {8,39,89,105,133,150,-1},{14,57,59,73,110,149,162},{17,41,78,143,145,151,-1},
    {24,37,64,98,121,159,-1},{16,41,74,128,169,171,-1}
};

static const int8_t nrw[83] = {
    7,6,6,6,7,6,7,6,6,7,6,6,7,7,6,6,6,7,6,7,6,7,6,6,6,7,6,6,6,7,6,6,6,6,7,
    6,6,6,7,7,6,6,6,6,7,7,6,6,6,6,7,6,6,6,7,6,6,6,6,7,6,6,6,7,6,6,6,7,7,6,
    6,7,6,6,6,6,6,6,6,7,6,6,6
};
// clang-format on

/******************************************************************************/
// Encoder
/******************************************************************************/

void encode174_91(const int8_t *msg77, int8_t *codeword)
{
    initGen();

    // ---- Step 1: pack 77 bits + 3 zeros into 10 bytes (big-endian, MSB first)
    uint8_t bytes[12] = {};                     // 12 bytes, zero-initialised
    for (int k = 0; k < 77; ++k) {
        if (msg77[k] & 1) {
            int byte_idx = k / 8;
            int bit_idx  = 7 - (k % 8);        // MSB of each byte first
            bytes[byte_idx] |= (uint8_t)(1u << bit_idx);
        }
    }
    // bytes[9] may have partial content from bit 72..76; bits 77-79 are 0.
    // bytes[10..11] stay 0 (the 16 zero padding bits the Fortran appends).

    // ---- Step 2: compute CRC-14 over the 12 bytes
    uint16_t crc = crc14(bytes, 12);            // polynomial 0x2757

    // ---- Step 3: build message91[0..90]
    //   bits 0..76 = msg77  (the encoded payload already)
    //   bits 77..90 = CRC14 (14 bits, MSB first)
    static int8_t message91[91];
    for (int k = 0; k < 77; ++k)
        message91[k] = msg77[k] & 1;
    for (int k = 0; k < 14; ++k)
        message91[77 + k] = (crc >> (13 - k)) & 1;

    // ---- Step 4: compute 83 parity-check bits via generator matrix
    static int8_t parity[83];
    for (int i = 0; i < 83; ++i) {
        int sum = 0;
        for (int j = 0; j < 91; ++j)
            sum += s_gen[i][j] & message91[j];
        parity[i] = sum & 1;
    }

    // ---- Step 5: assemble codeword = [message91 || parity]
    for (int k = 0;  k < 91; ++k) codeword[k]      = message91[k];
    for (int k = 0;  k < 83; ++k) codeword[91 + k] = parity[k];
}

/******************************************************************************/
// Belief-propagation LDPC decoder
//
// Faithful C++ port of decode174_91.f90 from the subspace-mode project.
// Supports BP-only (maxosd < 0) with 30 iterations and early stopping.
/******************************************************************************/

// Piecewise-linear approximation to atanh(x) — Fortran platanh
static float platanh(float x)
{
    float z = (x < 0.0f) ? -x : x;
    float y;
    if      (z <= 0.664f)  y = x / 0.83f;
    else if (z <= 0.9217f) y = (z - 0.4064f)  / 0.322f;
    else if (z <= 0.9951f) y = (z - 0.8378f)  / 0.0524f;
    else if (z <= 0.9998f) y = (z - 0.9914f)  / 0.0012f;
    else                   y = 7.0f;
    return (x < 0.0f) ? -y : y;
}

bool decode174_91_bp(const float *llr, int8_t *message91, int8_t *cw,
                     int *nharderror)
{
    static constexpr int N   = 174;
    static constexpr int K   = 91;
    static constexpr int M   = 83;
    static constexpr int NCW = 3;       // checks per variable
    static constexpr int MAX_ITER = 30;

    // Messages: check→variable (tov) and variable→check (toc)
    float tov[NCW][N] = {};             // variable-to-check
    float toc[7][M]   = {};             // check-to-variable
    float tanhtoc[7][M] = {};
    float zn[N];
    float zsum[N] = {};

    // Initialise toc from LLRs
    for (int j = 0; j < M; ++j) {
        for (int i = 0; i < nrw[j]; ++i)
            toc[i][j] = llr[Nm[j][i]];
    }

    int ncnt  = 0;
    int nclast = 0;

    for (int iter = 0; iter <= MAX_ITER; ++iter) {

        // --- Update variable-node LLRs ---
        for (int i = 0; i < N; ++i) {
            zn[i] = llr[i];
            for (int k = 0; k < NCW; ++k)
                zn[i] += tov[k][i];
            zsum[i] += zn[i];
        }

        // --- Hard decision ---
        for (int i = 0; i < N; ++i)
            cw[i] = (zn[i] > 0.0f) ? 1 : 0;

        // --- Syndrome check ---
        int ncheck = 0;
        for (int j = 0; j < M; ++j) {
            int s = 0;
            for (int k = 0; k < nrw[j]; ++k)
                s ^= cw[Nm[j][k]];
            if (s) ++ncheck;
        }

        if (ncheck == 0) {
            // Valid codeword — check CRC14
            // Reconstruct the 12-byte buffer from cw[0..76] + 3 zeros + cw[77..90] + 2 zero bytes
            // (Mirrors the Fortran m96 construction: m96[0:76]=cw[0:76], m96[82:95]=cw[77:90])
            uint8_t m96[12] = {};
            for (int k = 0; k < 77; ++k) {
                if (cw[k]) {
                    int byte_idx = k / 8;
                    int bit_idx  = 7 - (k % 8);
                    m96[byte_idx] |= (uint8_t)(1u << bit_idx);
                }
            }
            // CRC14 bits occupy bits 82..95 of m96 (0-based) → bytes 10..11
            for (int k = 0; k < 14; ++k) {
                int abs_bit = 82 + k;
                if (cw[77 + k]) {
                    int byte_idx = abs_bit / 8;
                    int bit_idx  = 7 - (abs_bit % 8);
                    m96[byte_idx] |= (uint8_t)(1u << bit_idx);
                }
            }
            if (crc14_check(m96, 12)) {
                for (int k = 0; k < K; ++k) message91[k] = cw[k];
                // Count hard errors vs channel decisions
                int nerr = 0;
                for (int i = 0; i < N; ++i)
                    if ((llr[i] >= 0.0f ? 1 : 0) != cw[i]) ++nerr;
                *nharderror = nerr;
                return true;
            }
        }

        // --- Early stopping ---
        if (iter > 0) {
            int nd = ncheck - nclast;
            if (nd < 0) ncnt = 0; else ++ncnt;
            if (ncnt >= 5 && iter >= 10 && ncheck > 15) {
                *nharderror = -1;
                return false;
            }
        }
        nclast = ncheck;

        if (iter == MAX_ITER) break;

        // --- Variable→check messages ---
        for (int j = 0; j < M; ++j) {
            for (int i = 0; i < nrw[j]; ++i) {
                int ibj = Nm[j][i];
                float t = zn[ibj];
                // subtract own contribution
                for (int kk = 0; kk < NCW; ++kk) {
                    if (Mn[ibj][kk] == j) {
                        t -= tov[kk][ibj];
                        break;
                    }
                }
                toc[i][j] = t;
            }
        }

        // --- Check→variable messages (tanh product rule) ---
        for (int j = 0; j < M; ++j)
            for (int i = 0; i < nrw[j]; ++i)
                tanhtoc[i][j] = std::tanh(-toc[i][j] / 2.0f);

        for (int ibj = 0; ibj < N; ++ibj) {
            for (int kk = 0; kk < NCW; ++kk) {
                int ichk = Mn[ibj][kk];
                float prod = 1.0f;
                for (int i = 0; i < nrw[ichk]; ++i) {
                    if (Nm[ichk][i] != ibj)
                        prod *= tanhtoc[i][ichk];
                }
                tov[kk][ibj] = 2.0f * platanh(-prod);
            }
        }
    }

    *nharderror = -1;
    return false;
}

} // namespace FT2
#endif // JS8_ENABLE_FT2
