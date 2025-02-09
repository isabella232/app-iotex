/*******************************************************************************
*   (c) 2016 Ledger
*   (c) 2018 ZondaX GmbH
*   (c) 2019 IoTeX Foundation
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/

#include "app_main.h"

#include <string.h>
#include <os_io_seproxyhal.h>
#include <os.h>
#include <ux.h>

#include <zxmacros.h>
#include <bech32.h>

#include "lib/transaction.h"
#include "lib/tx_display.h"
#include "view.h"
#include "crypto.h"
#include "settings.h"
#include "pb_parser.h"
#include "lib/biginteger.h"

#ifdef TESTING_ENABLED
// Generate using always the same private data
// to allow for reproducible results
const uint8_t privateKeyDataTest[] = {
        0x75, 0x56, 0x0e, 0x4d, 0xde, 0xa0, 0x63, 0x05,
        0xc3, 0x6e, 0x2e, 0xb5, 0xf7, 0x2a, 0xca, 0x71,
        0x2d, 0x13, 0x4c, 0xc2, 0xa0, 0x59, 0xbf, 0xe8,
        0x7e, 0x9b, 0x5d, 0x55, 0xbf, 0x81, 0x3b, 0xd4
};
#endif

const nvm_settings_t N_nvm_settings;
static const char SIGN_MAGIC[] = "\x16IoTeX Signed Message:\n";
unsigned char G_io_seproxyhal_spi_buffer[IO_SEPROXYHAL_BUFFER_SIZE_B];

unsigned char io_event(unsigned char channel) {
    UNUSED(channel);

    switch (G_io_seproxyhal_spi_buffer[0]) {
        case SEPROXYHAL_TAG_FINGER_EVENT: //
            UX_FINGER_EVENT(G_io_seproxyhal_spi_buffer);
            break;

        case SEPROXYHAL_TAG_BUTTON_PUSH_EVENT: // for Nano S
            UX_BUTTON_PUSH_EVENT(G_io_seproxyhal_spi_buffer);
            break;

        case SEPROXYHAL_TAG_DISPLAY_PROCESSED_EVENT:
            if (!UX_DISPLAYED())
                UX_DISPLAYED_EVENT();
            break;

        case SEPROXYHAL_TAG_TICKER_EVENT: { //
            UX_TICKER_EVENT(G_io_seproxyhal_spi_buffer, {
                    if (UX_ALLOWED) {
                        UX_REDISPLAY();
                    }
            });
            break;
        }

            // unknown events are acknowledged
        default:
            UX_DEFAULT_EVENT();
            break;
    }
    if (!io_seproxyhal_spi_is_status_sent()) {
        io_seproxyhal_general_status();
    }
    return 1; // DO NOT reset the current APDU transport
}

unsigned short io_exchange_al(unsigned char channel, unsigned short tx_len) {
    switch (channel & ~(IO_FLAGS)) {
        case CHANNEL_KEYBOARD:
            break;

            // multiplexed io exchange over a SPI channel and TLV encapsulated protocol
        case CHANNEL_SPI:
            if (tx_len) {
                io_seproxyhal_spi_send(G_io_apdu_buffer, tx_len);

                if (channel & IO_RESET_AFTER_REPLIED) {
                    reset();
                }
                return 0; // nothing received from the master so far (it's a tx
                // transaction)
            } else {
                return io_seproxyhal_spi_recv(G_io_apdu_buffer, sizeof(G_io_apdu_buffer), 0);
            }

        default:
            THROW(INVALID_PARAMETER);
    }
    return 0;
}

bool extractBip32(uint8_t *depth, uint32_t path[10], uint32_t rx, uint32_t offset) {
    if (rx < offset + 1) {
        return false;
    }

    *depth = G_io_apdu_buffer[offset];
    const uint16_t req_offset = 4 * *depth + 1 + offset;

    if (rx < req_offset || *depth > 10) {
        return false;
    }

    memcpy(path, G_io_apdu_buffer + offset + 1, *depth * 4);
    return true;
}

static bool validateIoTexPath(uint8_t depth, uint32_t path[10]) {
    // Only paths in the form 44'/304'/{account}'/0/{index} are supported
    if (depth != 5) {
        return false;
    }
    if (path[0] != 0x8000002c || path[1] != 0x80000130 || path[3] != 0) {
        return false;
    }
    return true;
}

static bool extractHRP(uint8_t *len, char *hrp, size_t rx, size_t offset) {
    if (rx < offset + 1) {
        return false;
    }

    *len = G_io_apdu_buffer[offset];

    if (*len == 0 || *len > MAX_BECH32_HRP_LEN) {
        return false;
    }

    if (rx < offset + 1 + *len) {
        return false;
    }

    memcpy(hrp, G_io_apdu_buffer + offset + 1, *len);
    hrp[*len] = 0; // zero terminate
    return true;
}

bool process_chunk(volatile uint32_t *tx, uint32_t rx, bool getBip32) {
    UNUSED(tx);

    int packageIndex = G_io_apdu_buffer[OFFSET_PCK_INDEX];
    int packageCount = G_io_apdu_buffer[OFFSET_PCK_COUNT];

    uint16_t offset = OFFSET_DATA;
    if (rx < offset) {
        THROW(APDU_CODE_DATA_INVALID);
    }

    if (packageIndex == 1) {
        transaction_initialize();
        transaction_reset();
        if (getBip32) {
            if (!extractBip32(&bip32_depth, bip32_path, rx, OFFSET_DATA)) {
                THROW(APDU_CODE_DATA_INVALID);
            }
            return packageIndex == packageCount;
        }
    }

    if (transaction_append(&(G_io_apdu_buffer[offset]), rx - offset) != rx - offset) {
        THROW(APDU_CODE_OUTPUT_BUFFER_TOO_SMALL);
    }

    return packageIndex == packageCount;
}

//region View Transaction Handlers
int16_t tx_getData(char *title, int16_t max_title_length,
                   char *key, int16_t max_key_length,
                   char *value, int16_t max_value_length,
                   int16_t page_index,
                   int16_t chunk_index,
                   int16_t *page_count_out,
                   int16_t *chunk_count_out) {
    /* Nonos max length 9 chars, Nonox can be different */
    static const char action_name[][9] = {
        "INVALID",
        "Transfer",
        "Executio",
        "Create",
        "Unstake",
        "Withdraw",
        "Add Depo",
        "Restake",
        "ChgCandi",
        "TxOwners",
        "CandiReg",
        "CdUpdate",
    };

    *page_count_out = tx_display_num_pages();
    *chunk_count_out = 1;

    if (*page_count_out > 0) {
        int action_idx = tx_ctx.actiontype < ACTION_MAX_INVALID ? tx_ctx.actiontype : 0;
        snprintf(title, max_title_length, "Act:%s %02d/%02d", action_name[action_idx], page_index + 1, *page_count_out);
    
        INIT_QUERY(key, max_key_length, value, max_value_length, chunk_index)
        decode_pb(transaction_get_buffer(),transaction_get_buffer_length(),NULL,page_index);
    }

    return *chunk_count_out;
}

void tx_reject();
void tx_accept_sign() {
    // Generate keys
    cx_ecfp_public_key_t publicKey;
    cx_ecfp_private_key_t privateKey;
    uint8_t privateKeyData[32];

    if (tx_ctx.has_contract_data && !N_settings.contractDataAllowed) {
        tx_reject();
        return;
    }

    unsigned int length = 0;
    int result = 0;
    switch (current_sigtype) {
        case SECP256K1:
            os_perso_derive_node_bip32(CX_CURVE_256K1,
                                       bip32_path, bip32_depth,
                                       privateKeyData, NULL);
            keys_secp256k1(&publicKey, &privateKey, privateKeyData);
            explicit_bzero(privateKeyData, sizeof(privateKeyData));
            result = sign_secp256k1(transaction_get_buffer(),
                                    transaction_get_buffer_length(),
                                    G_io_apdu_buffer,
                                    IO_APDU_BUFFER_SIZE,
                                    &length,
                                    &privateKey);
            break;
        default:
            THROW(APDU_CODE_INS_NOT_SUPPORTED);
            break;
    }

    if (result == 1) {
        set_code(G_io_apdu_buffer, length, APDU_CODE_OK);
        io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, length + 2);
        view_idle(0);
    } else {
        set_code(G_io_apdu_buffer, length, APDU_CODE_SIGN_VERIFY_ERROR);
        io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, length + 2);
        view_idle(0);
    }
}

void tx_reject() {
    set_code(G_io_apdu_buffer, 0, APDU_CODE_COMMAND_NOT_ALLOWED);
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, 2);
    view_idle(0);
}

//endregion

//region View Address Handlers

int16_t addr_getData(char *title, int16_t max_title_length,
                     char *key, int16_t max_key_length,
                     char *value, int16_t max_value_length,
                     int16_t page_index,
                     int16_t chunk_index,
                     int16_t *page_count_out,
                     int16_t *chunk_count_out) {
    UNUSED(max_value_length); // max_value_length is always large enough to store address
    UNUSED(chunk_index);

    if (page_count_out)
        *page_count_out = 1;
    if (chunk_count_out)
        *chunk_count_out = 1;

    snprintf(title, max_title_length, "Account %d", bip32_path[2] & 0x7FFFFFF);
    snprintf(key, max_key_length, "index %d", page_index);
    bip32_path[bip32_depth - 1] = page_index;

    // get address from the current bip32_path
    get_bech32_addr(value);
    return 0;
}

void addr_accept() {
#if defined(TARGET_NANOS)
    print_key("Returning");
    print_value("Address...");
    view_status();
    UX_WAIT();
#endif
    // Send pubkey
    uint8_t *pk = G_io_apdu_buffer;
    get_pk_compressed(pk);
    int pos = PK_COMPRESSED_LEN;

    // Convert pubkey to bech32 address
    char *bech32_out = (char *) (G_io_apdu_buffer + pos);
    get_bech32_addr(bech32_out);
    pos += strlen(bech32_out);

    set_code(G_io_apdu_buffer, pos, APDU_CODE_OK);
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, pos + 2);
    view_idle(0);
}


//endregion

void addr_reject() {
    set_code(G_io_apdu_buffer, 0, APDU_CODE_COMMAND_NOT_ALLOWED);
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, 2);
    view_idle(0);
}


// region sign personal message
int16_t smsg_getData(char *title, int16_t max_title_length,
                     char *key, int16_t max_key_length,
                     char *value, int16_t max_value_length,
                     int16_t page_index,
                     int16_t chunk_index,
                     int16_t *page_count_out,
                     int16_t *chunk_count_out) {
    UNUSED(chunk_index);

    uint32_t length;
    const uint32_t max_display_length = 128;
    const uint32_t last_page_length = (transaction_get_buffer_length() * 2) % max_display_length;

    int16_t page_count = (transaction_get_buffer_length() * 2) / max_display_length + (last_page_length != 0);
    if (page_count_out) {
        *page_count_out = page_count;
    }

    if (chunk_count_out) {
        *chunk_count_out = 1;
    }

    if (page_index + 1 != page_count) {
        length = max_display_length;
    }
    else {
        length = last_page_length ? last_page_length : max_display_length;
    }

    length = length ? length : max_display_length;

    if (transaction_get_buffer_length()) {
        snprintf(title, max_title_length, "Raw Message %02d/%02d", page_index + 1, page_count);
        snprintf(key, max_key_length, "Length: %d", transaction_get_buffer_length());
        snprintf(value, max_value_length, "%.*H", length / 2, transaction_get_buffer() + page_index * max_display_length / 2);
    }
    else {
        snprintf(title, max_title_length, "Raw Message %02d/%02d", 0, page_count);
        snprintf(key, max_key_length, "Length: %d", transaction_get_buffer_length());
        snprintf(value, max_value_length, "(null empty)");
    }
  
    return 0;
}

void smsg_accept() {
    int result;
    uint32_t length;
    uint8_t sign_msg[280] = {0};
    uint8_t private_key_data[32];
    const uint32_t sign_magic_length = strlen(SIGN_MAGIC);
  
    cx_ecfp_public_key_t public_key;
    cx_ecfp_private_key_t private_key;

    /* Copy sign magic to sign message */
    memcpy(sign_msg, SIGN_MAGIC, sign_magic_length);

    /* Append byte length and byte to sign msg */
    length = bigint_u642str(transaction_get_buffer_length(),
                            (char *)(sign_msg + sign_magic_length),
                            sizeof(sign_msg) - sign_magic_length);
    memcpy(sign_msg + sign_magic_length + length, transaction_get_buffer(), transaction_get_buffer_length());


    os_perso_derive_node_bip32(CX_CURVE_256K1,
                               bip32_path, bip32_depth,
                               private_key_data, NULL);

    keys_secp256k1(&public_key, &private_key, private_key_data);
    explicit_bzero(private_key_data, sizeof(private_key_data));

    result = sign_secp256k1(sign_msg,
                            sign_magic_length + length + transaction_get_buffer_length(),
                            G_io_apdu_buffer,
                            IO_APDU_BUFFER_SIZE,
                            &length,
                            &private_key);

    set_code(G_io_apdu_buffer, length, result ? APDU_CODE_OK : APDU_CODE_SIGN_VERIFY_ERROR);
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, length + 2);
    view_idle(0);
}

void smsg_reject() {
    set_code(G_io_apdu_buffer, 0, APDU_CODE_COMMAND_NOT_ALLOWED);
    io_exchange(CHANNEL_APDU | IO_RETURN_AFTER_TX, 2);
    view_idle(0);
}

// endregion

void handleApdu(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    uint16_t sw = 0;

    BEGIN_TRY
    {
        TRY
        {
            if (G_io_apdu_buffer[OFFSET_CLA] != CLA) {
                THROW(APDU_CODE_CLA_NOT_SUPPORTED);
            }

            if (rx < 5) {
                THROW(APDU_CODE_WRONG_LENGTH);
            }

            switch (G_io_apdu_buffer[OFFSET_INS]) {
                case INS_GET_VERSION: {
#ifdef TESTING_ENABLED
                    G_io_apdu_buffer[0] = 0xFF;
#else
                    G_io_apdu_buffer[0] = 0;
#endif
                    G_io_apdu_buffer[1] = LEDGER_MAJOR_VERSION;
                    G_io_apdu_buffer[2] = LEDGER_MINOR_VERSION;
                    G_io_apdu_buffer[3] = LEDGER_PATCH_VERSION;
                    G_io_apdu_buffer[4] = !IS_UX_ALLOWED;

                    *tx += 5;
                    THROW(APDU_CODE_OK);
                    break;
                }

                    // INS_PUBLIC_KEY_SECP256K1 will be deprecated in the near future
                case INS_PUBLIC_KEY_SECP256K1: {
                    if (!extractBip32(&bip32_depth, bip32_path, rx, OFFSET_DATA)) {
                        THROW(APDU_CODE_DATA_INVALID);
                    }

                    if (!validateIoTexPath(bip32_depth, bip32_path)) {
                        THROW(APDU_CODE_DATA_INVALID);
                    }

                    cx_ecfp_public_key_t publicKey;
                    getPubKey(&publicKey);

                    memcpy(G_io_apdu_buffer, publicKey.W, 65);
                    *tx += 65;

                    THROW(APDU_CODE_OK);
                    break;
                }

                case INS_GET_ADDR_SECP256K1: {
                    // Parse arguments
                    if (!extractHRP(&bech32_hrp_len, bech32_hrp, rx, OFFSET_DATA)) {
                        THROW(APDU_CODE_DATA_INVALID);
                    }

                    if (!extractBip32(&bip32_depth, bip32_path, rx, OFFSET_DATA + bech32_hrp_len + 1)) {
                        THROW(APDU_CODE_DATA_INVALID);
                    }

                    if (!validateIoTexPath(bip32_depth, bip32_path)) {
                        THROW(APDU_CODE_DATA_INVALID);
                    }

                    view_set_handlers(addr_getData, addr_accept, addr_reject);
                    view_addr_confirm(0);

                    *flags |= IO_ASYNCH_REPLY;
                    break;
                }

                case INS_SIGN_SECP256K1: {
                    current_sigtype = SECP256K1;
                    if (!process_chunk(tx, rx, true))
                        THROW(APDU_CODE_OK);

                    int error_code = 0;
                    const char *error_msg = transaction_parse(&error_code);
                    if (error_msg != NULL) {
                        int error_msg_length = strlen(error_msg);
                        memcpy(G_io_apdu_buffer, error_msg, error_msg_length);
                        *tx += (error_msg_length);

#ifdef _DEBUG_PB_DECODE_
                        /* Lower 8bit indicate which kind of error(return from decode_pb) */
                        THROW(0x6A00 | ((-error_code) & 0xff));
#else
                        THROW(APDU_CODE_BAD_KEY_HANDLE);
#endif
                    }

                    tx_display_index_root();
                    view_set_handlers(tx_getData, tx_accept_sign, tx_reject);
                    view_tx_show(0);

                    *flags |= IO_ASYNCH_REPLY;
                    break;
                }

                case INS_SIGN_PERSONAL_MESSAGE: {
                    if (process_chunk(tx, rx, true)) {
                        /* Maximum sign message length is 255 */
                        if (transaction_get_buffer_length() > 255) {
                            THROW(APDU_CODE_WRONG_LENGTH);
                        }

                        tx_display_index_root();
                        view_set_handlers(smsg_getData, smsg_accept, smsg_reject);
                        view_smsg_show(0);
                      
                        *flags |= IO_ASYNCH_REPLY;
                    }
                    THROW(APDU_CODE_OK);
                }
                break;

#ifdef TESTING_ENABLED
                case INS_HASH_TEST: {
                    if (process_chunk(tx, rx, false)) {
                        uint8_t message_digest[CX_SHA256_SIZE];

                        cx_hash_sha256(transaction_get_buffer(),
                                       transaction_get_buffer_length(),
                                       message_digest,
                                       CX_SHA256_SIZE);

                        memcpy(G_io_apdu_buffer, message_digest, CX_SHA256_SIZE);
                        *tx += 32;
                    }
                    THROW(APDU_CODE_OK);
                }
                break;

                case INS_PUBLIC_KEY_SECP256K1_TEST: {
                    // Generate key
                    cx_ecfp_public_key_t publicKey;
                    cx_ecfp_private_key_t privateKey;
                    keys_secp256k1(&publicKey, &privateKey, privateKeyDataTest );

                    memcpy(G_io_apdu_buffer, publicKey.W, 65);
                    *tx += 65;

                    THROW(APDU_CODE_OK);
                }
                break;

                case INS_SIGN_SECP256K1_TEST: {
                    if (process_chunk(tx, rx, false)) {

                        unsigned int length = 0;

                        // Generate keys
                        cx_ecfp_public_key_t publicKey;
                        cx_ecfp_private_key_t privateKey;
                        keys_secp256k1(&publicKey, &privateKey, privateKeyDataTest );

                        // Skip UI and validation
                        sign_secp256k1(
                                transaction_get_buffer(),
                                transaction_get_buffer_length(),
                                G_io_apdu_buffer,
                                IO_APDU_BUFFER_SIZE,
                                &length,
                                &privateKey);

                        *tx += length;
                    }
                    THROW(APDU_CODE_OK);
                }
                break;
#endif

                default:
                    THROW(APDU_CODE_INS_NOT_SUPPORTED);
            }
        }
        CATCH(EXCEPTION_IO_RESET)
        {
            THROW(EXCEPTION_IO_RESET);
        }
        CATCH_OTHER(e)
        {
            switch (e & 0xF000) {
                case 0x6000:
                case APDU_CODE_OK:
                    sw = e;
                    break;
                default:
                    sw = 0x6800 | (e & 0x7FF);
                    break;
            }
            G_io_apdu_buffer[*tx] = sw >> 8;
            G_io_apdu_buffer[*tx + 1] = sw;
            *tx += 2;
        }
        FINALLY
        {
        }
    }
    END_TRY;
}

void handle_generic_apdu(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    UNUSED(flags);

    if (rx > 4 && memcmp(G_io_apdu_buffer, "\xE0\x01\x00\x00", 4) == 0) {
        // Respond to get device info command
        uint8_t *p = G_io_apdu_buffer;
        // Target ID        4 bytes
        p[0]=(TARGET_ID >> 24) & 0xFF;
        p[1]=(TARGET_ID >> 16) & 0xFF;
        p[2]=(TARGET_ID >> 8) & 0xFF;
        p[3]=(TARGET_ID >> 0) & 0xFF;
        p += 4;
        // SE Version       [length][non-terminated string]
        *p = os_version(p + 1, 64);
        p = p + 1 + *p;
        // Flags            [length][flags]
        *p = 0;
        p++;
        // MCU Version      [length][non-terminated string]
        *p = os_seph_version(p + 1, 64);
        p = p + 1 + *p;

        *tx = p - G_io_apdu_buffer;
        THROW(APDU_CODE_OK);
    }
}

void app_init() {
    io_seproxyhal_init();

    /* First time initialize settings options */
    if (N_settings.initialized != 0x01) {
        nvm_settings_t settings;

        settings.contractDataAllowed = 0x00;
        settings.initialized = 0x01;
        nvm_write((void *) &N_settings, (void *) &settings, sizeof(nvm_settings_t));
    }

    USB_power(0);
    USB_power(1);
    view_idle(0);
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-noreturn"

void app_main() {
    volatile uint32_t rx = 0, tx = 0, flags = 0;

    for (;;) {
        volatile uint16_t sw = 0;

        BEGIN_TRY;
        {
            TRY;
            {
                rx = tx;
                tx = 0;
                rx = io_exchange(CHANNEL_APDU | flags, rx);
                flags = 0;

                if (rx == 0)
                    THROW(APDU_CODE_EMPTY_BUFFER);

                handle_generic_apdu(&flags, &tx, rx);

                handleApdu(&flags, &tx, rx);
            }
            CATCH_OTHER(e);
            {
                switch (e & 0xF000) {
                    case 0x6000:
                    case 0x9000:
                        sw = e;
                        break;
                    default:
                        sw = 0x6800 | (e & 0x7FF);
                        break;
                }
                G_io_apdu_buffer[tx] = sw >> 8;
                G_io_apdu_buffer[tx + 1] = sw;
                tx += 2;
            }
            FINALLY;
            {}
        }
        END_TRY;
    }
}

#pragma clang diagnostic pop
