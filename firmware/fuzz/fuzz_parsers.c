/* libFuzzer entry for the wire-facing parsers: Solana message, EVM RLP transaction, Bitcoin PSBT, the JSON
 * request line (which carries EIP-712 typed data) and the update image header. The first byte picks the
 * parser so one corpus covers all of them. Built with -DBUILD_FUZZERS=ON (clang); FUZZ_STANDALONE adds a
 * main() that replays files, for toolchains without libFuzzer. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "btcpsbt.h"
#include "evmtx.h"
#include "proto.h"
#include "solmsg.h"
#include "update.h"

static const uint8_t OWNER32[32] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb,
                                     0xcc, 0xdd, 0xee, 0xff, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
                                     0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x12};
static const uint8_t OWNER20[20] = {0xde, 0xad, 0xbe, 0xef, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                     0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};
static const uint8_t OWNER33[33] = {0x02, 0x79, 0xbe, 0x66, 0x7e, 0xf9, 0xdc, 0xbb, 0xac, 0x55, 0xa0,
                                     0x62, 0x95, 0xce, 0x87, 0x0b, 0x07, 0x02, 0x9b, 0xfc, 0xdb, 0x2d,
                                     0xce, 0x28, 0xd9, 0x59, 0xf2, 0x81, 0x5b, 0x16, 0xf8, 0x17, 0x98};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static solmsg_t msg;
    static solmsg_summary_t sum;
    static evmtx_t evm;
    static btcpsbt_t psbt;
    static proto_req_t req;
    static char line[PROTO_LINE_MAX];
    update_image_info_t info;
    if (size == 0) return 0;
    switch (data[0] % 5) {
    case 0:
        if (solmsg_parse(data + 1, size - 1, &msg) == SOLMSG_OK) solmsg_analyze(&msg, OWNER32, &sum);
        break;
    case 1:
        if (evmtx_parse(data + 1, size - 1, &evm) == EVMTX_OK) evmtx_analyze(&evm, OWNER20);
        break;
    case 2:
        if (btcpsbt_parse(data + 1, size - 1, &psbt) == BTCPSBT_OK) btcpsbt_analyze(&psbt, OWNER33);
        break;
    case 3:
        if (size - 1 >= sizeof line) return 0;
        memcpy(line, data + 1, size - 1);
        line[size - 1] = 0;
        proto_parse_request(line, &req);
        break;
    default:
        update_inspect_image(data + 1, size - 1, &info);
        break;
    }
    return 0;
}

#ifdef FUZZ_STANDALONE
#include <stdio.h>
int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        uint8_t *buf;
        long n;
        if (!f) continue;
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        buf = malloc(n > 0 ? (size_t)n : 1);
        if (buf && n > 0 && fread(buf, 1, (size_t)n, f) == (size_t)n) LLVMFuzzerTestOneInput(buf, (size_t)n);
        free(buf);
        fclose(f);
    }
    return 0;
}
#endif
