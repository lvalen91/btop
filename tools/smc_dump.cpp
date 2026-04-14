// One-shot diagnostic: enumerate all SMC keys starting with 'T' and print their
// dataType, size, and decoded/raw value. Built against the same IOKit SMC API
// as btop's src/osx/smc.cpp.
//
// Build:   clang++ -std=c++20 -O2 -framework IOKit -framework CoreFoundation \
//            tools/smc_dump.cpp -o bin/smc_dump
// Run:     ./bin/smc_dump

#include <IOKit/IOKitLib.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

typedef struct {
    char major;
    char minor;
    char build;
    char reserved[1];
    uint16_t release;
} SMCKeyData_vers_t;

typedef struct {
    uint16_t version;
    uint16_t length;
    uint32_t cpuPLimit;
    uint32_t gpuPLimit;
    uint32_t memPLimit;
} SMCKeyData_pLimitData_t;

typedef struct {
    uint32_t dataSize;
    uint32_t dataType;
    char dataAttributes;
} SMCKeyData_keyInfo_t;

typedef char SMCBytes_t[32];

typedef struct {
    uint32_t key;
    SMCKeyData_vers_t vers;
    SMCKeyData_pLimitData_t pLimitData;
    SMCKeyData_keyInfo_t keyInfo;
    char result;
    char status;
    char data8;
    uint32_t data32;
    SMCBytes_t bytes;
} SMCKeyData_t;

#define KERNEL_INDEX_SMC        2
#define SMC_CMD_READ_BYTES      5
#define SMC_CMD_READ_INDEX      8
#define SMC_CMD_READ_KEYINFO    9

static io_connect_t gConn = 0;

static uint32_t fourcc(const char *s) {
    return ((uint32_t)s[0] << 24) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 8) | (uint32_t)s[3];
}

static void unfourcc(uint32_t v, char out[5]) {
    out[0] = (v >> 24) & 0xff;
    out[1] = (v >> 16) & 0xff;
    out[2] = (v >> 8) & 0xff;
    out[3] = v & 0xff;
    out[4] = 0;
}

static kern_return_t smc_call(int index, SMCKeyData_t *in, SMCKeyData_t *out) {
    size_t inSize = sizeof(*in);
    size_t outSize = sizeof(*out);
    return IOConnectCallStructMethod(gConn, index, in, inSize, out, &outSize);
}

static bool read_key_info(uint32_t key, SMCKeyData_keyInfo_t *info) {
    SMCKeyData_t in{}, out{};
    in.key = key;
    in.data8 = SMC_CMD_READ_KEYINFO;
    if (smc_call(KERNEL_INDEX_SMC, &in, &out) != KERN_SUCCESS) return false;
    *info = out.keyInfo;
    return true;
}

static bool read_bytes(uint32_t key, uint32_t size, uint8_t *buf) {
    SMCKeyData_t in{}, out{};
    in.key = key;
    in.data8 = SMC_CMD_READ_BYTES;
    in.keyInfo.dataSize = size;
    if (smc_call(KERNEL_INDEX_SMC, &in, &out) != KERN_SUCCESS) return false;
    memcpy(buf, out.bytes, size > 32 ? 32 : size);
    return true;
}

static std::string decode(uint32_t type, uint32_t size, const uint8_t *b) {
    char t[5]; unfourcc(type, t);
    char buf[128];
    if (strcmp(t, "sp78") == 0 && size >= 2) {
        int16_t v = (int16_t)((b[0] << 8) | b[1]);
        snprintf(buf, sizeof(buf), "%.2f °C", v / 256.0);
        return buf;
    }
    if ((strcmp(t, "flt ") == 0 || strcmp(t, "fp2e") == 0) && size >= 4) {
        float f; memcpy(&f, b, 4);
        snprintf(buf, sizeof(buf), "%.2f", f);
        return buf;
    }
    if ((strcmp(t, "ui8 ") == 0 || strcmp(t, "ui8") == 0) && size >= 1) {
        snprintf(buf, sizeof(buf), "%u", b[0]);
        return buf;
    }
    if (strcmp(t, "ui16") == 0 && size >= 2) {
        snprintf(buf, sizeof(buf), "%u", (b[0] << 8) | b[1]);
        return buf;
    }
    if (strcmp(t, "ui32") == 0 && size >= 4) {
        uint32_t v = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
        snprintf(buf, sizeof(buf), "%u", v);
        return buf;
    }
    std::string hex = "0x";
    for (uint32_t i = 0; i < size && i < 16; i++) {
        char x[4]; snprintf(x, sizeof(x), "%02x", b[i]);
        hex += x;
    }
    return hex;
}

int main(int argc, char **argv) {
    char prefix = (argc > 1 && argv[1][0]) ? argv[1][0] : 'T';
    CFMutableDictionaryRef match = IOServiceMatching("AppleSMC");
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, match);
    if (!svc) { fprintf(stderr, "no AppleSMC service\n"); return 1; }
    if (IOServiceOpen(svc, mach_task_self(), 0, &gConn) != KERN_SUCCESS) {
        fprintf(stderr, "IOServiceOpen failed\n");
        IOObjectRelease(svc);
        return 1;
    }
    IOObjectRelease(svc);

    // Read total key count from "#KEY"
    SMCKeyData_keyInfo_t info{};
    if (!read_key_info(fourcc("#KEY"), &info)) { fprintf(stderr, "#KEY info failed\n"); return 1; }
    uint8_t buf[32]{};
    if (!read_bytes(fourcc("#KEY"), info.dataSize, buf)) { fprintf(stderr, "#KEY read failed\n"); return 1; }
    uint32_t total = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) | ((uint32_t)buf[2] << 8) | buf[3];
    fprintf(stderr, "SMC reports %u total keys\n", total);

    int shown = 0;
    for (uint32_t i = 0; i < total; i++) {
        SMCKeyData_t in{}, out{};
        in.data8 = SMC_CMD_READ_INDEX;
        in.data32 = i;
        if (smc_call(KERNEL_INDEX_SMC, &in, &out) != KERN_SUCCESS) continue;
        char name[5]; unfourcc(out.key, name);
        if (name[0] != prefix) continue;

        SMCKeyData_keyInfo_t ki{};
        if (!read_key_info(out.key, &ki)) continue;
        char ty[5]; unfourcc(ki.dataType, ty);

        uint8_t vb[32]{};
        std::string val = "?";
        if (ki.dataSize > 0 && ki.dataSize <= 32 && read_bytes(out.key, ki.dataSize, vb)) {
            val = decode(ki.dataType, ki.dataSize, vb);
        }
        printf("%-4s  type=%-4s  size=%-2u  value=%s\n", name, ty, ki.dataSize, val.c_str());
        shown++;
    }
    fprintf(stderr, "shown: %d T* keys\n", shown);

    IOServiceClose(gConn);
    return 0;
}
