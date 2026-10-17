#pragma once

#include "../subghz_i.h"

bool subghz_psa_bf_needs_bruteforce(SubGhz* subghz);
bool subghz_psa_bf_start(SubGhz* subghz);
bool subghz_psa_bf_is_running(void);
void subghz_psa_bf_cancel(SubGhz* subghz);
bool subghz_psa_bf_handle_tick(SubGhz* subghz);
bool subghz_psa_bf_handle_back(void);
void subghz_psa_bf_on_exit(void);
