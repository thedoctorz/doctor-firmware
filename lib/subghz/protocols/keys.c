#include "keys.h"
#include <lib/subghz/subghz_keystore.h>
#include <lib/subghz/environment.h>

#define KIA_KEY1 10u
#define KIA_KEY2 11u
#define KIA_KEY3 12u
#define KIA_KEY4 13u

uint64_t kia_mf_key = 0;
uint64_t kia_v6_a_key = 0;
uint64_t kia_v6_b_key = 0;
uint64_t kia_v5_key = 0;

void kia_keys_load(SubGhzEnvironment* environment) {
    SubGhzKeystore* keystore = subghz_environment_get_keystore(environment);
    for
        M_EACH(manufacture_code, *subghz_keystore_get_data(keystore), SubGhzKeyArray_t) {
            switch(manufacture_code->type) {
            case KIA_KEY1:
                kia_mf_key = manufacture_code->key;
                break;
            case KIA_KEY2:
                kia_v6_a_key = manufacture_code->key;
                break;
            case KIA_KEY3:
                kia_v6_b_key = manufacture_code->key;
                break;
            case KIA_KEY4:
                kia_v5_key = manufacture_code->key;
                break;
            default:
                break;
            }
        }
}

uint64_t get_kia_mf_key(void) {
    return kia_mf_key;
}

uint64_t get_kia_v6_keystore_a(void) {
    return kia_v6_a_key;
}

uint64_t get_kia_v6_keystore_b(void) {
    return kia_v6_b_key;
}

uint64_t get_kia_v5_key(void) {
    return kia_v5_key;
}
