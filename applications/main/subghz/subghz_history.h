
#pragma once

#include <math.h>
#include <furi.h>
#include <furi_hal.h>
#include <lib/flipper_format/flipper_format.h>
#include <lib/subghz/types.h>
#include "helpers/subghz_types.h"

typedef struct SubGhzHistory SubGhzHistory;

/** Allocate SubGhzHistory
 * 
 * @return SubGhzHistory* 
 */
SubGhzHistory* subghz_history_alloc(void);

/** Free SubGhzHistory
 * 
 * @param instance - SubGhzHistory instance
 */
void subghz_history_free(SubGhzHistory* instance);

/** Clear history
 * 
 * @param instance - SubGhzHistory instance
 */
void subghz_history_reset(SubGhzHistory* instance);

void subghz_history_delete_item(SubGhzHistory* instance, uint16_t idx);

/** Get frequency to history[idx]
 * 
 * @param instance  - SubGhzHistory instance
 * @param idx       - record index  
 * @return frequency - frequency Hz
 */
uint32_t subghz_history_get_frequency(SubGhzHistory* instance, uint16_t idx);

SubGhzRadioPreset* subghz_history_get_radio_preset(SubGhzHistory* instance, uint16_t idx);

/** Get preset to history[idx]
 * 
 * @param instance  - SubGhzHistory instance
 * @param idx       - record index  
 * @return preset   - preset name
 */
const char* subghz_history_get_preset(SubGhzHistory* instance, uint16_t idx);

/** Get history index write 
 * 
 * @param instance  - SubGhzHistory instance
 * @return idx      - current record index  
 */
uint16_t subghz_history_get_item(SubGhzHistory* instance);

/** Get type protocol to history[idx]
 * 
 * @param instance  - SubGhzHistory instance
 * @param idx       - record index  
 * @return type      - type protocol  
 */
uint8_t subghz_history_get_type_protocol(SubGhzHistory* instance, uint16_t idx);

const SubGhzProtocol* subghz_history_get_protocol(SubGhzHistory* instance, uint16_t idx);

SubGhzHistoryButtonIcon subghz_history_get_button_icon(SubGhzHistory* instance, uint16_t idx);

/** Get name protocol to history[idx]
 * 
 * @param instance  - SubGhzHistory instance
 * @param idx       - record index  
 * @return name      - const char* name protocol  
 */
const char* subghz_history_get_protocol_name(SubGhzHistory* instance, uint16_t idx);

/** Get datetime from history[idx]
 * 
 * @param instance  - SubGhzHistory instance
 * @param idx       - record index  
 * @return datetime - DateTime received timestamp  
 */
DateTime subghz_history_get_datetime(SubGhzHistory* instance, uint16_t idx);

/** Get string item menu to history[idx]
 * 
 * @param instance  - SubGhzHistory instance
 * @param output    - FuriString* output
 * @param idx       - record index
 */
void subghz_history_get_text_item_menu(SubGhzHistory* instance, FuriString* output, uint16_t idx);

/** Get time item menu to history[idx]
 * 
 * @param instance  - SubGhzHistory instance
 * @param output    - FuriString* output
 * @param idx       - record index
 */
void subghz_history_get_time_item_menu(SubGhzHistory* instance, FuriString* output, uint16_t idx);

/** Get string the remaining number of records to history
 * 
 * @param instance  - SubGhzHistory instance
 * @param output    - FuriString* output
 * @return bool - is FULL
 */
bool subghz_history_get_text_space_left(SubGhzHistory* instance, FuriString* output);

/** Return last index
 *
 * @param instance - SubGhzHistory instance
 * @return
 */
uint16_t subghz_history_get_last_index(SubGhzHistory* instance);

/** Set the GPS location tagged
 *
 * When valid, the coordinates are written as "Lat"/"Lon" keys into each new
 * item so that saving the signal geotags the resulting .sub file.
 *
 * @param instance   - SubGhzHistory instance
 * @param valid      - whether a usable fix is available
 * @param latitude   - latitude in decimal degrees
 * @param longitude  - longitude in decimal degrees
 */
void subghz_history_set_location(
    SubGhzHistory* instance,
    bool valid,
    float latitude,
    float longitude);

/** Add protocol to history
 *
 * @param instance  - SubGhzHistory instance
 * @param context    - SubGhzProtocolCommon context
 * @param preset    - SubGhzRadioPreset preset
 * @return bool;
 */
bool subghz_history_add_to_history(
    SubGhzHistory* instance,
    void* context,
    SubGhzRadioPreset* preset);

/** Add protocol to history tagging the radio source (dual RX)
 *
 * @param instance - SubGhzHistory instance
 * @param context  - SubGhzProtocolCommon context
 * @param preset   - SubGhzRadioPreset preset
 * @param source   - radio source marker (0 = internal, 1 = external)
 * @return bool;
 */
bool subghz_history_add_to_history_source(
    SubGhzHistory* instance,
    void* context,
    SubGhzRadioPreset* preset,
    uint8_t source);

/** Get the radio source marker of history[idx] (0 = internal, 1 = external)
 *
 * @param instance - SubGhzHistory instance
 * @param idx      - record index
 * @return uint8_t source marker
 */
uint8_t subghz_history_get_source(SubGhzHistory* instance, uint16_t idx);

/** Get SubGhzProtocolCommonLoad to load into the protocol decoder bin data
 * 
 * @param instance  - SubGhzHistory instance
 * @param idx       - record index
 * @return SubGhzProtocolCommonLoad*
 */
FlipperFormat* subghz_history_get_raw_data(SubGhzHistory* instance, uint16_t idx);
FlipperFormat* subghz_history_get_raw_data_copy(SubGhzHistory* instance, uint16_t idx);
