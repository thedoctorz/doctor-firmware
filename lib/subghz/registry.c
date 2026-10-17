#include "registry.h"

#include <furi.h>
#include <string.h>

typedef struct {
    const char* alias;
    const char* canonical;
} SubGhzProtocolNameAlias;

static const SubGhzProtocolNameAlias subghz_protocol_name_aliases[] = {
    {"Hitag-2", "Renault V1"},
    {"Renault", "Renault V0"},
    {"StarLine", "Star Line"},
    {"ScherKhan", "Scher-Khan"},
    {"HondaCVE", "Honda Static"},
    {"Korea v0", "Kia V0"},
    {"Korea V0", "Kia V0"},
    {"Suzuki", "Kia V0"},
    {"Suzuki V0", "Kia V0"},
    {"Honda v0", "Kia V0"},
    {"Honda V0", "Kia V0"},
    {"Mitsu v0", "Kia V0"},
    {"Mitsu V0", "Kia V0"},
    {"Korea v1", "Kia V1"},
    {"Korea V1", "Kia V1"},
    {"Korea v2", "Kia V2"},
    {"Korea V2", "Kia V2"},
    {"Korea v3v4", "Kia V3/V4"},
    {"Korea v3", "Kia V3/V4"},
    {"Korea v4", "Kia V3/V4"},
    {"Korea V3", "Kia V3/V4"},
    {"Korea V4", "Kia V3/V4"},
    {"Kia V3", "Kia V3/V4"},
    {"Kia V4", "Kia V3/V4"},
    {"Korea v5", "Kia V5"},
    {"Korea V5", "Kia V5"},
    {"Korea v6", "Kia V6"},
    {"Korea V6", "Kia V6"},
    {"Korea v7", "Kia V7"},
    {"Korea V7", "Kia V7"},
    {"Ford v0", "Ford V0"},
    {"Mazda v0", "Mazda V0"},
    {"Mazda v2", "Mazda V2"},
    {"Fiat v0", "Fiat V0"},
    {"Chrysler", "Chrysler V0"},
    {"Came", "CAME"},
    {"Came Twee", "CAME TWEE"},
    {"Came Atomo", "CAME Atomo"},
    {"ChamCode", "Cham_Code"},
    {"HT12X", "Holtek_HT12X"},
    {"Dickert", "Dickert_MAHS"},
    {"Intertechno v3", "Intertechno_V3"},
    {"V2 Phoenix", "Phoenix_V2"},
    {"Revers RB2", "Revers_RB2"},
    {"Alutech AT4N", "Alutech AT-4N"},
    {"Nice Flo", "Nice FLO"},
    {"Bett", "BETT"},
    {"Nord Ice", "Nord ICE"},
    {"Telcoma/Cardin Edge", "Telcoma/Cardin EDGE"},
    {"Ford v1", "Ford V1"},
    {"Honda v1", "Honda V1"},
    {"Passat", "VAG"},
    {"VAG v1", "VAG"},
    {"VAG v2", "VAG"},
    {"Audi", "VAG"},
    {"Land Rover V0", "Honda V2"},
};

const char* subghz_protocol_name_canonical(const char* name) {
    if(!name || name[0] == '\0') {
        return name;
    }
    for(size_t i = 0; i < COUNT_OF(subghz_protocol_name_aliases); i++) {
        if(strcmp(name, subghz_protocol_name_aliases[i].alias) == 0) {
            return subghz_protocol_name_aliases[i].canonical;
        }
    }
    return name;
}

bool subghz_protocol_names_match(const char* a, const char* b) {
    if(!a || !b) {
        return false;
    }
    if(strcmp(a, b) == 0) {
        return true;
    }
    return strcmp(subghz_protocol_name_canonical(a), subghz_protocol_name_canonical(b)) == 0;
}

const SubGhzProtocol* subghz_protocol_registry_get_by_name(
    const SubGhzProtocolRegistry* protocol_registry,
    const char* name) {
    furi_check(protocol_registry);
    const char* lookup = subghz_protocol_name_canonical(name);

    for(size_t i = 0; i < subghz_protocol_registry_count(protocol_registry); i++) {
        if(lookup && strcmp(lookup, protocol_registry->items[i]->name) == 0) {
            return protocol_registry->items[i];
        }
    }
    return NULL;
}

const SubGhzProtocol* subghz_protocol_registry_get_by_index(
    const SubGhzProtocolRegistry* protocol_registry,
    size_t index) {
    furi_check(protocol_registry);
    if(index < subghz_protocol_registry_count(protocol_registry)) {
        return protocol_registry->items[index];
    } else {
        return NULL;
    }
}

size_t subghz_protocol_registry_count(const SubGhzProtocolRegistry* protocol_registry) {
    furi_check(protocol_registry);
    return protocol_registry->size;
}
