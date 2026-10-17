#include <furi.h>
#include <furi_hal.h>
#include <lib/toolbox/args.h>
#include <toolbox/pipe.h>
#include <cli/cli_main_commands.h>
#include <toolbox/cli/cli_registry.h>

#include <ble/ble.h>
#include <gap_scan.h>
#include <gap_central.h>
#include "bt_settings.h"
#include "bt_service/bt.h"
#include <profiles/serial_profile.h>

static void bt_cli_command_hci_info(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    UNUSED(args);
    UNUSED(context);
    FuriString* buffer;
    buffer = furi_string_alloc();
    furi_hal_bt_dump_state(buffer);
    printf("%s", furi_string_get_cstr(buffer));
    furi_string_free(buffer);
}
#define BT_CLI_SCAN_QUEUE_LEN 16
#define BT_CLI_SCAN_SEEN_MAX  64

static void bt_cli_scan_callback(const GapScanReport* report, void* context) {
    FuriMessageQueue* queue = context;
    furi_message_queue_put(queue, report, 0);
}

typedef struct {
    uint8_t address[6];
    bool named;
} BtCliScanSeen;

static int32_t
    bt_cli_scan_find(const BtCliScanSeen* seen, size_t seen_count, const uint8_t* address) {
    for(size_t i = 0; i < seen_count; i++) {
        if(memcmp(seen[i].address, address, 6) == 0) return (int32_t)i;
    }
    return -1;
}

static const char* bt_cli_scan_event_type_str(uint8_t event_type) {
    switch(event_type) {
    case 0x00:
        return "CONN";
    case 0x01:
        return "DIRECT";
    case 0x02:
        return "SCAN";
    case 0x03:
        return "NONCONN";
    case 0x04:
        return "RSP";
    default:
        return "?";
    }
}

static void bt_cli_scan_print(const GapScanReport* report) {
    printf(
        "%02X:%02X:%02X:%02X:%02X:%02X %5d %-7s %-6s %s\r\n",
        report->address[5],
        report->address[4],
        report->address[3],
        report->address[2],
        report->address[1],
        report->address[0],
        report->rssi,
        bt_cli_scan_event_type_str(report->event_type),
        report->address_type ? "random" : "public",
        report->name);
    fflush(stdout);
}

static void bt_cli_command_scan(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(args);
    UNUSED(context);

    if(furi_hal_bt_get_radio_stack() != FuriHalBtStackFull) {
        printf("Scanning needs the \"ble_full\" radio stack (GAP central role).\r\n");
        printf("The \"ble_light\" stack is peripheral-only and cannot scan.\r\n");
        return;
    }

    if(!furi_hal_bt_is_alive()) {
        printf("BLE stack is not running.\r\n");
        return;
    }

    FuriMessageQueue* queue =
        furi_message_queue_alloc(BT_CLI_SCAN_QUEUE_LEN, sizeof(GapScanReport));

    do {
        if(!gap_scan_start(bt_cli_scan_callback, queue)) {
            printf("Failed to start scanning, HCI status 0x%02X.\r\n", gap_scan_get_last_error());
            break;
        }

        printf("Scanning for BLE devices. Press CTRL+C to stop.\r\n");
        printf("%-17s %5s %-7s %-6s %s\r\n", "ADDRESS", "RSSI", "TYPE", "ADDR", "NAME");

        BtCliScanSeen seen[BT_CLI_SCAN_SEEN_MAX];
        size_t seen_count = 0;
        size_t report_count = 0;
        size_t scan_rsp_count = 0;

        while(!cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            GapScanReport report;
            if(furi_message_queue_get(queue, &report, 100) != FuriStatusOk) continue;

            report_count++;
            if(report.event_type == 0x04) scan_rsp_count++;
            bool has_name = report.name[0] != '\0';
            int32_t index = bt_cli_scan_find(seen, seen_count, report.address);

            if(index < 0) {
                if(seen_count >= BT_CLI_SCAN_SEEN_MAX) continue;
                seen[seen_count].named = has_name;
                memcpy(seen[seen_count].address, report.address, 6);
                seen_count++;
                bt_cli_scan_print(&report);
            } else if(has_name && !seen[index].named) {
                seen[index].named = true;
                bt_cli_scan_print(&report);
            }
        }

        printf(
            "\r\nFound %u device(s) from %u report(s), %u scan response(s).\r\n",
            (unsigned)seen_count,
            (unsigned)report_count,
            (unsigned)scan_rsp_count);
        if(seen_count == BT_CLI_SCAN_SEEN_MAX) {
            printf("Device list was capped at %u entries.\r\n", BT_CLI_SCAN_SEEN_MAX);
        }
    } while(false);

    gap_scan_stop();
    furi_message_queue_free(queue);
}

#define BT_CLI_MAX_SERVICES        16
#define BT_CLI_MAX_CHARACTERISTICS 16
#define BT_CLI_GATT_TIMEOUT_MS     5000
#define BT_CLI_CONNECT_TIMEOUT_MS  10000

static bool bt_cli_parse_address(FuriString* text, uint8_t* address) {
    const char* str = furi_string_get_cstr(text);
    if(strlen(str) != 17) return false;

    for(size_t i = 0; i < 6; i++) {
        if(i < 5 && str[i * 3 + 2] != ':') return false;
        char octet[3] = {str[i * 3], str[i * 3 + 1], '\0'};
        char* end = NULL;
        unsigned long value = strtoul(octet, &end, 16);
        if(end != octet + 2) return false;
        address[5 - i] = (uint8_t)value;
    }
    return true;
}

static void bt_cli_print_uuid(const uint8_t* uuid, uint8_t uuid_len) {
    for(uint8_t i = 0; i < uuid_len; i++) {
        printf("%02X", uuid[uuid_len - 1 - i]);
        if(uuid_len == 16 && (i == 3 || i == 5 || i == 7 || i == 9)) printf("-");
    }
}

static void bt_cli_print_properties(uint8_t properties) {
    static const struct {
        uint8_t mask;
        const char* name;
    } flags[] = {
        {0x02, "read"},
        {0x04, "write-nr"},
        {0x08, "write"},
        {0x10, "notify"},
        {0x20, "indicate"},
    };

    bool first = true;
    for(size_t i = 0; i < COUNT_OF(flags); i++) {
        if(!(properties & flags[i].mask)) continue;
        printf("%s%s", first ? "" : ",", flags[i].name);
        first = false;
    }
    if(first) printf("-");
}

static void bt_cli_command_connect(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(pipe);
    UNUSED(context);

    if(furi_hal_bt_get_radio_stack() != FuriHalBtStackFull) {
        printf("Connecting out needs the \"ble_full\" radio stack (GAP central role).\r\n");
        return;
    }
    if(!furi_hal_bt_is_alive()) {
        printf("BLE stack is not running.\r\n");
        return;
    }

    FuriString* address_text = furi_string_alloc();
    FuriString* type_text = furi_string_alloc();
    uint8_t address[6];

    do {
        if(!args_read_string_and_trim(args, address_text) ||
           !bt_cli_parse_address(address_text, address)) {
            printf("Usage: bt connect <AA:BB:CC:DD:EE:FF> [public|random]\r\n");
            printf("Addresses come from \"bt scan\"; most devices there are random.\r\n");
            break;
        }

        /* Scanned devices are mostly random-address, so default to that */
        uint8_t address_type = 0x01;
        if(args_read_string_and_trim(args, type_text)) {
            if(furi_string_cmp_str(type_text, "public") == 0) {
                address_type = 0x00;
            } else if(furi_string_cmp_str(type_text, "random") != 0) {
                printf("Address type must be \"public\" or \"random\".\r\n");
                break;
            }
        }

        printf("Connecting to %s...\r\n", furi_string_get_cstr(address_text));
        fflush(stdout);

        if(!gap_central_connect(address, address_type, BT_CLI_CONNECT_TIMEOUT_MS)) {
            uint8_t error = gap_central_get_last_error();
            if(error) {
                printf("Connection failed, status 0x%02X.\r\n", error);
            } else {
                printf("Connection timed out - the device never answered.\r\n");
            }
            printf("Note that a device only accepts connections while it advertises as\r\n");
            printf(
                "connectable (TYPE \"CONN\" in \"bt scan\"), and private addresses rotate.\r\n");
            break;
        }

        printf("Connected. Discovering services...\r\n");
        fflush(stdout);

        GapCentralService services[BT_CLI_MAX_SERVICES];
        int32_t service_count =
            gap_central_discover_services(services, BT_CLI_MAX_SERVICES, BT_CLI_GATT_TIMEOUT_MS);

        if(service_count < 0) {
            printf("Service discovery failed, status 0x%02X.\r\n", gap_central_get_last_error());
            break;
        }

        printf("\r\n%ld service(s):\r\n", (long)service_count);
        for(int32_t i = 0; i < service_count; i++) {
            const GapCentralService* service = &services[i];
            printf("  [0x%04X-0x%04X] ", service->start_handle, service->end_handle);
            bt_cli_print_uuid(service->uuid, service->uuid_len);
            printf("\r\n");
            fflush(stdout);

            GapCentralCharacteristic chars[BT_CLI_MAX_CHARACTERISTICS];
            int32_t char_count = gap_central_discover_characteristics(
                service, chars, BT_CLI_MAX_CHARACTERISTICS, BT_CLI_GATT_TIMEOUT_MS);

            if(char_count < 0) {
                printf("      <characteristic discovery failed>\r\n");
                continue;
            }
            for(int32_t j = 0; j < char_count; j++) {
                printf("      0x%04X ", chars[j].value_handle);
                bt_cli_print_uuid(chars[j].uuid, chars[j].uuid_len);
                printf(" [");
                bt_cli_print_properties(chars[j].properties);
                printf("]\r\n");
            }
            if(char_count == BT_CLI_MAX_CHARACTERISTICS) {
                printf("      <list capped at %d>\r\n", BT_CLI_MAX_CHARACTERISTICS);
            }
            fflush(stdout);
        }
        if(service_count == BT_CLI_MAX_SERVICES) {
            printf("<service list capped at %d>\r\n", BT_CLI_MAX_SERVICES);
        }
    } while(false);

    gap_central_disconnect();
    printf("\r\nDisconnected.\r\n");

    furi_string_free(address_text);
    furi_string_free(type_text);
}

static void bt_cli_command_carrier_tx(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    int channel = 0;
    int power = 0;

    do {
        if(!args_read_int_and_trim(args, &channel) && (channel < 0 || channel > 39)) {
            printf("Incorrect or missing channel, expected int 0-39");
            break;
        }
        if(!args_read_int_and_trim(args, &power) && (power < 0 || power > 6)) {
            printf("Incorrect or missing power, expected int 0-6");
            break;
        }

        Bt* bt = furi_record_open(RECORD_BT);
        bt_disconnect(bt);
        furi_hal_bt_reinit();
        printf("Transmitting carrier at %d channel at %d dB power\r\n", channel, power);
        printf("Press CTRL+C to stop\r\n");
        furi_hal_bt_start_tone_tx(channel, 0x19 + power);

        while(!cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            furi_delay_ms(250);
        }
        furi_hal_bt_stop_tone_tx();

        bt_profile_restore_default(bt);
        furi_record_close(RECORD_BT);
    } while(false);
}

static void bt_cli_command_carrier_rx(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    int channel = 0;

    do {
        if(!args_read_int_and_trim(args, &channel) && (channel < 0 || channel > 39)) {
            printf("Incorrect or missing channel, expected int 0-39");
            break;
        }

        Bt* bt = furi_record_open(RECORD_BT);
        bt_disconnect(bt);
        furi_hal_bt_reinit();
        printf("Receiving carrier at %d channel\r\n", channel);
        printf("Press CTRL+C to stop\r\n");

        furi_hal_bt_start_packet_rx(channel, 1);

        while(!cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            furi_delay_ms(250);
            printf("RSSI: %6.1f dB\r", (double)furi_hal_bt_get_rssi());
            fflush(stdout);
        }

        furi_hal_bt_stop_packet_test();

        bt_profile_restore_default(bt);
        furi_record_close(RECORD_BT);
    } while(false);
}

static void bt_cli_command_packet_tx(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    int channel = 0;
    int pattern = 0;
    int datarate = 1;

    do {
        if(!args_read_int_and_trim(args, &channel) && (channel < 0 || channel > 39)) {
            printf("Incorrect or missing channel, expected int 0-39");
            break;
        }
        if(!args_read_int_and_trim(args, &pattern) && (pattern < 0 || pattern > 5)) {
            printf("Incorrect or missing pattern, expected int 0-5 \r\n");
            printf("0 - Pseudo-Random bit sequence 9\r\n");
            printf("1 - Pattern of alternating bits '11110000'\r\n");
            printf("2 - Pattern of alternating bits '10101010'\r\n");
            printf("3 - Pseudo-Random bit sequence 15\r\n");
            printf("4 - Pattern of All '1' bits\r\n");
            printf("5 - Pattern of All '0' bits\r\n");
            break;
        }
        if(!args_read_int_and_trim(args, &datarate) && (datarate < 1 || datarate > 2)) {
            printf("Incorrect or missing datarate, expected int 1-2");
            break;
        }

        Bt* bt = furi_record_open(RECORD_BT);
        bt_disconnect(bt);
        furi_hal_bt_reinit();
        printf(
            "Transmitting %d pattern packet at %d channel at %d M datarate\r\n",
            pattern,
            channel,
            datarate);
        printf("Press CTRL+C to stop\r\n");
        furi_hal_bt_start_packet_tx(channel, pattern, datarate);

        while(!cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            furi_delay_ms(250);
        }
        furi_hal_bt_stop_packet_test();
        printf("Transmitted %lu packets", furi_hal_bt_get_transmitted_packets());

        bt_profile_restore_default(bt);
        furi_record_close(RECORD_BT);
    } while(false);
}

static void bt_cli_command_packet_rx(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    int channel = 0;
    int datarate = 1;

    do {
        if(!args_read_int_and_trim(args, &channel) && (channel < 0 || channel > 39)) {
            printf("Incorrect or missing channel, expected int 0-39");
            break;
        }
        if(!args_read_int_and_trim(args, &datarate) && (datarate < 1 || datarate > 2)) {
            printf("Incorrect or missing datarate, expected int 1-2");
            break;
        }

        Bt* bt = furi_record_open(RECORD_BT);
        bt_disconnect(bt);
        furi_hal_bt_reinit();
        printf("Receiving packets at %d channel at %d M datarate\r\n", channel, datarate);
        printf("Press CTRL+C to stop\r\n");
        furi_hal_bt_start_packet_rx(channel, datarate);

        while(!cli_is_pipe_broken_or_is_etx_next_char(pipe)) {
            furi_delay_ms(250);
            printf("RSSI: %03.1f dB\r", (double)furi_hal_bt_get_rssi());
            fflush(stdout);
        }
        uint16_t packets_received = furi_hal_bt_stop_packet_test();
        printf("Received %hu packets", packets_received);

        bt_profile_restore_default(bt);
        furi_record_close(RECORD_BT);
    } while(false);
}

static void bt_cli_print_usage(void) {
    printf("Usage:\r\n");
    printf("bt <cmd> <args>\r\n");
    printf("Cmd list:\r\n");
    printf("\thci_info\t - HCI info\r\n");
    printf("\tscan\t - scan for nearby BLE devices (needs ble_full stack)\r\n");
    printf("\tconnect <addr> [public|random]\t - connect and list GATT services\r\n");
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) && furi_hal_bt_is_testing_supported()) {
        printf("\ttx_carrier <channel:0-39> <power:0-6>\t - start tx carrier test\r\n");
        printf("\trx_carrier <channel:0-39>\t - start rx carrier test\r\n");
        printf(
            "\ttx_packet <channel:0-39> <pattern:0-5> <datarate:1-2>\t - start tx packet test\r\n");
        printf("\trx_packet <channel:0-39> <datarate:1-2>\t - start rx packer test\r\n");
    }
}

static void bt_cli(PipeSide* pipe, FuriString* args, void* context) {
    UNUSED(context);
    furi_record_open(RECORD_BT);

    FuriString* cmd;
    cmd = furi_string_alloc();
    BtSettings bt_settings;
    bt_settings_load(&bt_settings);

    do {
        if(!args_read_string_and_trim(args, cmd)) {
            bt_cli_print_usage();
            break;
        }
        if(furi_string_cmp_str(cmd, "hci_info") == 0) {
            bt_cli_command_hci_info(pipe, args, NULL);
            break;
        }
        if(furi_string_cmp_str(cmd, "scan") == 0) {
            bt_cli_command_scan(pipe, args, NULL);
            break;
        }
        if(furi_string_cmp_str(cmd, "connect") == 0) {
            bt_cli_command_connect(pipe, args, NULL);
            break;
        }
        if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagDebug) && furi_hal_bt_is_testing_supported()) {
            if(furi_string_cmp_str(cmd, "tx_carrier") == 0) {
                bt_cli_command_carrier_tx(pipe, args, NULL);
                break;
            }
            if(furi_string_cmp_str(cmd, "rx_carrier") == 0) {
                bt_cli_command_carrier_rx(pipe, args, NULL);
                break;
            }
            if(furi_string_cmp_str(cmd, "tx_packet") == 0) {
                bt_cli_command_packet_tx(pipe, args, NULL);
                break;
            }
            if(furi_string_cmp_str(cmd, "rx_packet") == 0) {
                bt_cli_command_packet_rx(pipe, args, NULL);
                break;
            }
        }

        bt_cli_print_usage();
    } while(false);

    if(bt_settings.enabled) {
        furi_hal_bt_start_advertising();
    }

    furi_string_free(cmd);
    furi_record_close(RECORD_BT);
}

CLI_COMMAND_INTERFACE(bt, bt_cli, CliCommandFlagDefault, 4096, CLI_APPID);
