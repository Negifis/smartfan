#include "smc.h"
#include <string.h>

#define KERNEL_INDEX_SMC 2
#define SMC_CMD_READ_BYTES 5
#define SMC_CMD_WRITE_BYTES 6
#define SMC_CMD_READ_INDEX 8
#define SMC_CMD_READ_KEYINFO 9

typedef struct { char major, minor, build, reserved; uint16_t release; } smc_vers_t;
typedef struct { uint16_t version, length; uint32_t cpu, gpu, mem; } smc_plimit_t;
typedef struct { uint32_t dataSize; uint32_t dataType; char dataAttributes; } smc_keyinfo_t;
typedef struct {
    uint32_t key;
    smc_vers_t vers;
    smc_plimit_t pLimitData;
    smc_keyinfo_t keyInfo;
    char result, status, data8;
    uint32_t data32;
    uint8_t bytes[32];
} smc_data_t;

static io_connect_t conn;

static uint32_t str_to_u32(const char *s) {
    return ((uint32_t)(uint8_t)s[0] << 24) | ((uint32_t)(uint8_t)s[1] << 16) |
           ((uint32_t)(uint8_t)s[2] << 8) | (uint32_t)(uint8_t)s[3];
}

static void u32_to_str(uint32_t v, char out[5]) {
    out[0] = v >> 24; out[1] = v >> 16; out[2] = v >> 8; out[3] = v; out[4] = 0;
}

static kern_return_t call(smc_data_t *in, smc_data_t *out) {
    size_t sz = sizeof(*out);
    return IOConnectCallStructMethod(conn, KERNEL_INDEX_SMC, in, sizeof(*in), out, &sz);
}

kern_return_t smc_open(void) {
    io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("AppleSMC"));
    if (!svc) return kIOReturnNotFound;
    kern_return_t r = IOServiceOpen(svc, mach_task_self(), 0, &conn);
    IOObjectRelease(svc);
    return r;
}

void smc_close(void) {
    if (conn) IOServiceClose(conn);
    conn = 0;
}

static kern_return_t keyinfo(uint32_t key, smc_keyinfo_t *info) {
    smc_data_t in = {0}, out = {0};
    in.key = key;
    in.data8 = SMC_CMD_READ_KEYINFO;
    kern_return_t r = call(&in, &out);
    if (r != KERN_SUCCESS) return r;
    if (out.result != 0) return kIOReturnNotFound;
    *info = out.keyInfo;
    return KERN_SUCCESS;
}

kern_return_t smc_read(const char *key, smc_val_t *v) {
    memset(v, 0, sizeof(*v));
    strncpy(v->key, key, 4);
    uint32_t k = str_to_u32(key);
    smc_keyinfo_t info;
    kern_return_t r = keyinfo(k, &info);
    if (r != KERN_SUCCESS) return r;
    smc_data_t in = {0}, out = {0};
    in.key = k;
    in.keyInfo.dataSize = info.dataSize;
    in.data8 = SMC_CMD_READ_BYTES;
    r = call(&in, &out);
    if (r != KERN_SUCCESS) return r;
    if (out.result != 0) return kIOReturnError;
    v->size = info.dataSize > 32 ? 32 : info.dataSize;
    u32_to_str(info.dataType, v->type);
    memcpy(v->bytes, out.bytes, v->size);
    return KERN_SUCCESS;
}

kern_return_t smc_write(const char *key, const uint8_t *bytes, uint32_t size) {
    smc_data_t in = {0}, out = {0};
    in.key = str_to_u32(key);
    in.data8 = SMC_CMD_WRITE_BYTES;
    in.keyInfo.dataSize = size;
    memcpy(in.bytes, bytes, size);
    kern_return_t r = call(&in, &out);
    if (r != KERN_SUCCESS) return r;
    return out.result == 0 ? KERN_SUCCESS : kIOReturnNotPrivileged;
}

kern_return_t smc_key_at(uint32_t index, char out_key[5]) {
    smc_data_t in = {0}, out = {0};
    in.data8 = SMC_CMD_READ_INDEX;
    in.data32 = index;
    kern_return_t r = call(&in, &out);
    if (r != KERN_SUCCESS) return r;
    u32_to_str(out.key, out_key);
    return KERN_SUCCESS;
}

uint32_t smc_key_count(void) {
    smc_val_t v;
    if (smc_read("#KEY", &v) != KERN_SUCCESS) return 0;
    return str_to_u32((const char *)v.bytes);
}

bool smc_to_double(const smc_val_t *v, double *out) {
    const uint8_t *b = v->bytes;
    if (!strcmp(v->type, "sp78") && v->size == 2) { *out = (int16_t)((b[0] << 8) | b[1]) / 256.0; return true; }
    if (!strcmp(v->type, "fpe2") && v->size == 2) { *out = ((b[0] << 8) | b[1]) / 4.0; return true; }
    if (!strcmp(v->type, "flt ") && v->size == 4) { float f; memcpy(&f, b, 4); *out = f; return true; }
    if (!strcmp(v->type, "ui8 ") && v->size == 1) { *out = b[0]; return true; }
    if (!strcmp(v->type, "ui16") && v->size == 2) { *out = (b[0] << 8) | b[1]; return true; }
    if (!strcmp(v->type, "ui32") && v->size == 4) { *out = str_to_u32((const char *)b); return true; }
    return false;
}

bool smc_from_double(const smc_val_t *like, double value, uint8_t out[32]) {
    memset(out, 0, 32);
    if (!strcmp(like->type, "flt ")) { float f = (float)value; memcpy(out, &f, 4); return true; }
    if (!strcmp(like->type, "fpe2")) { uint16_t u = (uint16_t)(value * 4); out[0] = u >> 8; out[1] = u; return true; }
    if (!strcmp(like->type, "ui8 ")) { out[0] = (uint8_t)value; return true; }
    if (!strcmp(like->type, "ui16")) { uint16_t u = (uint16_t)value; out[0] = u >> 8; out[1] = u; return true; }
    return false;
}
