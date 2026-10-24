// Prints every temperature (T*) and fan (F*) SMC key with its type and value.
#include "smc.h"
#include <stdio.h>

int main(void) {
    if (smc_open() != KERN_SUCCESS) { fprintf(stderr, "cannot open AppleSMC\n"); return 1; }
    uint32_t n = smc_key_count();
    printf("keys: %u\n", n);
    for (uint32_t i = 0; i < n; i++) {
        char key[5];
        if (smc_key_at(i, key) != KERN_SUCCESS) continue;
        if (key[0] != 'T' && key[0] != 'F') continue;
        smc_val_t v;
        if (smc_read(key, &v) != KERN_SUCCESS) continue;
        double d;
        if (smc_to_double(&v, &d)) printf("%s %s %.2f\n", key, v.type, d);
        else printf("%s %s (size %u)\n", key, v.type, v.size);
    }
    smc_close();
    return 0;
}
