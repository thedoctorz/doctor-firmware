#include "receiver.h"

#include "registry.h"
#include "protocols/bin_raw.h"

#include <m-array.h>

typedef struct {
    union {
        SubGhzProtocolEncoderBase* base;
        const SubGhzProtocol* protocol;
    };
    bool allocated;
    bool pinned;
    bool disabled;
    bool dispatched;
} SubGhzReceiverSlot;

_Static_assert(sizeof(SubGhzReceiverSlot) == 2 * sizeof(void*), "Keep receiver slots compact");

static const SubGhzProtocol* subghz_receiver_slot_protocol(const SubGhzReceiverSlot* slot) {
    return slot->allocated ? slot->base->protocol : slot->protocol;
}

typedef struct {
    void* context;
    SubGhzDecoderFeed feed;
} SubGhzReceiverDispatch;

ARRAY_DEF(SubGhzReceiverSlotArray, SubGhzReceiverSlot, M_POD_OPLIST); //-V658
#define M_OPL_SubGhzReceiverSlotArray_t() ARRAY_OPLIST(SubGhzReceiverSlotArray, M_POD_OPLIST)

struct SubGhzReceiver {
    SubGhzReceiverSlotArray_t slots;
    SubGhzEnvironment* environment;
    SubGhzProtocolFlag filter;
    SubGhzProtocolDecoderBinRAW* bin_raw;

    SubGhzProtocolFlag modulation;
    SubGhzProtocolFlag band;

    SubGhzReceiverDispatch* dispatch;
    size_t dispatch_count;
    bool dispatch_dirty;

    SubGhzReceiverCallback callback;
    void* context;
};

static void subghz_receiver_rx_callback(SubGhzProtocolDecoderBase* decoder_base, void* context);
static SubGhzReceiver* subghz_receiver_alloc_internal(
    SubGhzEnvironment* environment,
    SubGhzProtocolFlag exclude_flags,
    bool deferred);

SubGhzReceiver* subghz_receiver_alloc_init_session(SubGhzEnvironment* environment) {
    return subghz_receiver_alloc_internal(environment, 0, true);
}

SubGhzReceiver* subghz_receiver_alloc_init(SubGhzEnvironment* environment) {
    return subghz_receiver_alloc_init_ex(environment, 0);
}

SubGhzReceiver* subghz_receiver_alloc_init_ex(
    SubGhzEnvironment* environment,
    SubGhzProtocolFlag exclude_flags) {
    return subghz_receiver_alloc_internal(environment, exclude_flags, false);
}

static SubGhzReceiver* subghz_receiver_alloc_internal(
    SubGhzEnvironment* environment,
    SubGhzProtocolFlag exclude_flags,
    bool deferred) {
    SubGhzReceiver* instance = malloc(sizeof(SubGhzReceiver));
    instance->environment = environment;
    instance->bin_raw = NULL;
    SubGhzReceiverSlotArray_init(instance->slots);
    const SubGhzProtocolRegistry* protocol_registry_items =
        subghz_environment_get_protocol_registry(environment);

    size_t slot_count = 0;
    for(size_t i = 0; i < subghz_protocol_registry_count(protocol_registry_items); ++i) {
        const SubGhzProtocol* protocol =
            subghz_protocol_registry_get_by_index(protocol_registry_items, i);
        if(!(protocol->flag & exclude_flags) && protocol->decoder && protocol->decoder->alloc)
            slot_count++;
    }
    SubGhzReceiverSlotArray_reserve(instance->slots, slot_count);

    for(size_t i = 0; i < subghz_protocol_registry_count(protocol_registry_items); ++i) {
        const SubGhzProtocol* protocol =
            subghz_protocol_registry_get_by_index(protocol_registry_items, i);

        if(exclude_flags && (protocol->flag & exclude_flags) != 0) {
            continue;
        }

        if(protocol->decoder && protocol->decoder->alloc) {
            SubGhzReceiverSlot* slot = SubGhzReceiverSlotArray_push_new(instance->slots);
            slot->protocol = protocol;
            slot->pinned = false;
            if(!deferred) slot->base = protocol->decoder->alloc(environment);
            slot->allocated = !deferred;
            if(slot->allocated && (protocol->flag & SubGhzProtocolFlag_BinRAW))
                instance->bin_raw = (void*)slot->base;
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

// Only call with the decoder worker stopped. A lookup pins the returned instance,
// preserving the historical pointer lifetime for file loading, editing and TX.
void subghz_receiver_prepare_session(SubGhzReceiver* instance, SubGhzProtocolFlag gate) {
    furi_check(instance);
    const SubGhzProtocolFlag mod = gate & (SubGhzProtocolFlag_AM | SubGhzProtocolFlag_FM);
    const SubGhzProtocolFlag band =
        gate & (SubGhzProtocolFlag_315 | SubGhzProtocolFlag_433 | SubGhzProtocolFlag_868);
    instance->dispatch_count = 0;
    if(instance->bin_raw && !(instance->filter & SubGhzProtocolFlag_BinRAW))
        subghz_protocol_decoder_bin_raw_release(instance->bin_raw);
    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            const bool bin_raw_off =
                (subghz_receiver_slot_protocol(slot)->flag & SubGhzProtocolFlag_BinRAW) &&
                !(instance->filter & SubGhzProtocolFlag_BinRAW);
            const bool needed = slot->pinned ||
                                (!bin_raw_off && !slot->disabled &&
                                 (!mod || (subghz_receiver_slot_protocol(slot)->flag & mod)) &&
                                 (!band || (subghz_receiver_slot_protocol(slot)->flag & band)));
            if(!needed && slot->allocated) {
                const SubGhzProtocol* protocol = subghz_receiver_slot_protocol(slot);
                if(protocol->flag & SubGhzProtocolFlag_BinRAW) instance->bin_raw = NULL;
                protocol->decoder->free(slot->base);
                slot->protocol = protocol;
                slot->allocated = false;
            }
            slot->dispatched = false;
        }
    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            if(!slot->allocated && !slot->disabled &&
               (!(subghz_receiver_slot_protocol(slot)->flag & SubGhzProtocolFlag_BinRAW) ||
                (instance->filter & SubGhzProtocolFlag_BinRAW)) &&
               (!mod || (subghz_receiver_slot_protocol(slot)->flag & mod)) &&
               (!band || (subghz_receiver_slot_protocol(slot)->flag & band))) {
                slot->base =
                    subghz_receiver_slot_protocol(slot)->decoder->alloc(instance->environment);
                slot->allocated = true;
                if(subghz_receiver_slot_protocol(slot)->flag & SubGhzProtocolFlag_BinRAW)
                    instance->bin_raw = (void*)slot->base;
                subghz_protocol_decoder_base_set_decoder_callback(
                    (SubGhzProtocolDecoderBase*)slot->base, subghz_receiver_rx_callback, instance);
            }
        }
    instance->dispatch_dirty = true;
}

void subghz_receiver_free(SubGhzReceiver* instance) {
    furi_check(instance);

    instance->callback = NULL;
    instance->context = NULL;

    for
        M_EACH(slot, instance->slots, SubGhzReceiverSlotArray_t) {
            if(slot->allocated) subghz_receiver_slot_protocol(slot)->decoder->free(slot->base);
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
            const bool dispatch = slot->allocated && !slot->disabled &&
                                  (subghz_receiver_slot_protocol(slot)->flag & filter) != 0 &&
                                  (subghz_receiver_slot_protocol(slot)->flag & modulation) != 0 &&
                                  (subghz_receiver_slot_protocol(slot)->flag & band) != 0;
            if(dispatch) {
                if(!slot->dispatched)
                    subghz_receiver_slot_protocol(slot)->decoder->reset(slot->base);
                SubGhzReceiverDispatch* entry = &instance->dispatch[instance->dispatch_count++];
                entry->context = slot->base;
                entry->feed = subghz_receiver_slot_protocol(slot)->decoder->feed;
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
            if(slot->allocated) subghz_receiver_slot_protocol(slot)->decoder->reset(slot->base);
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
            if(!slot->allocated) continue;
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
            if(strcmp(subghz_receiver_slot_protocol(slot)->name, protocol_name) == 0) {
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
            if(strcmp(subghz_receiver_slot_protocol(slot)->name, decoder_name) == 0) {
                if(!slot->allocated) {
                    slot->base =
                        subghz_receiver_slot_protocol(slot)->decoder->alloc(instance->environment);
                    slot->allocated = true;
                    if(subghz_receiver_slot_protocol(slot)->flag & SubGhzProtocolFlag_BinRAW)
                        instance->bin_raw = (void*)slot->base;
                    subghz_protocol_decoder_base_set_decoder_callback(
                        (SubGhzProtocolDecoderBase*)slot->base,
                        subghz_receiver_rx_callback,
                        instance);
                    instance->dispatch_dirty = true;
                }
                slot->pinned = true;
                result = (SubGhzProtocolDecoderBase*)slot->base;
                break;
            }
        }
    return result;
}

void subghz_receiver_decode_rssi(SubGhzReceiver* instance, float rssi) {
    if(instance->bin_raw && (instance->filter & SubGhzProtocolFlag_BinRAW)) {
        if(instance->dispatch_dirty) subghz_receiver_update_dispatch(instance);
        subghz_protocol_decoder_bin_raw_data_input_rssi(instance->bin_raw, rssi);
    }
}
