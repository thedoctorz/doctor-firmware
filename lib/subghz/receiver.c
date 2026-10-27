#include "receiver.h"

#include "registry.h"

#include <m-array.h>

typedef struct {
    SubGhzProtocolEncoderBase* base;
    bool disabled;
    bool dispatched;
} SubGhzReceiverSlot;

typedef struct {
    void* context;
    SubGhzDecoderFeed feed;
} SubGhzReceiverDispatch;

ARRAY_DEF(SubGhzReceiverSlotArray, SubGhzReceiverSlot, M_POD_OPLIST); //-V658
#define M_OPL_SubGhzReceiverSlotArray_t() ARRAY_OPLIST(SubGhzReceiverSlotArray, M_POD_OPLIST)

struct SubGhzReceiver {
    SubGhzReceiverSlotArray_t slots;
    SubGhzProtocolFlag filter;

    SubGhzProtocolFlag modulation;
    SubGhzProtocolFlag band;

    SubGhzReceiverDispatch* dispatch;
    size_t dispatch_count;
    bool dispatch_dirty;

    SubGhzReceiverCallback callback;
    void* context;
};

SubGhzReceiver* subghz_receiver_alloc_init(SubGhzEnvironment* environment) {
    return subghz_receiver_alloc_init_ex(environment, 0);
}

SubGhzReceiver* subghz_receiver_alloc_init_ex(
    SubGhzEnvironment* environment,
    SubGhzProtocolFlag exclude_flags) {
    SubGhzReceiver* instance = malloc(sizeof(SubGhzReceiver));
    SubGhzReceiverSlotArray_init(instance->slots);
    const SubGhzProtocolRegistry* protocol_registry_items =
        subghz_environment_get_protocol_registry(environment);

    for(size_t i = 0; i < subghz_protocol_registry_count(protocol_registry_items); ++i) {
        const SubGhzProtocol* protocol =
            subghz_protocol_registry_get_by_index(protocol_registry_items, i);

        if(exclude_flags && (protocol->flag & exclude_flags) != 0) {
            continue;
        }

        if(protocol->decoder && protocol->decoder->alloc) {
            SubGhzReceiverSlot* slot = SubGhzReceiverSlotArray_push_new(instance->slots);
            slot->base = protocol->decoder->alloc(environment);
            slot->disabled = false;
            slot->dispatched = false;
        }
    }

    instance->callback = NULL;
    instance->context = NULL;
    instance->modulation = SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM;
    instance->band = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868;
    instance->filter = 0;
    instance->dispatch =
        malloc(sizeof(SubGhzReceiverDispatch) * SubGhzReceiverSlotArray_size(instance->slots));
    instance->dispatch_count = 0;
    instance->dispatch_dirty = true;
    return instance;
}

void subghz_receiver_free(SubGhzReceiver* instance) {
    furi_check(instance);

    instance->callback = NULL;
    instance->context = NULL;

    // Release allocated slots
    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            slot->base->protocol->decoder->free(slot->base);
            slot->base = NULL;
        }
    SubGhzReceiverSlotArray_clear(instance->slots);
    free(instance->dispatch);

    free(instance);
}

static void subghz_receiver_update_dispatch(SubGhzReceiver* instance) {
    FURI_CRITICAL_ENTER();
    const SubGhzProtocolFlag filter = instance->filter;
    const SubGhzProtocolFlag modulation = instance->modulation;
    const SubGhzProtocolFlag band = instance->band;
    instance->dispatch_dirty = false;
    FURI_CRITICAL_EXIT();

    instance->dispatch_count = 0;
    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            const bool dispatch = !slot->disabled &&
                                  (slot->base->protocol->flag & filter) != 0 &&
                                  (slot->base->protocol->flag & modulation) != 0 &&
                                  (slot->base->protocol->flag & band) != 0;
            if(dispatch) {
                if(!slot->dispatched) slot->base->protocol->decoder->reset(slot->base);
                SubGhzReceiverDispatch* entry = &instance->dispatch[instance->dispatch_count++];
                entry->context = slot->base;
                entry->feed = slot->base->protocol->decoder->feed;
            }
            slot->dispatched = dispatch;
        }
}

void subghz_receiver_decode(SubGhzReceiver* instance, bool level, uint32_t duration) {
    furi_check(instance);
    if(instance->dispatch_dirty) subghz_receiver_update_dispatch(instance);
    for(size_t i = 0; i < instance->dispatch_count; i++) {
        const SubGhzReceiverDispatch entry = instance->dispatch[i];
        entry.feed(entry.context, level, duration);
    }
}

void subghz_receiver_reset(SubGhzReceiver* instance) {
    furi_check(instance);
    furi_check(instance->slots);

    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            slot->base->protocol->decoder->reset(slot->base);
        }
}

static void subghz_receiver_rx_callback(SubGhzProtocolDecoderBase* decoder_base, void* context) {
    SubGhzReceiver* instance = context;
    if(instance->callback) {
        const FuriThreadPriority priority = furi_thread_get_current_priority();
        if(priority > FuriThreadPriorityNormal) {
            furi_thread_set_current_priority(FuriThreadPriorityNormal);
        }
        instance->callback(instance, decoder_base, instance->context);
        if(priority > FuriThreadPriorityNormal) {
            furi_thread_set_current_priority(priority);
        }
    }
}

void subghz_receiver_set_rx_callback(
    SubGhzReceiver* instance,
    SubGhzReceiverCallback callback,
    void* context) {
    furi_check(instance);

    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            subghz_protocol_decoder_base_set_decoder_callback(
                (SubGhzProtocolDecoderBase*)slot->base, subghz_receiver_rx_callback, instance);
        }

    instance->callback = callback;
    instance->context = context;
}

void subghz_receiver_set_filter(SubGhzReceiver* instance, SubGhzProtocolFlag filter) {
    furi_check(instance);
    instance->filter = filter;
    instance->dispatch_dirty = true;
}

void subghz_receiver_set_modulation(SubGhzReceiver* instance, SubGhzProtocolFlag modulation) {
    furi_check(instance);

    modulation &= SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM;
    if(modulation == 0) {
        modulation = SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM;
    }

    instance->modulation = modulation;
    instance->dispatch_dirty = true;
}

void subghz_receiver_set_band(SubGhzReceiver* instance, SubGhzProtocolFlag band) {
    furi_check(instance);

    band &= SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868;
    if(band == 0) {
        band = SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868;
    }
    instance->band = band;
    instance->dispatch_dirty = true;
}

bool subghz_receiver_set_protocol_enabled(
    SubGhzReceiver* instance,
    const char* protocol_name,
    bool enabled) {
    furi_check(instance);
    furi_check(protocol_name);

    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            if(strcmp(slot->base->protocol->name, protocol_name) == 0) {
                if(slot->disabled == enabled) {
                    slot->disabled = !enabled;
                    instance->dispatch_dirty = true;
                }
                return true;
            }
        }
    return false;
}

void subghz_receiver_set_gate(SubGhzReceiver* instance, SubGhzProtocolFlag gate) {
    furi_check(instance);

    subghz_receiver_set_modulation(instance, gate);
    subghz_receiver_set_band(instance, gate);
}

SubGhzProtocolDecoderBase* subghz_receiver_search_decoder_base_by_name(
    SubGhzReceiver* instance,
    const char* decoder_name) {
    furi_check(instance);

    SubGhzProtocolDecoderBase* result = NULL;

    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            if(strcmp(slot->base->protocol->name, decoder_name) == 0) {
                result = (SubGhzProtocolDecoderBase*)slot->base;
                break;
            }
        }
    return result;
}
