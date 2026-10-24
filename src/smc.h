// Minimal AppleSMC access: read/write keys, decode common value types.
#pragma once
#include <IOKit/IOKitLib.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    char key[5];
    char type[5];
    uint32_t size;
    uint8_t bytes[32];
} smc_val_t;

kern_return_t smc_open(void);
void smc_close(void);
kern_return_t smc_read(const char *key, smc_val_t *out);
kern_return_t smc_write(const char *key, const uint8_t *bytes, uint32_t size);
kern_return_t smc_key_at(uint32_t index, char out[5]);
uint32_t smc_key_count(void);

// Decodes numeric types (sp78, flt, fpe2, ui8/16/32, ...). Returns false if unknown.
bool smc_to_double(const smc_val_t *v, double *out);
// Encodes value into the key's own type. Returns false if unsupported.
bool smc_from_double(const smc_val_t *like, double value, uint8_t out[32]);
