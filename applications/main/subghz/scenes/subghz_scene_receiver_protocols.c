#include "../subghz_i.h"

#define TAG "SubGhzSceneReceiverProtocols"

enum SubGhzProtocolsIndex {
    SubGhzProtocolsIndexEnableAll,
    SubGhzProtocolsIndexDisableAll,
    SubGhzProtocolsIndexFirstProtocol,
};

#define PROTOCOL_TOGGLE_COUNT 2
#define PROTOCOL_ROWS_MAX     (UINT8_MAX + 1 - SubGhzProtocolsIndexFirstProtocol)

static const char* const protocol_toggle_text[PROTOCOL_TOGGLE_COUNT] = {
    "OFF",
    "ON",
};

typedef struct {
    const SubGhzProtocol* protocol;
    VariableItem* item;
} SubGhzProtocolsRow;

static SubGhzProtocolsRow* subghz_scene_receiver_protocols_rows = NULL;
static size_t subghz_scene_receiver_protocols_row_count = 0;

static const SubGhzProtocolsRow* subghz_scene_receiver_protocols_row(uint32_t list_index) {
    if(list_index < SubGhzProtocolsIndexFirstProtocol) return NULL;
    const size_t row = list_index - SubGhzProtocolsIndexFirstProtocol;
    if(row >= subghz_scene_receiver_protocols_row_count) return NULL;
    return &subghz_scene_receiver_protocols_rows[row];
}

static void subghz_scene_receiver_protocols_show(VariableItem* item, bool enabled) {
    variable_item_set_current_value_index(item, enabled);
    variable_item_set_current_value_text(item, protocol_toggle_text[enabled]);
}

static void subghz_scene_receiver_protocols_set(VariableItem* item) {
    SubGhz* subghz = variable_item_get_context(item);
    const bool enabled = variable_item_get_current_value_index(item) == 1;

    const SubGhzProtocolsRow* row = subghz_scene_receiver_protocols_row(
        variable_item_list_get_selected_item_index(subghz->protocol_list));
    if(!row) return;

    variable_item_set_current_value_text(item, protocol_toggle_text[enabled]);
    subghz_txrx_set_protocol_enabled(subghz->txrx, row->protocol->name, enabled);
}

static void subghz_scene_receiver_protocols_set_all(SubGhz* subghz, bool enabled) {
    FuriString* list = furi_string_alloc();
    for(size_t i = 0; i < subghz_scene_receiver_protocols_row_count; i++) {
        const SubGhzProtocolsRow* row = &subghz_scene_receiver_protocols_rows[i];
        if(!enabled) {
            if(!furi_string_empty(list)) furi_string_push_back(list, ',');
            furi_string_cat_str(list, row->protocol->name);
        }
        subghz_scene_receiver_protocols_show(row->item, enabled);
    }
    subghz_txrx_set_disabled_protocols(subghz->txrx, furi_string_get_cstr(list));
    furi_string_free(list);
}

static void subghz_scene_receiver_protocols_enter_callback(void* context, uint32_t index) {
    furi_assert(context);
    SubGhz* subghz = context;

    if(index == SubGhzProtocolsIndexEnableAll) {
        subghz_scene_receiver_protocols_set_all(subghz, true);
    } else if(index == SubGhzProtocolsIndexDisableAll) {
        subghz_scene_receiver_protocols_set_all(subghz, false);
    } else {
        const SubGhzProtocolsRow* row = subghz_scene_receiver_protocols_row(index);
        if(!row) return;
        const bool enabled = !subghz_txrx_is_protocol_enabled(subghz->txrx, row->protocol->name);
        subghz_scene_receiver_protocols_show(row->item, enabled);
        subghz_txrx_set_protocol_enabled(subghz->txrx, row->protocol->name, enabled);
    }
}

void subghz_scene_receiver_protocols_on_enter(void* context) {
    SubGhz* subghz = context;
    VariableItemList* list = subghz->protocol_list;

    const SubGhzProtocolRegistry* registry =
        subghz_environment_get_protocol_registry(subghz_txrx_get_environment(subghz->txrx));
    size_t total = subghz_txrx_count_toggleable_protocols(subghz->txrx, NULL);
    if(total > PROTOCOL_ROWS_MAX) total = PROTOCOL_ROWS_MAX;
    subghz_scene_receiver_protocols_rows = malloc(total * sizeof(SubGhzProtocolsRow));
    subghz_scene_receiver_protocols_row_count = 0;

    variable_item_list_add(list, "Enable all", 1, NULL, NULL);
    variable_item_list_add(list, "Disable all", 1, NULL, NULL);

    for(size_t i = 0; i < subghz_protocol_registry_count(registry) &&
                      subghz_scene_receiver_protocols_row_count < total;
        i++) {
        const SubGhzProtocol* protocol = subghz_protocol_registry_get_by_index(registry, i);
        if(!subghz_txrx_protocol_is_toggleable(protocol)) continue;

        VariableItem* item = variable_item_list_add(
            list, protocol->name, PROTOCOL_TOGGLE_COUNT, subghz_scene_receiver_protocols_set, subghz);
        subghz_scene_receiver_protocols_show(
            item, subghz_txrx_is_protocol_enabled(subghz->txrx, protocol->name));

        SubGhzProtocolsRow* row =
            &subghz_scene_receiver_protocols_rows[subghz_scene_receiver_protocols_row_count++];
        row->protocol = protocol;
        row->item = item;
    }

    variable_item_list_set_enter_callback(
        list, subghz_scene_receiver_protocols_enter_callback, subghz);
    variable_item_list_set_selected_item(list, 0);
    view_dispatcher_switch_to_view(subghz->view_dispatcher, SubGhzViewIdProtocolList);
}

bool subghz_scene_receiver_protocols_on_event(void* context, SceneManagerEvent event) {
    UNUSED(context);
    UNUSED(event);
    return false;
}

void subghz_scene_receiver_protocols_on_exit(void* context) {
    SubGhz* subghz = context;
    variable_item_list_set_selected_item(subghz->protocol_list, 0);
    variable_item_list_reset(subghz->protocol_list);

    free(subghz_scene_receiver_protocols_rows);
    subghz_scene_receiver_protocols_rows = NULL;
    subghz_scene_receiver_protocols_row_count = 0;

    // Persisted by the Receiver Config scene when it is left
    furi_string_set(
        subghz->last_settings->disabled_protocols,
        subghz_txrx_get_disabled_protocols(subghz->txrx));
}
