/*
 * Copyright (C) 2018-2021 Jolla Ltd.
 * Copyright (C) 2018-2021 Slava Monich <slava.monich@jolla.com>
 * Copyright (C) 2021 Open Mobile Platform LLC.
 *
 * You may use this file under the terms of BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer in the
 *      documentation and/or other materials provided with the distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#define GLIB_DISABLE_DEPRECATION_WARNINGS

#include "ril_binder_radio.h"
#include "ril_binder_radio_impl.h"
#include "ril_binder_oemhook.h"
#include "ril_binder_log.h"

#include <ofono/ril-constants.h>

#include "grilio_channel.h"
#include "grilio_encode.h"
#include "grilio_parser.h"
#include "grilio_request.h"
#include "grilio_transport_impl.h"

#include <gbinder.h>

#include <radio_instance.h>

#include <gutil_idlequeue.h>
#include <gutil_misc.h>

/* Logging */
GLOG_MODULE_DEFINE("grilio-binder");

#define RIL_BINDER_KEY_MODEM      "modem"
#define RIL_BINDER_KEY_DEV        "dev"
#define RIL_BINDER_KEY_NAME       "name"
#define RIL_BINDER_KEY_INTERFACE  "interface"

#define RIL_BINDER_DEFAULT_MODEM     "/ril_0"
#define RIL_BINDER_DEFAULT_DEV       "/dev/hwbinder"
#define RIL_BINDER_DEFAULT_NAME      "slot1"

#define DEFAULT_INTERFACE RADIO_INTERFACE_1_2

#define RIL_PROTO_IP_STR     "IP"
#define RIL_PROTO_IPV6_STR   "IPV6"
#define RIL_PROTO_IPV4V6_STR "IPV4V6"

/* Preferred network types as defined in ril.h */
enum ril_pref_net_type {
    PREF_NET_TYPE_GSM_WCDMA                 = 0,
    PREF_NET_TYPE_GSM_ONLY                  = 1,
    PREF_NET_TYPE_WCDMA                     = 2,
    PREF_NET_TYPE_GSM_WCDMA_AUTO            = 3,
    PREF_NET_TYPE_CDMA_EVDO_AUTO            = 4,
    PREF_NET_TYPE_CDMA_ONLY                 = 5,
    PREF_NET_TYPE_EVDO_ONLY                 = 6,
    PREF_NET_TYPE_GSM_WCDMA_CDMA_EVDO_AUTO  = 7,
    PREF_NET_TYPE_LTE_CDMA_EVDO             = 8,
    PREF_NET_TYPE_LTE_GSM_WCDMA             = 9,
    PREF_NET_TYPE_LTE_CMDA_EVDO_GSM_WCDMA   = 10,
    PREF_NET_TYPE_LTE_ONLY                  = 11,
    PREF_NET_TYPE_LTE_WCDMA                 = 12
};

enum {
    RADIO_EVENT_INDICATION,
    RADIO_EVENT_RESPONSE,
    RADIO_EVENT_ACK,
    RADIO_EVENT_DEATH,
    RADIO_EVENT_COUNT
};

typedef struct ril_binder_radio_call {
    guint code;
    RADIO_REQ req_tx;
    RADIO_RESP resp_tx;
    gboolean (*encode)(GRilIoRequest* in, GBinderLocalRequest* out);
    RilBinderRadioDecodeFunc decode;
    const char* name;
} RilBinderRadioCall;

typedef struct ril_binder_radio_event {
    guint code;
    RADIO_IND unsol_tx;
    RilBinderRadioDecodeFunc decode;
    const char* name;
} RilBinderRadioEvent;

typedef struct ril_binder_radio_failure_data {
    GRilIoTransport* transport;
    guint serial;
} RilBinderRadioFailureData;

struct ril_binder_radio_priv {
    RilBinderOemHook* oemhook;
    gulong oemhook_raw_response_id;
    GUtilIdleQueue* idle;
    GByteArray* buf;
    gulong radio_event_id[RADIO_EVENT_COUNT];
    /* code -> RilBinderRadioCall */
    GHashTable* req_map[RADIO_INTERFACE_COUNT];
    /* resp_tx -> RilBinderRadioCall */
    GHashTable* resp_map[RADIO_INTERFACE_COUNT];
    /* unsol_tx -> RilBinderRadioEvent */
    GHashTable* unsol_map[RADIO_INTERFACE_COUNT];
};

G_DEFINE_TYPE(RilBinderRadio, ril_binder_radio, GRILIO_TYPE_TRANSPORT)

#define PARENT_CLASS ril_binder_radio_parent_class

#define ARRAY_AND_COUNT(a) a, G_N_ELEMENTS(a)
#define DBG_(self,fmt,args...) \
    GDEBUG("%s" fmt, (self)->parent.log_prefix, ##args)

/*==========================================================================*
 * Utilities
 *==========================================================================*/

#define ril_binder_radio_write_hidl_string_data(writer,ptr,field,index) \
    ril_binder_radio_write_hidl_string_data2(writer,ptr,field,index,0)
#define ril_binder_radio_write_hidl_string_data2(writer,ptr,field,index,off) \
     ril_binder_radio_write_string_with_parent(writer, &ptr->field, index, \
        (off) + ((guint8*)(&ptr->field) - (guint8*)ptr))

static
inline
void
ril_binder_radio_write_string_with_parent(
    GBinderWriter* writer,
    const GBinderHidlString* str,
    guint32 index,
    guint32 offset)
{
    GBinderParent parent;

    parent.index = index;
    parent.offset = offset;

    /* Strings are NULL-terminated, hence len + 1 */
    gbinder_writer_append_buffer_object_with_parent(writer, str->data.str,
        str->len + 1, &parent);
}

static
void
ril_binder_radio_write_data_profile_strings(
    GBinderWriter* w,
    const RadioDataProfile* dp,
    guint idx,
    guint i)
{
    const guint off = sizeof(*dp) * i;

    /* Write the string data in the right order */
    ril_binder_radio_write_hidl_string_data2(w, dp, apn, idx, off);
    ril_binder_radio_write_hidl_string_data2(w, dp, protocol, idx, off);
    ril_binder_radio_write_hidl_string_data2(w, dp, roamingProtocol, idx, off);
    ril_binder_radio_write_hidl_string_data2(w, dp, user, idx, off);
    ril_binder_radio_write_hidl_string_data2(w, dp, password, idx, off);
    ril_binder_radio_write_hidl_string_data2(w, dp, mvnoMatchData, idx, off);
}

inline
static
void
ril_binder_radio_write_single_data_profile(
    GBinderWriter* writer,
    const RadioDataProfile* dp)
{
    ril_binder_radio_write_data_profile_strings(writer, dp,
        gbinder_writer_append_buffer_object(writer, dp, sizeof(*dp)), 0);
}

static
void
ril_binder_radio_take_string(
    GBinderLocalRequest* out,
    GBinderHidlString* str,
    char* chars)
{
    str->owns_buffer = TRUE;
    if (chars && chars[0]) {
        /* GBinderLocalRequest takes ownership of the string contents */
        str->data.str = chars;
        str->len = strlen(chars);
        gbinder_local_request_cleanup(out, g_free, chars);
    } else {
        /* Replace NULL strings with empty strings */
        str->data.str = "";
        str->len = 0;
        g_free(chars);
    }
}

static
void
ril_binder_radio_init_parser(
    GRilIoParser* parser,
    GRilIoRequest* r)
{
    grilio_parser_init(parser, grilio_request_data(r), grilio_request_size(r));
}

static
const char*
ril_binder_radio_arg_value(
    GHashTable* args,
    const char* key,
    const char* def)
{
    const char* val = args ? g_hash_table_lookup(args, key) : NULL;

    return (val && val[0]) ? val : def;
}

static
void
ril_binder_radio_init_call_maps(
    GHashTable* req_map,
    GHashTable* resp_map,
    const RilBinderRadioCall* calls,
    guint count)
{
    guint i;

    for (i = 0; i < count; i++) {
        const RilBinderRadioCall* call = calls + i;

        if (call->req_tx) {
            g_hash_table_insert(req_map, GINT_TO_POINTER(call->code),
                (gpointer) call);
        }
        if (call->resp_tx) {
            g_hash_table_insert(resp_map, GINT_TO_POINTER(call->resp_tx),
                (gpointer) call);
        }
    }
}

static
void
ril_binder_radio_init_unsol_map(
    GHashTable* unsol_map,
    const RilBinderRadioEvent* events,
    guint count)
{
    guint i;

    for (i = 0; i < count; i++) {
        const RilBinderRadioEvent* event = events + i;

        g_hash_table_insert(unsol_map, GINT_TO_POINTER(event->unsol_tx),
            (gpointer) event);
    }
}

static
RADIO_APN_TYPES
ril_binder_radio_apn_types_for_profile(
    RADIO_DATA_PROFILE_ID profile_id)
{
    switch (profile_id) {
    case RADIO_DATA_PROFILE_INVALID:
        return RADIO_APN_TYPE_NONE;
    case RADIO_DATA_PROFILE_IMS:
        return RADIO_APN_TYPE_IMS;
    case RADIO_DATA_PROFILE_CBS:
        return RADIO_APN_TYPE_CBS;
    case RADIO_DATA_PROFILE_FOTA:
        return RADIO_APN_TYPE_FOTA;
    case RADIO_DATA_PROFILE_DEFAULT:
        return (RADIO_APN_TYPE_DEFAULT |
                RADIO_APN_TYPE_SUPL |
                RADIO_APN_TYPE_IA);
    default:
        /*
         * There's no standard profile id for MMS, OEM-specific profile ids
         * are used for that.
         */
        return RADIO_APN_TYPE_MMS;
    }
}

static
const char *
radio_pdp_protocol_type_to_str(enum radio_pdp_protocol_type type)
{
    switch (type) {
    case RADIO_PDP_PROTOCOL_IP:
        return RIL_PROTO_IP_STR;
    case RADIO_PDP_PROTOCOL_IPV6:
        return RIL_PROTO_IPV6_STR;
    case RADIO_PDP_PROTOCOL_IPV4V6:
        return RIL_PROTO_IPV4V6_STR;
    default:
        return "";
    }
}

static
const char*
ril_binder_radio_interface_name(
    RADIO_INTERFACE interface)
{
    switch (interface) {
    case RADIO_INTERFACE_1_0: return "radio@1.0";
    case RADIO_INTERFACE_1_1: return "radio@1.1";
    case RADIO_INTERFACE_1_2: return "radio@1.2";
    case RADIO_INTERFACE_1_3: return "radio@1.3";
    case RADIO_INTERFACE_1_4: return "radio@1.4";
    case RADIO_INTERFACE_COUNT:
        break;
    }
    return NULL;
}

/*==========================================================================*
 * Encoders (plugin -> binder)
 *==========================================================================*/

static
gboolean
ril_binder_radio_encode_serial(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    gbinder_local_request_append_int32(out, grilio_request_serial(in));
    return TRUE;
}

static
gboolean
ril_binder_radio_encode_int(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    gint32 value;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &value)) {
        GBinderWriter writer;

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_int32(&writer, value);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_encode_bool(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    gint32 count, value;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 1 &&
        grilio_parser_get_int32(&parser, &value)) {
        GBinderWriter writer;

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_bool(&writer, value);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_encode_ints(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    gint32 count;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count)) {
        GBinderWriter writer;
        guint i;

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        for (i = 0; i < count; i++) {
            gint32 value;

            if (grilio_parser_get_int32(&parser, &value)) {
                gbinder_writer_append_int32(&writer, value);
            } else {
                return FALSE;
            }
        }
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_encode_string(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    char* str;

    ril_binder_radio_init_parser(&parser, in);
    str = grilio_parser_get_utf8(&parser);
    if (str) {
        GBinderWriter writer;

        gbinder_local_request_cleanup(out, g_free, str);
        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_hidl_string(&writer, str);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_encode_strings(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    gint32 count;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count)) {
        GBinderWriter writer;
        guint i;

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        for (i = 0; i < count; i++) {
            char* str;

            if (grilio_parser_get_nullable_utf8(&parser, &str)) {
                if (str) {
                    gbinder_local_request_cleanup(out, g_free, str);
                    gbinder_writer_append_hidl_string(&writer, str);
                } else {
                    gbinder_writer_append_hidl_string(&writer, "");
                }
            } else {
                return FALSE;
            }
        }
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_encode_ints_to_bool_int(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    gint32 count, arg1, arg2;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 2 &&
        grilio_parser_get_int32(&parser, &arg1) &&
        grilio_parser_get_int32(&parser, &arg2)) {
        GBinderWriter writer;

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_bool(&writer, arg1);
        gbinder_writer_append_int32(&writer, arg2);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_encode_deactivate_data_call(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    gboolean ok = FALSE;
    GRilIoParser parser;
    gint32 count, cid, reason;
    char* cid_str = NULL;
    char* reason_str = NULL;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 2 &&
        (cid_str = grilio_parser_get_utf8(&parser)) != NULL &&
        (reason_str = grilio_parser_get_utf8(&parser)) != NULL &&
        gutil_parse_int(cid_str, 10, &cid) &&
        gutil_parse_int(reason_str, 10, &reason)) {
        GBinderWriter writer;

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_int32(&writer, cid);
        gbinder_writer_append_bool(&writer, reason);
        ok = TRUE;
    }
    g_free(cid_str);
    g_free(reason_str);
    return ok;
}

static
gboolean
ril_binder_radio_encode_deactivate_data_call_1_2(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    gboolean ok = FALSE;
    GRilIoParser parser;
    gint32 count, cid, reason;
    char* cid_str = NULL;
    char* reason_str = NULL;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 2 &&
        (cid_str = grilio_parser_get_utf8(&parser)) != NULL &&
        (reason_str = grilio_parser_get_utf8(&parser)) != NULL &&
        gutil_parse_int(cid_str, 10, &cid) &&
        gutil_parse_int(reason_str, 10, &reason)) {
        GBinderWriter writer;

        if (reason == 0) {
            reason = RADIO_DATA_REQUEST_REASON_NORMAL;
        } else if (reason == 1) {
            reason = RADIO_DATA_REQUEST_REASON_SHUTDOWN;
        } else {
            reason = RADIO_DATA_REQUEST_REASON_HANDOVER;
        }

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_int32(&writer, cid);
        gbinder_writer_append_int32(&writer, reason);
        ok = TRUE;
    }
    g_free(cid_str);
    g_free(reason_str);
    return ok;
}

/**
 * @param int32_t Serial number of request.
 * @param dialInfo Dial struct
 */
static
gboolean
ril_binder_radio_encode_dial(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    char* number;
    gint32 clir;

    ril_binder_radio_init_parser(&parser, in);
    if ((number = grilio_parser_get_utf8(&parser)) != NULL &&
        grilio_parser_get_int32(&parser, &clir)) {
        /* and ignore UUS information */
        GBinderWriter writer;
        GBinderParent parent;
        RadioDial* dial;

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        dial = gbinder_writer_new0(&writer, RadioDial);
        ril_binder_radio_take_string(out, &dial->address, number);
        dial->clir = clir;

        /* Write the arguments */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent.index = gbinder_writer_append_buffer_object(&writer, dial,
            sizeof(*dial));

        /* Write the string data */
        ril_binder_radio_write_hidl_string_data(&writer, dial, address,
            parent.index);

        /* UUS information is empty but we still need to write a buffer */
        parent.offset = G_STRUCT_OFFSET(RadioDial, uusInfo.data.ptr);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            "" /* arbitrary pointer */, 0, &parent);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param message GsmSmsMessage as defined in types.hal
 */
static
gboolean
ril_binder_radio_encode_gsm_sms_message(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    gint32 count;
    char* smsc = NULL;
    char* pdu = NULL;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 2 &&
        grilio_parser_get_nullable_utf8(&parser, &smsc) &&
        (pdu = grilio_parser_get_utf8(&parser)) != NULL) {
        RadioGsmSmsMessage* sms;
        GBinderWriter writer;
        guint parent;

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        sms = gbinder_writer_new0(&writer, RadioGsmSmsMessage);
        ril_binder_radio_take_string(out, &sms->smscPdu, smsc);
        ril_binder_radio_take_string(out, &sms->pdu, pdu);

        /* Write the arguments */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent = gbinder_writer_append_buffer_object(&writer,
            sms, sizeof(*sms));

        /* Write the string data in the right order */
        ril_binder_radio_write_hidl_string_data(&writer, sms, smscPdu, parent);
        ril_binder_radio_write_hidl_string_data(&writer, sms, pdu, parent);
        return TRUE;
    }

    g_free(smsc);
    g_free(pdu);
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param RadioTechnology Radio technology to use.
 * @param DataProfileInfo Data profile info.
 * @param bool modemCognitive Indicating this profile was sent to the modem
 *                       through setDataProfile earlier.
 * @param bool roamingAllowed Indicating data roaming is allowed or not.
 * @param bool isRoaming Indicating the device is roaming or not.
 */
static
gboolean
ril_binder_radio_encode_setup_data_call(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    gboolean ok = FALSE;
    GRilIoParser parser;
    gint32 count, tech, auth, profile_id;
    char* profile_str = NULL;
    char* tech_str = NULL;
    char* apn = NULL;
    char* user = NULL;
    char* password = NULL;
    char* auth_str = NULL;
    char* proto = NULL;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 7 &&
        (tech_str = grilio_parser_get_utf8(&parser)) != NULL &&
        gutil_parse_int(tech_str, 10, &tech) &&
        (profile_str = grilio_parser_get_utf8(&parser)) != NULL &&
        gutil_parse_int(profile_str, 10, &profile_id) &&
        (apn = grilio_parser_get_utf8(&parser)) != NULL &&
        (user = grilio_parser_get_utf8(&parser)) != NULL &&
        (password = grilio_parser_get_utf8(&parser)) != NULL &&
        (auth_str = grilio_parser_get_utf8(&parser)) != NULL &&
        gutil_parse_int(auth_str, 10, &auth) &&
        (proto = grilio_parser_get_utf8(&parser)) != NULL) {
        GBinderWriter writer;
        RadioDataProfile* profile;

        /* ril.h has this to say about the radio tech parameter:
         *
         * ((const char **)data)[0] Radio technology to use: 0-CDMA,
         *                          1-GSM/UMTS, 2... for values above 2
         *                          this is RIL_RadioTechnology + 2.
         *
         * Makes little sense but it is what it is.
         */
        if (tech > 4) {
            tech -= 2;
        }

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        profile = gbinder_writer_new0(&writer, RadioDataProfile);
        ril_binder_radio_take_string(out, &profile->apn, apn);
        ril_binder_radio_take_string(out, &profile->protocol, proto);
        ril_binder_radio_take_string(out, &profile->user, user);
        ril_binder_radio_take_string(out, &profile->password, password);
        ril_binder_radio_take_string(out, &profile->mvnoMatchData, NULL);
        profile->roamingProtocol = profile->protocol;
        profile->profileId = profile_id;
        profile->authType = auth;
        profile->enabled = TRUE;
        profile->supportedApnTypesBitmap =
            ril_binder_radio_apn_types_for_profile(profile_id);

        /* Write the parcel */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_int32(&writer, tech); /* radioTechnology */
        ril_binder_radio_write_single_data_profile(&writer, profile);
        gbinder_writer_append_bool(&writer, FALSE); /* modemCognitive */
        /* TODO: provide the actual roaming status? */
        gbinder_writer_append_bool(&writer, TRUE);  /* roamingAllowed */
        gbinder_writer_append_bool(&writer, FALSE); /* isRoaming */
        ok = TRUE;
    } else {
        g_free(apn);
        g_free(user);
        g_free(password);
        g_free(proto);
    }

    g_free(profile_str);
    g_free(tech_str);
    g_free(auth_str);
    return ok;
}

/**
 * @param int32_t Serial number of request.
 * @param RadioTechnology Radio technology to use.
 * @param DataProfileInfo Data profile info.
 * @param bool modemCognitive Indicating this profile was sent to the modem
 *                       through setDataProfile earlier.
 * @param bool roamingAllowed Indicating data roaming is allowed or not.
 * @param bool isRoaming Indicating the device is roaming or not.
 */
static
gboolean
ril_binder_radio_encode_setup_data_call_1_2(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    gboolean ok = FALSE;
    GRilIoParser parser;
    gint32 count, tech, auth, profile_id;
    char* profile_str = NULL;
    char* tech_str = NULL;
    char* auth_str = NULL;
    char* apn = NULL;
    char* user = NULL;
    char* password = NULL;
    char* proto = NULL;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 7 &&
        (tech_str = grilio_parser_get_utf8(&parser)) != NULL &&
        gutil_parse_int(tech_str, 10, &tech) &&
        (profile_str = grilio_parser_get_utf8(&parser)) != NULL &&
        gutil_parse_int(profile_str, 10, &profile_id) &&
        (apn = grilio_parser_get_utf8(&parser)) != NULL &&
        (user = grilio_parser_get_utf8(&parser)) != NULL &&
        (password = grilio_parser_get_utf8(&parser)) != NULL &&
        (auth_str = grilio_parser_get_utf8(&parser)) != NULL &&
        gutil_parse_int(auth_str, 10, &auth) &&
        (proto = grilio_parser_get_utf8(&parser)) != NULL) {
        GBinderWriter writer;
        RadioDataProfile* profile;
        RADIO_ACCESS_NETWORK ran;

        /* ril.h has this to say about the radio tech parameter:
         *
         * ((const char **)data)[0] Radio technology to use: 0-CDMA,
         *                          1-GSM/UMTS, 2... for values above 2
         *                          this is RIL_RadioTechnology + 2.
         *
         * Makes little sense but it is what it is.
         */
        if (tech > 4) {
            tech -= 2;
        }

        ran = RADIO_ACCESS_NETWORK_UNKNOWN;
        switch ((RADIO_TECH)tech) {
        case RADIO_TECH_GPRS:
        case RADIO_TECH_EDGE:
        case RADIO_TECH_GSM:
            ran = RADIO_ACCESS_NETWORK_GERAN;
            break;
        case RADIO_TECH_UMTS:
        case RADIO_TECH_HSDPA:
        case RADIO_TECH_HSPAP:
        case RADIO_TECH_HSUPA:
        case RADIO_TECH_HSPA:
        case RADIO_TECH_TD_SCDMA:
            ran = RADIO_ACCESS_NETWORK_UTRAN;
            break;
        case RADIO_TECH_IS95A:
        case RADIO_TECH_IS95B:
        case RADIO_TECH_ONE_X_RTT:
        case RADIO_TECH_EVDO_0:
        case RADIO_TECH_EVDO_A:
        case RADIO_TECH_EVDO_B:
        case RADIO_TECH_EHRPD:
            ran = RADIO_ACCESS_NETWORK_CDMA2000;
            break;
        case RADIO_TECH_LTE:
        case RADIO_TECH_LTE_CA:
            ran = RADIO_ACCESS_NETWORK_EUTRAN;
            break;
        case RADIO_TECH_IWLAN:
            ran = RADIO_ACCESS_NETWORK_IWLAN;
            break;
        case RADIO_TECH_UNKNOWN:
            break;
        }

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        profile = gbinder_writer_new0(&writer, RadioDataProfile);
        ril_binder_radio_take_string(out, &profile->apn, apn);
        ril_binder_radio_take_string(out, &profile->protocol, proto);
        ril_binder_radio_take_string(out, &profile->user, user);
        ril_binder_radio_take_string(out, &profile->password, password);
        ril_binder_radio_take_string(out, &profile->mvnoMatchData, NULL);
        profile->roamingProtocol = profile->protocol;
        profile->profileId = profile_id;
        profile->authType = auth;
        profile->enabled = TRUE;
        profile->supportedApnTypesBitmap =
            ril_binder_radio_apn_types_for_profile(profile_id);

        /* Write the parcel */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_int32(&writer, ran); /* accessNetwork */
        ril_binder_radio_write_single_data_profile(&writer, profile);
        gbinder_writer_append_bool(&writer, FALSE); /* modemCognitive */
        /* TODO: provide the actual roaming status? */
        gbinder_writer_append_bool(&writer, TRUE);  /* roamingAllowed */
        gbinder_writer_append_bool(&writer, FALSE); /* isRoaming */
        gbinder_writer_append_int32(&writer, RADIO_DATA_REQUEST_REASON_NORMAL);
        gbinder_writer_append_hidl_string_vec(&writer, NULL, 0); /* addresses */
        gbinder_writer_append_hidl_string_vec(&writer, NULL, 0); /* dnses */
        ok = TRUE;
    } else {
        g_free(apn);
        g_free(user);
        g_free(password);
        g_free(proto);
    }

    g_free(profile_str);
    g_free(tech_str);
    g_free(auth_str);
    return ok;
}

/**
 * @param int32_t Serial number of request.
 * @param smsWriteArgs SmsWriteArgs defined in types.hal
 */
static
gboolean
ril_binder_radio_encode_sms_write_args(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    RadioSmsWriteArgs* sms = g_new0(RadioSmsWriteArgs, 1);
    GRilIoParser parser;
    char* pdu = NULL;
    char* smsc = NULL;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &sms->status) &&
        (pdu = grilio_parser_get_utf8(&parser)) != NULL &&
        grilio_parser_get_nullable_utf8(&parser, &smsc)) {
        GBinderWriter writer;
        guint parent;

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        gbinder_local_request_cleanup(out, g_free, sms);
        ril_binder_radio_take_string(out, &sms->pdu, pdu);
        ril_binder_radio_take_string(out, &sms->smsc, smsc);

        /* Write the arguments */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent = gbinder_writer_append_buffer_object(&writer,
            sms, sizeof(*sms));

        /* Write the string data in the right order */
        ril_binder_radio_write_hidl_string_data(&writer, sms, pdu, parent);
        ril_binder_radio_write_hidl_string_data(&writer, sms, smsc, parent);
        return TRUE;
    }

    g_free(smsc);
    g_free(pdu);
    g_free(sms);
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param IccIo
 */
static
gboolean
ril_binder_radio_encode_icc_io(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    RadioIccIo* io = g_new0(RadioIccIo, 1);
    char* path = NULL;
    char* data = NULL;
    char* pin2 = NULL;
    char* aid = NULL;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &io->command) &&
        grilio_parser_get_int32(&parser, &io->fileId) &&
        grilio_parser_get_nullable_utf8(&parser, &path) &&
        grilio_parser_get_int32(&parser, &io->p1) &&
        grilio_parser_get_int32(&parser, &io->p2) &&
        grilio_parser_get_int32(&parser, &io->p3) &&
        grilio_parser_get_nullable_utf8(&parser, &data) &&
        grilio_parser_get_nullable_utf8(&parser, &pin2) &&
        grilio_parser_get_nullable_utf8(&parser, &aid)) {
        GBinderWriter writer;
        guint parent;

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        gbinder_local_request_cleanup(out, g_free, io);
        ril_binder_radio_take_string(out, &io->path, path);
        ril_binder_radio_take_string(out, &io->data, data);
        ril_binder_radio_take_string(out, &io->pin2, pin2);
        ril_binder_radio_take_string(out, &io->aid, aid);

        /* Write the arguments */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent = gbinder_writer_append_buffer_object(&writer, io, sizeof(*io));

        /* Write the string data in the right order */
        ril_binder_radio_write_hidl_string_data(&writer, io, path, parent);
        ril_binder_radio_write_hidl_string_data(&writer, io, data, parent);
        ril_binder_radio_write_hidl_string_data(&writer, io, pin2, parent);
        ril_binder_radio_write_hidl_string_data(&writer, io, aid, parent);
        return TRUE;
    }

    g_free(io);
    g_free(path);
    g_free(data);
    g_free(pin2);
    g_free(aid);
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param CallForwardInfo
*/
static
gboolean
ril_binder_radio_encode_call_forward_info(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    RadioCallForwardInfo* info = g_new0(RadioCallForwardInfo, 1);
    char* number = NULL;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &info->status) &&
        grilio_parser_get_int32(&parser, &info->reason) &&
        grilio_parser_get_int32(&parser, &info->serviceClass) &&
        grilio_parser_get_int32(&parser, &info->toa) &&
        grilio_parser_get_nullable_utf8(&parser, &number) &&
        grilio_parser_get_int32(&parser, &info->timeSeconds)) {
        GBinderWriter writer;
        guint parent;

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        gbinder_local_request_cleanup(out, g_free, info);
        ril_binder_radio_take_string(out, &info->number, number);

        /* Write the arguments */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent = gbinder_writer_append_buffer_object(&writer,
            info, sizeof(*info));

        /* Write the string data */
        ril_binder_radio_write_hidl_string_data(&writer, info, number, parent);
        return TRUE;
    }

    g_free(info);
    g_free(number);
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param facility is the facility string code from TS 27.007 7.4
 * @param password is the password, or "" if not required
 * @param serviceClass is the TS 27.007 service class bit vector of services
 * @param appId is AID value, empty string if no value.
 */
static
gboolean
ril_binder_radio_encode_get_facility_lock(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    gboolean ok = FALSE;
    GRilIoParser parser;
    gint32 count;
    char* fac = NULL;
    char* pwd = NULL;
    char* cls = NULL;
    char* aid = NULL;
    int cls_num;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 4 &&
        grilio_parser_get_nullable_utf8(&parser, &fac) &&
        grilio_parser_get_nullable_utf8(&parser, &pwd) &&
        grilio_parser_get_nullable_utf8(&parser, &cls) &&
        grilio_parser_get_nullable_utf8(&parser, &aid) &&
        gutil_parse_int(cls, 10, &cls_num)) {
        GBinderWriter writer;

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        if (fac) {
            gbinder_local_request_cleanup(out, g_free, fac);
            gbinder_writer_append_hidl_string(&writer, fac);
        } else {
            gbinder_writer_append_hidl_string(&writer, "");
        }

        if (pwd) {
            gbinder_local_request_cleanup(out, g_free, pwd);
            gbinder_writer_append_hidl_string(&writer, pwd);
        } else {
            gbinder_writer_append_hidl_string(&writer, "");
        }

        gbinder_writer_append_int32(&writer, cls_num);

        if (aid) {
            gbinder_local_request_cleanup(out, g_free, aid);
            gbinder_writer_append_hidl_string(&writer, aid);
        } else {
            gbinder_writer_append_hidl_string(&writer, "");
        }

        ok = TRUE;
    } else {
        g_free(fac);
        g_free(pwd);
        g_free(aid);
    }

    g_free(cls);
    return ok;
}

/**
 * @param int32_t Serial number of request.
 * @param facility is the facility string code from TS 27.007 7.4
 * @param lockState false for "unlock" and true for "lock"
 * @param password is the password
 * @param serviceClass is string representation of decimal TS 27.007
 * @param appId is AID value, empty string if no value.
 */
static
gboolean
ril_binder_radio_encode_set_facility_lock(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    gboolean ok = FALSE;
    GRilIoParser parser;
    char* fac = NULL;
    char* lock = NULL;
    char* pwd = NULL;
    char* cls = NULL;
    char* aid = NULL;
    gint32 count, lock_num, cls_num;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 5 &&
        grilio_parser_get_nullable_utf8(&parser, &fac) &&
        grilio_parser_get_nullable_utf8(&parser, &lock) &&
        grilio_parser_get_nullable_utf8(&parser, &pwd) &&
        grilio_parser_get_nullable_utf8(&parser, &cls) &&
        grilio_parser_get_nullable_utf8(&parser, &aid) &&
        gutil_parse_int(lock, 10, &lock_num) &&
        gutil_parse_int(cls, 10, &cls_num)) {
        GBinderWriter writer;

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        if (fac) {
            gbinder_local_request_cleanup(out, g_free, fac);
            gbinder_writer_append_hidl_string(&writer, fac);
        } else {
            gbinder_writer_append_hidl_string(&writer, "");
        }

        gbinder_writer_append_bool(&writer, lock_num);

        if (pwd) {
            gbinder_local_request_cleanup(out, g_free, pwd);
            gbinder_writer_append_hidl_string(&writer, pwd);
        } else {
            gbinder_writer_append_hidl_string(&writer, "");
        }

        gbinder_writer_append_int32(&writer, cls_num);

        if (aid) {
            gbinder_local_request_cleanup(out, g_free, aid);
            gbinder_writer_append_hidl_string(&writer, aid);
        } else {
            gbinder_writer_append_hidl_string(&writer, "");
        }

        ok = TRUE;
    } else {
        g_free(fac);
        g_free(pwd);
        g_free(aid);
    }

    g_free(lock);
    g_free(cls);
    return ok;
}

/**
 * @param int32_t Serial number of request.
 * @param DeviceStateType The updated device state type.
 * @param bool The updated state.
 */
static
void
ril_binder_radio_device_state_req(
    GBinderLocalRequest* req,
    guint32 serial,
    RADIO_DEVICE_STATE type,
    gboolean state)
{
    GBinderWriter writer;

    gbinder_local_request_init_writer(req, &writer);
    gbinder_writer_append_int32(&writer, serial);
    gbinder_writer_append_int32(&writer, type);
    gbinder_writer_append_bool(&writer, state);
}

static
gboolean
ril_binder_radio_map_screen_state_to_device_state(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    gint32 count, value;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 1 &&
        grilio_parser_get_int32(&parser, &value)) {
        ril_binder_radio_device_state_req(out, grilio_request_serial(in),
            RADIO_DEVICE_STATE_POWER_SAVE_MODE, !value);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_encode_device_state(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    gint32 count, type, state;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count) && count == 2 &&
        grilio_parser_get_int32(&parser, &type) &&
        grilio_parser_get_int32(&parser, &state)) {
        ril_binder_radio_device_state_req(out, grilio_request_serial(in),
            type, state);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param configInfo Setting of GSM/WCDMA Cell broadcast config
 */
static
gboolean
ril_binder_radio_encode_gsm_broadcast_sms_config(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    gint32 count;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count)) {
        GBinderHidlVec* vec = g_new0(GBinderHidlVec, 1);
        RadioGsmBroadcastSmsConfig* configs = NULL;
        gboolean ok = TRUE;
        guint i;

        vec->count = count;
        vec->owns_buffer = TRUE;
        gbinder_local_request_cleanup(out, g_free, vec);
        if (count > 0) {
            configs = g_new0(RadioGsmBroadcastSmsConfig, count);
            gbinder_local_request_cleanup(out, g_free, configs);
            vec->data.ptr = configs;
        }

        for (i = 0; i < count && ok; i++) {
            RadioGsmBroadcastSmsConfig* config = configs + i;
            gint32 selected;

            if (grilio_parser_get_int32(&parser, &config->fromServiceId) &&
                grilio_parser_get_int32(&parser, &config->toServiceId) &&
                grilio_parser_get_int32(&parser, &config->fromCodeScheme) &&
                grilio_parser_get_int32(&parser, &config->toCodeScheme) &&
                grilio_parser_get_int32(&parser, &selected)) {
                config->selected = (guint8)selected;
            } else {
                ok = FALSE;
            }
        }

        if (ok && grilio_parser_at_end(&parser)) {
            GBinderWriter writer;
            GBinderParent parent;

            /* The entire input has been successfully parsed */
            gbinder_local_request_init_writer(out, &writer);
            gbinder_writer_append_int32(&writer, grilio_request_serial(in));

            /* Write the parent structure */
            parent.offset = G_STRUCT_OFFSET(GBinderHidlVec, data.ptr);
            parent.index = gbinder_writer_append_buffer_object(&writer,
                vec, sizeof(*vec));

            if (count > 0) {
                gbinder_writer_append_buffer_object_with_parent(&writer,
                    configs, sizeof(configs[0]) * count, &parent);
            }
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param SelectUiccSub as defined in types.hal
 */
static
gboolean
ril_binder_radio_encode_uicc_sub(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    RadioSelectUiccSub* sub = g_new0(RadioSelectUiccSub, 1);

    gbinder_local_request_cleanup(out, g_free, sub);
    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &sub->slot) &&
        grilio_parser_get_int32(&parser, &sub->appIndex) &&
        grilio_parser_get_int32(&parser, &sub->subType) &&
        grilio_parser_get_int32(&parser, &sub->actStatus) &&
        grilio_parser_at_end(&parser)) {
        GBinderWriter writer;

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_buffer_object(&writer, sub, sizeof(*sub));
        return TRUE;
    }
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param DataProfileInfo Data profile containing APN settings
 * @param bool modemCognitive Indicating the data profile was sent to the modem
 *                       through setDataProfile earlier.
 * @param bool isRoaming Indicating the device is roaming or not.
 */
static
gboolean
ril_binder_radio_encode_initial_attach_apn(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    char* apn = NULL;
    char* proto = NULL;
    char* username = NULL;
    char* password = NULL;
    gint32 auth;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_nullable_utf8(&parser, &apn) &&
        grilio_parser_get_nullable_utf8(&parser, &proto) &&
        grilio_parser_get_int32(&parser, &auth) &&
        grilio_parser_get_nullable_utf8(&parser, &username) &&
        grilio_parser_get_nullable_utf8(&parser, &password)) {
        RadioDataProfile* profile;
        GBinderWriter writer;

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        profile = gbinder_writer_new0(&writer, RadioDataProfile);
        ril_binder_radio_take_string(out, &profile->apn, apn);
        ril_binder_radio_take_string(out, &profile->protocol, proto);
        ril_binder_radio_take_string(out, &profile->user, username);
        ril_binder_radio_take_string(out, &profile->password, password);
        ril_binder_radio_take_string(out, &profile->mvnoMatchData, NULL);
        profile->roamingProtocol = profile->protocol;
        profile->authType = auth;
        profile->supportedApnTypesBitmap = RADIO_APN_TYPE_IA;
        profile->enabled = TRUE;

        /* int32_t serial */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        /* DataProfileInfo dataProfileInfo */
        ril_binder_radio_write_single_data_profile(&writer, profile);
        /* bool modemCognitive */
        gbinder_writer_append_bool(&writer, FALSE);
        /* bool isRoaming */
        /* TODO: provide the actual roaming status? */
        gbinder_writer_append_bool(&writer, FALSE);
        return TRUE;
    }
    g_free(apn);
    g_free(proto);
    g_free(username);
    g_free(password);
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param vec<DataProfileInfo> Array of DataProfiles to set.
 * @param bool Indicating the device is roaming or not.
 */
static
gboolean
ril_binder_radio_encode_data_profiles(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    guint32 n;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_uint32(&parser, &n)) {
        guint i;
        GBinderWriter writer;
        GBinderHidlVec* vec;
        RadioDataProfile* profiles;

        gbinder_local_request_init_writer(out, &writer);
        profiles = gbinder_writer_malloc0(&writer, sizeof(*profiles) * n);
        vec = gbinder_writer_new0(&writer, GBinderHidlVec);
        vec->data.ptr = profiles;
        vec->count = n;
        vec->owns_buffer = TRUE;

        for (i = 0; i < n; i++) {
            RadioDataProfile* dp = profiles + i;
            gint32 profile_id, type, auth_type, enabled;
            char* apn = NULL;
            char* proto = NULL;
            char* username = NULL;
            char* password = NULL;

            if (grilio_parser_get_int32(&parser, &profile_id) &&
                grilio_parser_get_nullable_utf8(&parser, &apn) &&
                grilio_parser_get_nullable_utf8(&parser, &proto) &&
                grilio_parser_get_int32(&parser, &auth_type) &&
                grilio_parser_get_nullable_utf8(&parser, &username) &&
                grilio_parser_get_nullable_utf8(&parser, &password) &&
                grilio_parser_get_int32(&parser, &type) &&
                grilio_parser_get_int32(&parser, &dp->maxConnsTime) &&
                grilio_parser_get_int32(&parser, &dp->maxConns) &&
                grilio_parser_get_int32(&parser, &dp->waitTime) &&
                grilio_parser_get_int32(&parser, &enabled)) {
                /* Fill in the profile */
                ril_binder_radio_take_string(out, &dp->apn, apn);
                ril_binder_radio_take_string(out, &dp->protocol, proto);
                ril_binder_radio_take_string(out, &dp->user, username);
                ril_binder_radio_take_string(out, &dp->password, password);
                ril_binder_radio_take_string(out, &dp->mvnoMatchData, NULL);
                dp->type = type;
                dp->roamingProtocol = dp->protocol;
                dp->profileId = profile_id;
                dp->authType = auth_type;
                dp->enabled = enabled;
                dp->supportedApnTypesBitmap =
                    ril_binder_radio_apn_types_for_profile(profile_id);
            } else {
                g_free(apn);
                g_free(proto);
                g_free(username);
                g_free(password);
                break;
            }
        }

        if (i == n) {
            guint index;
            GBinderParent parent;

            /* int32_t serial */
            gbinder_writer_append_int32(&writer, grilio_request_serial(in));

            /* vec<DataProfileInfo> profiles */
            parent.offset = GBINDER_HIDL_VEC_BUFFER_OFFSET;
            parent.index = gbinder_writer_append_buffer_object(&writer,
                vec, sizeof(*vec));
            index = gbinder_writer_append_buffer_object_with_parent(&writer,
                profiles, sizeof(*profiles) * n, &parent);
            for (i = 0; i < n; i++) {
                ril_binder_radio_write_data_profile_strings(&writer,
                    profiles + i, index, i);
            }

            /* bool isRoaming */
            gbinder_writer_append_bool(&writer, FALSE);
            return TRUE;
        }
    }
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param RadioCapability structure to be set
 */
static
gboolean
ril_binder_radio_encode_radio_capability(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    char* uuid = NULL;
    gint32 version, session, phase, raf, status;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &version) &&
        grilio_parser_get_int32(&parser, &session) &&
        grilio_parser_get_int32(&parser, &phase) &&
        grilio_parser_get_int32(&parser, &raf) &&
        (uuid = grilio_parser_get_utf8(&parser)) != NULL &&
        grilio_parser_get_int32(&parser, &status)) {
        GBinderWriter writer;
        RadioCapability* rc;
        guint index;

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        rc = gbinder_writer_new0(&writer, RadioCapability);
        ril_binder_radio_take_string(out, &rc->logicalModemUuid, uuid);
        rc->session = session;
        rc->phase = phase;
        rc->raf = raf;
        rc->status = status;

        /* Write the arguments */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        index = gbinder_writer_append_buffer_object(&writer, rc, sizeof(*rc));

        /* Write the string data */
        ril_binder_radio_write_hidl_string_data(&writer, rc, logicalModemUuid,
            index);
        return TRUE;
    }
    g_free(uuid);
    return FALSE;
}

static
gboolean
ril_binder_radio_encode_icc_open_logical_channel(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    char* aid;

    ril_binder_radio_init_parser(&parser, in);
    aid = grilio_parser_get_utf8(&parser);
    if (aid) {
        GBinderWriter writer;
        gint32 p2 = 0;

        grilio_parser_get_int32(&parser, &p2); /* Optional? */
        gbinder_local_request_cleanup(out, g_free, aid);
        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        gbinder_writer_append_hidl_string(&writer, aid);
        gbinder_writer_append_int32(&writer, p2);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param int32_t Serial number of request.
 * @param SimApdu
 */
static
gboolean
ril_binder_radio_encode_icc_transmit_apdu_logical_channel(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    GRilIoParser parser;
    RadioSimApdu* apdu = g_new0(RadioSimApdu, 1);
    char* data = NULL;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &apdu->sessionId) &&
        grilio_parser_get_int32(&parser, &apdu->cla) &&
        grilio_parser_get_int32(&parser, &apdu->instruction) &&
        grilio_parser_get_int32(&parser, &apdu->p1) &&
        grilio_parser_get_int32(&parser, &apdu->p2) &&
        grilio_parser_get_int32(&parser, &apdu->p3) &&
        grilio_parser_get_nullable_utf8(&parser, &data)) {
        GBinderWriter writer;
        guint parent;

        /* Initialize the writer and the data to be written */
        gbinder_local_request_init_writer(out, &writer);
        gbinder_local_request_cleanup(out, g_free, apdu);
        ril_binder_radio_take_string(out, &apdu->data, data);

        /* Write the arguments */
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));
        parent = gbinder_writer_append_buffer_object(&writer, apdu,
            sizeof(*apdu));
        ril_binder_radio_write_hidl_string_data(&writer, apdu, data, parent);
        return TRUE;
    }

    g_free(apdu);
    return FALSE;
}

/*==========================================================================*
 * Decoders (binder -> plugin)
 *==========================================================================*/

static
gboolean
ril_binder_radio_decode_int32(
    GBinderReader* in,
    GByteArray* out)
{
    gint32 value;

    if (gbinder_reader_read_int32(in, &value)) {
        grilio_encode_int32(out, value);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_decode_int_1(
    GBinderReader* in,
    GByteArray* out)
{
    gint32 value;

    if (gbinder_reader_read_int32(in, &value)) {
        grilio_encode_int32(out, 1);
        grilio_encode_int32(out, value);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_decode_int_2(
    GBinderReader* in,
    GByteArray* out)
{
    gint32 values[2];

    if (gbinder_reader_read_int32(in, values + 0) &&
        gbinder_reader_read_int32(in, values + 1)) {
        grilio_encode_int32(out, 2);
        grilio_encode_int32(out, values[0]);
        grilio_encode_int32(out, values[1]);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_decode_bool_to_int_array(
    GBinderReader* in,
    GByteArray* out)
{
    gint32 value;

    if (gbinder_reader_read_bool(in, &value)) {
        grilio_encode_int32(out, 1);
        grilio_encode_int32(out, value);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_decode_string(
    GBinderReader* in,
    GByteArray* out)
{
    const char* str = gbinder_reader_read_hidl_string_c(in);

    if (str) {
        grilio_encode_utf8(out, str);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_decode_string_n(
    GBinderReader* in,
    GByteArray* out,
    guint n)
{
    guint i;

    grilio_encode_int32(out, n);
    for (i = 0; i < n; i++) {
        const char* str = gbinder_reader_read_hidl_string_c(in);

        if (str) {
            grilio_encode_utf8(out, str);
        } else {
            return FALSE;
        }
    }
    return TRUE;
}

static
gboolean
ril_binder_radio_decode_string_3(
    GBinderReader* in,
    GByteArray* out)
{
    return ril_binder_radio_decode_string_n(in, out, 3);
}

static
gboolean
ril_binder_radio_decode_int_array(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize n = 0;
    const gint32* values = gbinder_reader_read_hidl_type_vec(in, gint32, &n);

    if (values) {
        guint i;

        grilio_encode_int32(out, n);
        for (i = 0; i < n; i++) {
            grilio_encode_int32(out, values[i]);
        }
        ok = TRUE;
    }
    return ok;
}

static
gboolean
ril_binder_radio_decode_byte_array(
    GBinderReader* in,
    GByteArray* out)
{
    gsize size = 0;
    const guint8* ptr = gbinder_reader_read_hidl_byte_vec(in, &size);

    if (ptr) {
        g_byte_array_append(out, ptr, size);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_decode_byte_array_to_hex(
    GBinderReader* in,
    GByteArray* out)
{
    gsize size = 0;
    const guint8* bytes = gbinder_reader_read_hidl_byte_vec(in, &size);

    if (bytes) {
        static const char hex[] = "0123456789ABCDEF";
        char* str = g_new(char, 2*size);
        char* ptr = str;
        guint i;

        for (i = 0; i < size; i++) {
            const guint8 b = bytes[i];

            *ptr++ = hex[(b >> 4) & 0xf];
            *ptr++ = hex[b & 0xf];
        }

        grilio_encode_utf8_chars(out, str, 2*size);
        g_free(str);
        return TRUE;
    }
    return FALSE;
}

static
void
ril_binder_decode_vec_utf8_as_string(
    GByteArray* out,
    const GBinderHidlVec *vec,
    const char *separator)
{
    const GBinderHidlString* elem = vec->data.ptr;
    char str[256];
    gchar *p = str;
    int i;

    bzero(str, sizeof(str));
    for (i = 0; i < vec->count; i++) {
        if (i) {
            p += g_strlcat(p, separator, sizeof(str) - (p - str));
        }
        p += g_strlcat(p, elem->data.str, sizeof(str) - (p - str));
        elem++;
    }
    grilio_encode_utf8(out, str);
}

static
void
ril_binder_radio_decode_data_call_1_4(
    GByteArray* out,
    const RadioDataCall_1_4* call)
{
    grilio_encode_int32(out, call->cause);
    grilio_encode_int32(out, call->suggestedRetryTime);
    grilio_encode_int32(out, call->cid);
    grilio_encode_int32(out, call->active);
    grilio_encode_utf8(out, radio_pdp_protocol_type_to_str(call->type));
    grilio_encode_utf8(out, call->ifname.data.str);
    ril_binder_decode_vec_utf8_as_string(out, &call->addresses, " ");
    ril_binder_decode_vec_utf8_as_string(out, &call->dnses, " ");
    ril_binder_decode_vec_utf8_as_string(out, &call->gateways, " ");
    ril_binder_decode_vec_utf8_as_string(out, &call->pcscf, " ");
    grilio_encode_int32(out, call->mtu);
}

/**
 * @param cardStatus ICC card status as defined by CardStatus in types.hal
 */
static
void
ril_binder_radio_decode_icc_card_status(
    const RadioCardStatus* sim,
    GByteArray* out)
{
    const RadioAppStatus* apps = sim->apps.data.ptr;
    guint i;

    grilio_encode_int32(out, sim->cardState);
    grilio_encode_int32(out, sim->universalPinState);
    grilio_encode_int32(out, sim->gsmUmtsSubscriptionAppIndex);
    grilio_encode_int32(out, sim->cdmaSubscriptionAppIndex);
    grilio_encode_int32(out, sim->imsSubscriptionAppIndex);
    grilio_encode_int32(out, sim->apps.count);

    for (i = 0; i < sim->apps.count; i++) {
        const RadioAppStatus* app = apps + i;

        grilio_encode_int32(out, app->appType);
        grilio_encode_int32(out, app->appState);
        grilio_encode_int32(out, app->persoSubstate);
        grilio_encode_utf8(out, app->aid.data.str);
        grilio_encode_utf8(out, app->label.data.str);
        grilio_encode_int32(out, app->pinReplaced);
        grilio_encode_int32(out, app->pin1);
        grilio_encode_int32(out, app->pin2);
    }
}

static
gboolean
ril_binder_radio_decode_icc_card_status_1_0(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    const RadioCardStatus* sim = gbinder_reader_read_hidl_struct
        (in, RadioCardStatus);

    if (sim) {
        ril_binder_radio_decode_icc_card_status(sim, out);
        ok = TRUE;
    }
    return ok;
}

/**
 * @param cardStatus ICC card status as defined by CardStatus in types.hal
 */
static
gboolean
ril_binder_radio_decode_icc_card_status_1_2(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    const RadioCardStatus_1_2* sim = gbinder_reader_read_hidl_struct
        (in, RadioCardStatus_1_2);

    if (sim) {
        ril_binder_radio_decode_icc_card_status(&sim->base, out);
        ok = TRUE;
    }
    return ok;
}

/**
 * @param cardStatus ICC card status as defined by CardStatus in types.hal
 */
static
gboolean
ril_binder_radio_decode_icc_card_status_1_4(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    const RadioCardStatus_1_4* sim = gbinder_reader_read_hidl_struct
        (in, RadioCardStatus_1_4);

    if (sim) {
        ril_binder_radio_decode_icc_card_status(&sim->base, out);
        ok = TRUE;
    }
    return ok;
}

/**
 * @param voiceRegResponse VoiceRegStateResult defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_voice_reg_state(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioVoiceRegStateResult* reg = gbinder_reader_read_hidl_struct
        (in, RadioVoiceRegStateResult);

    if (reg) {
        grilio_encode_int32(out, 5);
        grilio_encode_format(out, "%d", reg->regState);
        grilio_encode_utf8(out, ""); /* slac */
        grilio_encode_utf8(out, ""); /* sci */
        grilio_encode_format(out, "%d", reg->rat);
        grilio_encode_format(out, "%d", reg->reasonForDenial);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param dataRegResponse DataRegStateResult defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_data_reg_state(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioDataRegStateResult* reg = gbinder_reader_read_hidl_struct
        (in, RadioDataRegStateResult);

    if (reg) {
        grilio_encode_int32(out, 6);
        grilio_encode_format(out, "%d", reg->regState);
        grilio_encode_utf8(out, ""); /* slac */
        grilio_encode_utf8(out, ""); /* sci */
        grilio_encode_format(out, "%d", reg->rat);
        grilio_encode_format(out, "%d", reg->reasonDataDenied);
        grilio_encode_format(out, "%d", reg->maxDataCalls);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param dataRegResponse DataRegStateResult defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_data_reg_state_1_4(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioDataRegStateResult_1_4* reg = gbinder_reader_read_hidl_struct
        (in, RadioDataRegStateResult_1_4);

    if (reg) {
        grilio_encode_int32(out, 6);
        grilio_encode_format(out, "%d", reg->regState);
        grilio_encode_utf8(out, ""); /* slac */
        grilio_encode_utf8(out, ""); /* sci */
        grilio_encode_format(out, "%d", reg->rat);
        grilio_encode_format(out, "%d", reg->reasonDataDenied);
        grilio_encode_format(out, "%d", reg->maxDataCalls);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param sms Response to sms sent as defined by SendSmsResult in types.hal
*/
static
gboolean
ril_binder_radio_decode_sms_send_result(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioSendSmsResult* result = gbinder_reader_read_hidl_struct
        (in, RadioSendSmsResult);

    if (result) {
        grilio_encode_int32(out, result->messageRef);
        grilio_encode_utf8(out, result->ackPDU.data.str);
        grilio_encode_int32(out, result->errorCode);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param iccIo ICC io operation response defined by IccIoResult in types.hal
 */
static
gboolean
ril_binder_radio_decode_icc_io_result(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioIccIoResult* result = gbinder_reader_read_hidl_struct
        (in, RadioIccIoResult);

    if (result) {
        grilio_encode_int32(out, result->sw1);
        grilio_encode_int32(out, result->sw2);
        grilio_encode_utf8(out, result->response.data.str);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param callForwardInfos points to a vector of CallForwardInfo, one for
 *        each distinct registered phone number.
 */
static
gboolean
ril_binder_radio_decode_call_forward_info_array(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize count = 0;
    const RadioCallForwardInfo* infos = gbinder_reader_read_hidl_type_vec
        (in, RadioCallForwardInfo, &count);

    if (infos) {
        guint i;

        grilio_encode_int32(out, count);
        for (i = 0; i < count; i++) {
            const RadioCallForwardInfo* info = infos + i;

            grilio_encode_int32(out, info->status);
            grilio_encode_int32(out, info->reason);
            grilio_encode_int32(out, info->serviceClass);
            grilio_encode_int32(out, info->toa);
            grilio_encode_utf8(out, info->number.data.str);
            grilio_encode_int32(out, info->timeSeconds);
        }
        ok = TRUE;
    }
    return ok;
}

/**
 * @param calls Current call list
 */
static
void
ril_binder_radio_decode_call(
    const RadioCall* call,
    GByteArray* out)
{
    grilio_encode_int32(out, call->state);
    grilio_encode_int32(out, call->index);
    grilio_encode_int32(out, call->toa);
    grilio_encode_int32(out, call->isMpty);
    grilio_encode_int32(out, call->isMT);
    grilio_encode_int32(out, call->als);
    grilio_encode_int32(out, call->isVoice);
    grilio_encode_int32(out, call->isVoicePrivacy);
    grilio_encode_utf8(out, call->number.data.str);
    grilio_encode_int32(out, call->numberPresentation);
    grilio_encode_utf8(out, call->name.data.str);
    grilio_encode_int32(out, call->namePresentation);
    grilio_encode_int32(out, 0);  /* uusInfo */
}

static
gboolean
ril_binder_radio_decode_call_list(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize count = 0;
    const RadioCall* calls = gbinder_reader_read_hidl_type_vec
        (in, RadioCall, &count);

    if (calls) {
        guint i;

        grilio_encode_int32(out, count);
        for (i = 0; i < count; i++) {
            ril_binder_radio_decode_call(calls + i, out);
        }
        ok = TRUE;
    }
    return ok;
}

/**
 * @param calls Current call list
 */
static
gboolean
ril_binder_radio_decode_call_list_1_2(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize count = 0;
    const RadioCall_1_2* calls = gbinder_reader_read_hidl_type_vec
        (in, RadioCall_1_2, &count);

    if (calls) {
        guint i;

        grilio_encode_int32(out, count);
        for (i = 0; i < count; i++) {
            ril_binder_radio_decode_call(&calls[i].base, out);
        }
        ok = TRUE;
    }
    return ok;
}

static
gboolean
ril_binder_radio_decode_last_call_fail_cause(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioLastCallFailCauseInfo* info = gbinder_reader_read_hidl_struct
        (in, RadioLastCallFailCauseInfo);

    if (info) {
        grilio_encode_int32(out, info->causeCode);
        grilio_encode_utf8(out, info->vendorCause.data.str);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param enable whether current call waiting state is enabled or disabled
 * @param serviceClass If enable, then callWaitingResp[1]
 *        must follow, with the TS 27.007 service class bit vector of services
 *        for which call waiting is enabled.
 *        For example, if callWaitingResp[0] is 1 and
 *        callWaitingResp[1] is 3, then call waiting is enabled for data
 *        and voice and disabled for everything else.
 */
static
gboolean
ril_binder_radio_decode_call_waiting(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean enable;
    gint32 serviceClass;

    if (gbinder_reader_read_bool(in, &enable) &&
        gbinder_reader_read_int32(in, &serviceClass)) {
        grilio_encode_int32(out, 2);
        grilio_encode_int32(out, enable);
        grilio_encode_int32(out, serviceClass);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param networkInfos List of network operator information as OperatorInfos
 *                     defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_operator_info_list(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize count = 0;
    const RadioOperatorInfo* ops = gbinder_reader_read_hidl_type_vec
        (in, RadioOperatorInfo, &count);

    if (ops) {
        guint i;

        /* 4 strings per operator */
        grilio_encode_int32(out, 4*count);
        for (i = 0; i < count; i++) {
            const RadioOperatorInfo* op = ops + i;

            grilio_encode_utf8(out, op->alphaLong.data.str);
            grilio_encode_utf8(out, op->alphaShort.data.str);
            grilio_encode_utf8(out, op->operatorNumeric.data.str);
            grilio_encode_utf8(out,
                (op->status == RADIO_OP_AVAILABLE) ? "available" :
                (op->status == RADIO_OP_CURRENT) ? "current" :
                (op->status == RADIO_OP_FORBIDDEN) ? "forbidden" : "unknown");
        }
        ok = TRUE;
    }
    return ok;
}

/**
 * @param dcResponse List of SetupDataCallResult as defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_data_call_list(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize count = 0;
    const RadioDataCall* calls = gbinder_reader_read_hidl_type_vec
        (in, RadioDataCall, &count);

    if (calls) {
        guint i;

        grilio_encode_int32(out, DATA_CALL_VERSION);
        grilio_encode_int32(out, count);
        for (i = 0; i < count; i++) {
            ril_binder_radio_decode_data_call(out, calls + i);
        }
        ok = TRUE;
    }
    return ok;
}

/**
 * @param dcResponse List of SetupDataCallResult as defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_data_call_list_1_4(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize count = 0;
    const RadioDataCall_1_4* calls = gbinder_reader_read_hidl_type_vec
        (in, RadioDataCall_1_4, &count);

    if (calls) {
        guint i;

        grilio_encode_int32(out, DATA_CALL_VERSION);
        grilio_encode_int32(out, count);
        for (i = 0; i < count; i++) {
            ril_binder_radio_decode_data_call_1_4(out, calls + i);
        }
        ok = TRUE;
    }
    return ok;
}

/**
 * @param dcResponse SetupDataCallResult defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_setup_data_call_result(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioDataCall* call = gbinder_reader_read_hidl_struct
        (in, RadioDataCall);

    if (call) {
        grilio_encode_int32(out, DATA_CALL_VERSION);
        grilio_encode_int32(out, 1);
        ril_binder_radio_decode_data_call(out, call);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param dcResponse SetupDataCallResult defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_setup_data_call_result_1_4(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioDataCall_1_4* call = gbinder_reader_read_hidl_struct
        (in, RadioDataCall_1_4);

    if (call) {
        grilio_encode_int32(out, DATA_CALL_VERSION);
        grilio_encode_int32(out, 1);
        ril_binder_radio_decode_data_call_1_4(out, call);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param nwType RadioPreferredNetworkType defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_pref_network_type(
    GBinderReader* in,
    GByteArray* out)
{
    gint32 pref;

    if (gbinder_reader_read_int32(in, &pref)) {
        grilio_encode_int32(out, 1);
        grilio_encode_int32(out, pref);
        return TRUE;
    }
    return FALSE;
}

static
gint32
ril_binder_radio_raf_to_pref_mode(
    gint32 raf)
{
    gint32 gen = 0;

#define RADIO_GEN_2G (RAF_GSM | RAF_GPRS | RAF_EDGE)
#define RADIO_GEN_3G (RAF_UMTS | RAF_HSDPA | RAF_HSUPA | RAF_HSPA | RAF_HSPAP)
#define RADIO_GEN_4G (RAF_LTE | RAF_LTE_CA)

    gen |= (raf & RADIO_GEN_2G) ? RADIO_GEN_2G : 0;
    gen |= (raf & RADIO_GEN_3G) ? RADIO_GEN_3G : 0;
    gen |= (raf & RADIO_GEN_4G) ? RADIO_GEN_4G : 0;

    switch (gen) {
    case RADIO_GEN_2G:
        return PREF_NET_TYPE_GSM_ONLY;
    case RADIO_GEN_2G | RADIO_GEN_3G:
        return PREF_NET_TYPE_GSM_WCDMA;
    case RADIO_GEN_2G | RADIO_GEN_3G | RADIO_GEN_4G:
        return PREF_NET_TYPE_LTE_GSM_WCDMA;
    case RADIO_GEN_3G | RADIO_GEN_4G:
        return PREF_NET_TYPE_LTE_WCDMA;
    case RADIO_GEN_4G:
        return PREF_NET_TYPE_LTE_ONLY;
    default:
        /* Other combinations are not yet supported */
        return PREF_NET_TYPE_GSM_ONLY;
    }
#undef RADIO_GEN_2G
#undef RADIO_GEN_3G
#undef RADIO_GEN_4G
}

/**
 * @param networkTypeBitmap a 32-bit bitmap of RadioAccessFamily
 */
static
gboolean
ril_binder_radio_decode_pref_network_type_bitmap(
    GBinderReader* in,
    GByteArray* out)
{
    gint32 raf, pref;

    if (gbinder_reader_read_int32(in, &raf)) {
        pref = ril_binder_radio_raf_to_pref_mode(raf);
        grilio_encode_int32(out, 1);
        grilio_encode_int32(out, pref);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param configs Vector of GSM/WCDMA Cell broadcast configs
 */
static
gboolean
ril_binder_radio_decode_gsm_broadcast_sms_config(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize n = 0;
    const RadioGsmBroadcastSmsConfig* configs =
        gbinder_reader_read_hidl_type_vec(in, RadioGsmBroadcastSmsConfig, &n);

    if (configs) {
        guint i;

        grilio_encode_int32(out, n);
        for (i = 0; i < n; i++) {
            const RadioGsmBroadcastSmsConfig* config = configs + i;

            grilio_encode_int32(out, config->fromServiceId);
            grilio_encode_int32(out, config->toServiceId);
            grilio_encode_int32(out, config->fromCodeScheme);
            grilio_encode_int32(out, config->toCodeScheme);
            grilio_encode_int32(out, config->selected);
        }
        ok = TRUE;
    }
    return ok;
}

/**
 * @param imei IMEI if GSM subscription is available
 * @param imeisv IMEISV if GSM subscription is available
 * @param esn ESN if CDMA subscription is available
 * @param meid MEID if CDMA subscription is available
 */
static
gboolean
ril_binder_radio_decode_device_identity(
    GBinderReader* in,
    GByteArray* out)
{
    const char* imei = gbinder_reader_read_hidl_string_c(in);
    const char* imeisv = gbinder_reader_read_hidl_string_c(in);
    const char* esn = gbinder_reader_read_hidl_string_c(in);
    const char* meid = gbinder_reader_read_hidl_string_c(in);

    if (imei || imeisv || esn || meid) {
        grilio_encode_int32(out, 4);
        grilio_encode_utf8(out, imei);
        grilio_encode_utf8(out, imeisv);
        grilio_encode_utf8(out, esn);
        grilio_encode_utf8(out, meid);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param modeType USSD type code
 * @param msg Message string in UTF-8, if applicable
 */
static
gboolean
ril_binder_radio_decode_ussd(
    GBinderReader* in,
    GByteArray* out)
{
    guint32 code;

    if (gbinder_reader_read_uint32(in, &code)) {
        const char* msg = gbinder_reader_read_hidl_string_c(in);

        grilio_encode_int32(out, 2);
        grilio_encode_format(out, "%u", code);
        grilio_encode_utf8(out, msg);
        return TRUE;
    }
    return FALSE;
}

static
void
ril_binder_radio_decode_signal_strength_common(
    const RadioSignalStrengthGsm* gsm,
    const RadioSignalStrengthCdma* cdma,
    const RadioSignalStrengthEvdo* evdo,
    const RadioSignalStrengthLte* lte,
    const RadioSignalStrengthTdScdma* tdScdma,
    const RadioSignalStrengthWcdma* wcdma,
    GByteArray* out)
{
    /* GW_SignalStrength */
    if (wcdma && wcdma->signalStrength <= 31 && gsm->signalStrength > 31) {
        /*
         * Presumably, 3G signal. The wcdma field did't exist in RIL
         * socket times.
         *
         * Valid signal strength values for both 2G and 3G are (0-31, 99)
         * as defined in TS 27.007 8.5
         */
        grilio_encode_int32(out, wcdma->signalStrength);
        grilio_encode_int32(out, wcdma->bitErrorRate);
    } else {
        grilio_encode_int32(out, gsm->signalStrength);
        grilio_encode_int32(out, gsm->bitErrorRate);
    }

    /* CDMA_SignalStrength */
    grilio_encode_int32(out, cdma->dbm);
    grilio_encode_int32(out, cdma->ecio);

    /* EVDO_SignalStrength */
    grilio_encode_int32(out, evdo->dbm);
    grilio_encode_int32(out, evdo->ecio);
    grilio_encode_int32(out, evdo->signalNoiseRatio);

        /* LTE_SignalStrength_v8 */
    grilio_encode_int32(out, lte->signalStrength);
    grilio_encode_int32(out, lte->rsrp);
    grilio_encode_int32(out, lte->rsrq);
    grilio_encode_int32(out, lte->rssnr);
    grilio_encode_int32(out, lte->cqi);
    grilio_encode_int32(out, lte->timingAdvance);

    /* TD_SCDMA_SignalStrength */
    grilio_encode_int32(out, tdScdma->rscp);
}


/**
 * @param signalStrength SignalStrength information as defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_signal_strength(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioSignalStrength* strength = gbinder_reader_read_hidl_struct
        (in, RadioSignalStrength);

    if (strength) {
        RadioSignalStrengthTdScdma tdscdma;

        tdscdma.rscp = strength->tdScdma.rscp;
        ril_binder_radio_decode_signal_strength_common(&strength->gw,
                &strength->cdma, &strength->evdo, &strength->lte,
                &tdscdma, NULL, out);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param signalStrength SignalStrength information as defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_signal_strength_1_2(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioSignalStrength_1_2* strength = gbinder_reader_read_hidl_struct
        (in, RadioSignalStrength_1_2);

    if (strength) {
        RadioSignalStrengthTdScdma tdscdma;

        tdscdma.rscp = strength->tdScdma.rscp;
        ril_binder_radio_decode_signal_strength_common(&strength->gw,
                &strength->cdma, &strength->evdo, &strength->lte,
                &tdscdma, &strength->wcdma.base, out);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param signalStrength SignalStrength information as defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_signal_strength_1_4(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioSignalStrength_1_4* strength = gbinder_reader_read_hidl_struct
        (in, RadioSignalStrength_1_4);

    if (strength) {
        RadioSignalStrengthTdScdma tdscdma;

        tdscdma.rscp = strength->tdscdma.rscp;
        ril_binder_radio_decode_signal_strength_common(&strength->gsm,
                &strength->cdma, &strength->evdo, &strength->lte,
                &tdscdma, &strength->wcdma.base, out);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param suppSvc SuppSvcNotification as defined in types.hal
 */
static
gboolean
ril_binder_radio_decode_supp_svc_notification(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioSuppSvcNotification* notify = gbinder_reader_read_hidl_struct
        (in, RadioSuppSvcNotification);

    if (notify) {
        grilio_encode_int32(out, notify->isMT);
        grilio_encode_int32(out, notify->code);
        grilio_encode_int32(out, notify->index);
        grilio_encode_int32(out, notify->type);
        grilio_encode_utf8(out, notify->number.data.str);
        return TRUE;
    }
    return TRUE;
}

/**
 * @param refreshResult Result of sim refresh
 */
static
gboolean
ril_binder_radio_decode_sim_refresh(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioSimRefresh* refresh = gbinder_reader_read_hidl_struct
        (in, RadioSimRefresh);

    if (refresh) {
        grilio_encode_int32(out, refresh->type);
        grilio_encode_int32(out, refresh->efId);
        grilio_encode_utf8(out, refresh->aid.data.str);
        return TRUE;
    }
    return FALSE;
}

static
void
ril_binder_radio_decode_cell_info_header(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    grilio_encode_int32(out, cell->cellInfoType);
    grilio_encode_int32(out, cell->registered);
    grilio_encode_int32(out, cell->timeStampType);
    /* There should be grilio_encode_int64() call below (there's no
     * such function in libgrilio) but the timestamp value is ignored
     * anyway, so who cares... */
    grilio_encode_bytes(out, &cell->timeStamp, sizeof(cell->timeStamp));
}

static
void
ril_binder_radio_decode_cell_info_gsm(
    GByteArray* out,
    const RadioCellIdentityGsm* id,
    const RadioSignalStrengthGsm* ss)
{
    int mcc, mnc;

    if (!gutil_parse_int(id->mcc.data.str, 10, &mcc)) {
        mcc = RADIO_CELL_INVALID_VALUE;
    }
    if (!gutil_parse_int(id->mnc.data.str, 10, &mnc)) {
        mnc = RADIO_CELL_INVALID_VALUE;
    }
    grilio_encode_int32(out, mcc);
    grilio_encode_int32(out, mnc);
    grilio_encode_int32(out, id->lac);
    grilio_encode_int32(out, id->cid);
    grilio_encode_int32(out, id->arfcn);
    grilio_encode_int32(out, id->bsic);
    grilio_encode_int32(out, ss->signalStrength);
    grilio_encode_int32(out, ss->bitErrorRate);
    grilio_encode_int32(out, ss->timingAdvance);
}

static
void
ril_binder_radio_decode_cell_info_gsm_1_0(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    const RadioCellInfoGsm* info =  cell->gsm.data.ptr;
    guint i, count = cell->gsm.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header(out, cell);
        ril_binder_radio_decode_cell_info_gsm(out,
            &info[i].cellIdentityGsm,
            &info[i].signalStrengthGsm);
    }
}

static
void
ril_binder_radio_decode_cell_info_cdma(
    GByteArray* out,
    const RadioCellIdentityCdma* id,
    const RadioSignalStrengthCdma* ss,
    const RadioSignalStrengthEvdo* evdo)
{
    grilio_encode_int32(out, id->networkId);
    grilio_encode_int32(out, id->systemId);
    grilio_encode_int32(out, id->baseStationId);
    grilio_encode_int32(out, id->longitude);
    grilio_encode_int32(out, id->latitude);
    grilio_encode_int32(out, ss->dbm);
    grilio_encode_int32(out, ss->ecio);
    grilio_encode_int32(out, evdo->dbm);
    grilio_encode_int32(out, evdo->ecio);
    grilio_encode_int32(out, evdo->signalNoiseRatio);
}

static
void
ril_binder_radio_decode_cell_info_cdma_1_0(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    const RadioCellInfoCdma* info = cell->cdma.data.ptr;
    guint i, count = cell->cdma.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header(out, cell);
        ril_binder_radio_decode_cell_info_cdma(out,
            &info[i].cellIdentityCdma,
            &info[i].signalStrengthCdma,
            &info[i].signalStrengthEvdo);
    }
}

static
void
ril_binder_radio_decode_cell_info_lte(
    GByteArray* out,
    const RadioCellIdentityLte* id,
    const RadioSignalStrengthLte* ss)
{
    int mcc, mnc;

    if (!gutil_parse_int(id->mcc.data.str, 10, &mcc)) {
        mcc = RADIO_CELL_INVALID_VALUE;
    }
    if (!gutil_parse_int(id->mnc.data.str, 10, &mnc)) {
        mnc = RADIO_CELL_INVALID_VALUE;
    }
    grilio_encode_int32(out, mcc);
    grilio_encode_int32(out, mnc);
    grilio_encode_int32(out, id->ci);
    grilio_encode_int32(out, id->pci);
    grilio_encode_int32(out, id->tac);
    grilio_encode_int32(out, id->earfcn);
    grilio_encode_int32(out, ss->signalStrength);
    grilio_encode_int32(out, ss->rsrp);
    grilio_encode_int32(out, ss->rsrq);
    grilio_encode_int32(out, ss->rssnr);
    grilio_encode_int32(out, ss->cqi);
    grilio_encode_int32(out, ss->timingAdvance);
}

static
void
ril_binder_radio_decode_cell_info_lte_1_0(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    const RadioCellInfoLte* info = cell->lte.data.ptr;
    guint i, count = cell->lte.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header(out, cell);
        ril_binder_radio_decode_cell_info_lte(out,
            &info[i].cellIdentityLte,
            &info[i].signalStrengthLte);
    }
}

static
void
ril_binder_radio_decode_cell_info_wcdma(
    GByteArray* out,
    const RadioCellIdentityWcdma* id,
    const RadioSignalStrengthWcdma* ss)
{
    int mcc, mnc;

    if (!gutil_parse_int(id->mcc.data.str, 10, &mcc)) {
        mcc = RADIO_CELL_INVALID_VALUE;
    }
    if (!gutil_parse_int(id->mnc.data.str, 10, &mnc)) {
        mnc = RADIO_CELL_INVALID_VALUE;
    }
    grilio_encode_int32(out, mcc);
    grilio_encode_int32(out, mnc);
    grilio_encode_int32(out, id->lac);
    grilio_encode_int32(out, id->cid);
    grilio_encode_int32(out, id->psc);
    grilio_encode_int32(out, id->uarfcn);
    grilio_encode_int32(out, ss->signalStrength);
    grilio_encode_int32(out, ss->bitErrorRate);
}

static
void
ril_binder_radio_decode_cell_info_wcdma_1_0(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    const RadioCellInfoWcdma* info = cell->wcdma.data.ptr;
    guint i, count = cell->wcdma.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header(out, cell);
        ril_binder_radio_decode_cell_info_wcdma(out,
            &info[i].cellIdentityWcdma,
            &info[i].signalStrengthWcdma);
    }
}

static
void
ril_binder_radio_decode_cell_info_tdscdma(
    GByteArray* out,
    const RadioCellIdentityTdscdma* id,
    guint32 rscp)
{
    int mcc = RADIO_CELL_INVALID_VALUE;
    int mnc = RADIO_CELL_INVALID_VALUE;

    gutil_parse_int(id->mcc.data.str, 10, &mcc);
    gutil_parse_int(id->mnc.data.str, 10, &mnc);
    grilio_encode_int32(out, mcc);
    grilio_encode_int32(out, mnc);
    grilio_encode_int32(out, id->lac);
    grilio_encode_int32(out, id->cid);
    grilio_encode_int32(out, id->cpid);
    grilio_encode_int32(out, rscp);
}

static
void
ril_binder_radio_decode_cell_info_tdscdma_1_0(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    const RadioCellInfoTdscdma* info = cell->tdscdma.data.ptr;
    guint i, count = cell->tdscdma.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header(out, cell);
        ril_binder_radio_decode_cell_info_tdscdma(out,
            &info[i].cellIdentityTdscdma,
            info[i].signalStrengthTdscdma.rscp);
    }
}

static
void
ril_binder_radio_decode_cell_info_header_1_2(
    GByteArray* out,
    const RadioCellInfo_1_2* cell)
{
    grilio_encode_int32(out, cell->cellInfoType);
    grilio_encode_int32(out, cell->registered);
    grilio_encode_int32(out, cell->timeStampType);
    /* There should be grilio_encode_int64() call below (there's no
     * such function in libgrilio) but the timestamp value is ignored
     * anyway, so who cares... */
    grilio_encode_bytes(out, &cell->timeStamp, sizeof(cell->timeStamp));
}

static
void
ril_binder_radio_decode_cell_info_header_1_4(
    GByteArray* out,
    const RadioCellInfo_1_4* cell,
    const RADIO_CELL_INFO_TYPE cellInfoType)
{
    grilio_encode_int32(out, cellInfoType);
    grilio_encode_int32(out, cell->registered);
    grilio_encode_int32(out, 0); /* timeStampType */
    grilio_encode_int32(out, 0); /* timeStamp */
    grilio_encode_int32(out, 0); /* timeStamp */
}

static
void
ril_binder_radio_decode_cell_info_gsm_1_2(
    GByteArray* out,
    const RadioCellInfo_1_2* cell)
{
    const RadioCellInfoGsm_1_2* info =  cell->gsm.data.ptr;
    guint i, count = cell->gsm.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header_1_2(out, cell);
        ril_binder_radio_decode_cell_info_gsm(out,
            &info[i].cellIdentityGsm.base,
            &info[i].signalStrengthGsm);
    }
}

static
void
ril_binder_radio_decode_cell_info_cdma_1_2(
    GByteArray* out,
    const RadioCellInfo_1_2* cell)
{
    const RadioCellInfoCdma_1_2* info = cell->cdma.data.ptr;
    guint i, count = cell->cdma.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header_1_2(out, cell);
        ril_binder_radio_decode_cell_info_cdma(out,
            &info[i].cellIdentityCdma.base,
            &info[i].signalStrengthCdma,
            &info[i].signalStrengthEvdo);
    }
}

static
void
ril_binder_radio_decode_cell_info_lte_1_2(
    GByteArray* out,
    const RadioCellInfo_1_2* cell)
{
    const RadioCellInfoLte_1_2* info = cell->lte.data.ptr;
    guint i, count = cell->lte.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header_1_2(out, cell);
        ril_binder_radio_decode_cell_info_lte(out,
            &info[i].cellIdentityLte.base,
            &info[i].signalStrengthLte);
    }
}

static
void
ril_binder_radio_decode_cell_info_wcdma_1_2(
    GByteArray* out,
    const RadioCellInfo_1_2* cell)
{
    const RadioCellInfoWcdma_1_2* info = cell->wcdma.data.ptr;
    guint i, count = cell->wcdma.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header_1_2(out, cell);
        ril_binder_radio_decode_cell_info_wcdma(out,
            &info[i].cellIdentityWcdma.base,
            &info[i].signalStrengthWcdma.base);
    }
}

static
void
ril_binder_radio_decode_cell_info_tdscdma_1_2(
    GByteArray* out,
    const RadioCellInfo_1_2* cell)
{
    const RadioCellInfoTdscdma_1_2* info = cell->tdscdma.data.ptr;
    guint i, count = cell->tdscdma.count;

    for (i = 0; i < count; i++) {
        ril_binder_radio_decode_cell_info_header_1_2(out, cell);
        ril_binder_radio_decode_cell_info_tdscdma(out,
            &info[i].cellIdentityTdscdma.base,
            info[i].signalStrengthTdscdma.rscp);
    }
}

/**
 * @param cellInfo List of current cell information known to radio
 */
static
gboolean
ril_binder_radio_decode_cell_info_list(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize count = 0;
    const RadioCellInfo* cells = gbinder_reader_read_hidl_type_vec
        (in, RadioCellInfo, &count);

    if (cells) {
        guint i, n = 0;

        /* Count supported types */
        for (i = 0; i < count; i++) {
            const RadioCellInfo* cell = cells + i;

            switch (cells[i].cellInfoType) {
            case RADIO_CELL_INFO_GSM:
                n += cell->gsm.count;
                break;
            case RADIO_CELL_INFO_CDMA:
                n += cell->cdma.count;
                break;
            case RADIO_CELL_INFO_LTE:
                n += cell->lte.count;
                break;
            case RADIO_CELL_INFO_WCDMA:
                n += cell->wcdma.count;
                break;
            case RADIO_CELL_INFO_TD_SCDMA:
                n += cell->tdscdma.count;
                break;
            }
        }

        grilio_encode_int32(out, n);

        for (i = 0; i < count; i++) {
            const RadioCellInfo* cell = cells + i;

            switch (cell->cellInfoType) {
            case RADIO_CELL_INFO_GSM:
                ril_binder_radio_decode_cell_info_gsm_1_0(out, cell);
                break;
            case RADIO_CELL_INFO_CDMA:
                ril_binder_radio_decode_cell_info_cdma_1_0(out, cell);
                break;
            case RADIO_CELL_INFO_LTE:
                ril_binder_radio_decode_cell_info_lte_1_0(out, cell);
                break;
            case RADIO_CELL_INFO_WCDMA:
                ril_binder_radio_decode_cell_info_wcdma_1_0(out, cell);
                break;
            case RADIO_CELL_INFO_TD_SCDMA:
                ril_binder_radio_decode_cell_info_tdscdma_1_0(out, cell);
                break;
            }
        }
        ok = TRUE;
    }
    return ok;
}

/**
 * @param cellInfo List of current cell information known to radio
 */
static
gboolean
ril_binder_radio_decode_cell_info_list_1_2(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize count = 0;
    const RadioCellInfo_1_2* cells = gbinder_reader_read_hidl_type_vec
        (in, RadioCellInfo_1_2, &count);

    if (cells) {
        guint i, n = 0;

        /* Count supported types */
        for (i = 0; i < count; i++) {
            const RadioCellInfo_1_2* cell = cells + i;

            switch (cells[i].cellInfoType) {
            case RADIO_CELL_INFO_GSM:
                n += cell->gsm.count;
                break;
            case RADIO_CELL_INFO_CDMA:
                n += cell->cdma.count;
                break;
            case RADIO_CELL_INFO_LTE:
                n += cell->lte.count;
                break;
            case RADIO_CELL_INFO_WCDMA:
                n += cell->wcdma.count;
                break;
            case RADIO_CELL_INFO_TD_SCDMA:
                n += cell->tdscdma.count;
                break;
            }
        }

        grilio_encode_int32(out, n);

        for (i = 0; i < count; i++) {
            const RadioCellInfo_1_2* cell = cells + i;

            switch (cell->cellInfoType) {
            case RADIO_CELL_INFO_GSM:
                ril_binder_radio_decode_cell_info_gsm_1_2(out, cell);
                break;
            case RADIO_CELL_INFO_CDMA:
                ril_binder_radio_decode_cell_info_cdma_1_2(out, cell);
                break;
            case RADIO_CELL_INFO_LTE:
                ril_binder_radio_decode_cell_info_lte_1_2(out, cell);
                break;
            case RADIO_CELL_INFO_WCDMA:
                ril_binder_radio_decode_cell_info_wcdma_1_2(out, cell);
                break;
            case RADIO_CELL_INFO_TD_SCDMA:
                ril_binder_radio_decode_cell_info_tdscdma_1_2(out, cell);
                break;
            }
        }
        ok = TRUE;
    }
    return ok;
}

/**
 * @param cellInfo List of current cell information known to radio
 */
static
gboolean
ril_binder_radio_decode_cell_info_list_1_4(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    gsize count = 0;
    const RadioCellInfo_1_4* cells = gbinder_reader_read_hidl_type_vec
        (in, RadioCellInfo_1_4, &count);

    if (cells) {
        guint i, n = 0;

        /* Count supported types */
        for (i = 0; i < count; i++) {
            switch (cells[i].cellInfoType) {
            case RADIO_CELL_INFO_1_4_GSM:
            case RADIO_CELL_INFO_1_4_CDMA:
            case RADIO_CELL_INFO_1_4_WCDMA:
            case RADIO_CELL_INFO_1_4_LTE:
            case RADIO_CELL_INFO_1_4_TD_SCDMA:
                n++;
                break;
            /* Do not count 5G cells for now */
            case RADIO_CELL_INFO_1_4_NR:
                break;
            }
        }

        grilio_encode_int32(out, n);

        for (i = 0; i < count; i++) {
            const RadioCellInfo_1_4* cell = cells + i;

            switch (cell->cellInfoType) {
            case RADIO_CELL_INFO_1_4_GSM:
                ril_binder_radio_decode_cell_info_header_1_4(out,
                        cell, RADIO_CELL_INFO_GSM);
                ril_binder_radio_decode_cell_info_gsm(out,
                        &cell->info.gsm.cellIdentityGsm.base,
                        &cell->info.gsm.signalStrengthGsm);
                break;
            case RADIO_CELL_INFO_1_4_CDMA:
                ril_binder_radio_decode_cell_info_header_1_4(out,
                        cell, RADIO_CELL_INFO_CDMA);
                ril_binder_radio_decode_cell_info_cdma(out,
                        &cell->info.cdma.cellIdentityCdma.base,
                        &cell->info.cdma.signalStrengthCdma,
                        &cell->info.cdma.signalStrengthEvdo);
                break;
            case RADIO_CELL_INFO_1_4_LTE:
                ril_binder_radio_decode_cell_info_header_1_4(out,
                        cell, RADIO_CELL_INFO_LTE);
                ril_binder_radio_decode_cell_info_lte(out,
                        &cell->info.lte.base.cellIdentityLte.base,
                        &cell->info.lte.base.signalStrengthLte);
                break;
            case RADIO_CELL_INFO_1_4_WCDMA:
                ril_binder_radio_decode_cell_info_header_1_4(out,
                        cell, RADIO_CELL_INFO_WCDMA);
                ril_binder_radio_decode_cell_info_wcdma(out,
                        &cell->info.wcdma.cellIdentityWcdma.base,
                        &cell->info.wcdma.signalStrengthWcdma.base);
                break;
            case RADIO_CELL_INFO_1_4_TD_SCDMA:
                ril_binder_radio_decode_cell_info_header_1_4(out,
                        cell, RADIO_CELL_INFO_TD_SCDMA);
                ril_binder_radio_decode_cell_info_tdscdma(out,
                        &cell->info.tdscdma.cellIdentityTdscdma.base,
                        cell->info.tdscdma.signalStrengthTdscdma.rscp);
                break;
            case RADIO_CELL_INFO_1_4_NR:
                break;
            }
        }
        ok = TRUE;
    }
    return ok;
}

/**
 * @param bool true = registered, false = not registered
 * @param RadioTechnologyFamily (int32).
 */
static
gboolean
ril_binder_radio_decode_ims_registration_state(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean reg;
    gint32 family;

    if (gbinder_reader_read_bool(in, &reg) &&
        gbinder_reader_read_int32(in, &family)) {
        grilio_encode_int32(out, 2); /* Number of ints to follow */
        grilio_encode_int32(out, reg);
        grilio_encode_int32(out, family);
        return TRUE;
    }
    return FALSE;
}

static
gboolean
ril_binder_radio_decode_icc_open_logical_channel(
    GBinderReader* in,
    GByteArray* out)
{
    guint32 channel;

    if (gbinder_reader_read_uint32(in, &channel)) {
        grilio_encode_int32(out, 1); /* Number of ints to follow */
        grilio_encode_int32(out, channel);
        /* Ignore the select response, ofono doesn't need it */
        return TRUE;
    }
    return FALSE;
}

/**
 * @param rc Radio capability as defined by RadioCapability
 */
static
gboolean
ril_binder_radio_decode_radio_capability(
    GBinderReader* in,
    GByteArray* out)
{
    const RadioCapability* rc = gbinder_reader_read_hidl_struct
        (in, RadioCapability);

    if (rc) {
        grilio_encode_int32(out, 1 /* RIL_RADIO_CAPABILITY_VERSION */);
        grilio_encode_int32(out, rc->session);
        grilio_encode_int32(out, rc->phase);
        grilio_encode_int32(out, rc->raf);
        grilio_encode_utf8(out, rc->logicalModemUuid.data.str);
        grilio_encode_int32(out, rc->status);
        return TRUE;
    }
    return FALSE;
}

/*==========================================================================*
 * Calls
 *==========================================================================*/

static const RilBinderRadioCall ril_binder_radio_calls_1_0[] = {
    {
        RIL_REQUEST_GET_SIM_STATUS,
        RADIO_REQ_GET_ICC_CARD_STATUS,
        RADIO_RESP_GET_ICC_CARD_STATUS,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_icc_card_status_1_0,
        "getIccCardStatus"
    },{
        RIL_REQUEST_ENTER_SIM_PIN,
        RADIO_REQ_SUPPLY_ICC_PIN_FOR_APP,
        RADIO_RESP_SUPPLY_ICC_PIN_FOR_APP,
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyIccPinForApp"
    },{
        RIL_REQUEST_ENTER_SIM_PUK,
        RADIO_REQ_SUPPLY_ICC_PUK_FOR_APP,
        RADIO_RESP_SUPPLY_ICC_PUK_FOR_APP,
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyIccPukForApp"
    },{
        RIL_REQUEST_ENTER_SIM_PIN2,
        RADIO_REQ_SUPPLY_ICC_PIN2_FOR_APP,
        RADIO_RESP_SUPPLY_ICC_PIN2_FOR_APP,
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyIccPin2ForApp"
    },{
        RIL_REQUEST_ENTER_SIM_PUK2,
        RADIO_REQ_SUPPLY_ICC_PUK2_FOR_APP,
        RADIO_RESP_SUPPLY_ICC_PUK2_FOR_APP,
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyIccPuk2ForApp"
    },{
        RIL_REQUEST_CHANGE_SIM_PIN,
        RADIO_REQ_CHANGE_ICC_PIN_FOR_APP,
        RADIO_RESP_CHANGE_ICC_PIN_FOR_APP,
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "changeIccPinForApp"
    },{
        RIL_REQUEST_CHANGE_SIM_PIN2,
        RADIO_REQ_CHANGE_ICC_PIN2_FOR_APP,
        RADIO_RESP_CHANGE_ICC_PIN2_FOR_APP,
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "changeIccPin2ForApp"
    },{
        RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION,
        RADIO_REQ_SUPPLY_NETWORK_DEPERSONALIZATION,
        RADIO_RESP_SUPPLY_NETWORK_DEPERSONALIZATION,
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyNetworkDepersonalization"
    },{
        RIL_REQUEST_GET_CURRENT_CALLS,
        RADIO_REQ_GET_CURRENT_CALLS,
        RADIO_RESP_GET_CURRENT_CALLS,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_call_list,
        "getCurrentCalls"
    },{
        RIL_REQUEST_DIAL,
        RADIO_REQ_DIAL,
        RADIO_RESP_DIAL,
        ril_binder_radio_encode_dial,
        NULL,
        "dial"
    },{
        RIL_REQUEST_GET_IMSI,
        RADIO_REQ_GET_IMSI_FOR_APP,
        RADIO_RESP_GET_IMSI_FOR_APP,
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_string,
        "getImsiForApp"
    },{
        RIL_REQUEST_HANGUP,
        RADIO_REQ_HANGUP,
        RADIO_RESP_HANGUP,
        ril_binder_radio_encode_ints,
        NULL,
        "hangup"
    },{
        RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND,
        RADIO_REQ_HANGUP_WAITING_OR_BACKGROUND,
        RADIO_RESP_HANGUP_WAITING_OR_BACKGROUND,
        ril_binder_radio_encode_serial,
        NULL,
        "hangupWaitingOrBackground"
    },{
        RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND,
        RADIO_REQ_HANGUP_FOREGROUND_RESUME_BACKGROUND,
        RADIO_RESP_HANGUP_FOREGROUND_RESUME_BACKGROUND,
        ril_binder_radio_encode_serial,
        NULL,
        "hangupForegroundResumeBackground"
    },{
        RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE,
        RADIO_REQ_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE,
        RADIO_RESP_SWITCH_WAITING_OR_HOLDING_AND_ACTIVE,
        ril_binder_radio_encode_serial,
        NULL,
        "switchWaitingOrHoldingAndActive"
    },{
        RIL_REQUEST_CONFERENCE,
        RADIO_REQ_CONFERENCE,
        RADIO_RESP_CONFERENCE,
        ril_binder_radio_encode_serial,
        NULL,
        "conference"
    },{
        RIL_REQUEST_UDUB,
        RADIO_REQ_REJECT_CALL,
        RADIO_RESP_REJECT_CALL,
        ril_binder_radio_encode_serial,
        NULL,
        "rejectCall"
    },{
        RIL_REQUEST_LAST_CALL_FAIL_CAUSE,
        RADIO_REQ_GET_LAST_CALL_FAIL_CAUSE,
        RADIO_RESP_GET_LAST_CALL_FAIL_CAUSE,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_last_call_fail_cause,
        "getLastCallFailCause"
    },{
        RIL_REQUEST_SIGNAL_STRENGTH,
        RADIO_REQ_GET_SIGNAL_STRENGTH,
        RADIO_RESP_GET_SIGNAL_STRENGTH,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_signal_strength,
        "getSignalStrength"
    },{
        RIL_REQUEST_VOICE_REGISTRATION_STATE,
        RADIO_REQ_GET_VOICE_REGISTRATION_STATE,
        RADIO_RESP_GET_VOICE_REGISTRATION_STATE,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_voice_reg_state,
        "getVoiceRegistrationState"
    },{
        RIL_REQUEST_DATA_REGISTRATION_STATE,
        RADIO_REQ_GET_DATA_REGISTRATION_STATE,
        RADIO_RESP_GET_DATA_REGISTRATION_STATE,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_data_reg_state,
        "getDataRegistrationState"
    },{
        RIL_REQUEST_OPERATOR,
        RADIO_REQ_GET_OPERATOR,
        RADIO_RESP_GET_OPERATOR,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_string_3,
        "getOperator"
    },{
        RIL_REQUEST_RADIO_POWER,
        RADIO_REQ_SET_RADIO_POWER,
        RADIO_RESP_SET_RADIO_POWER,
        ril_binder_radio_encode_bool,
        NULL,
        "setRadioPower"
    },{
        RIL_REQUEST_DTMF,
        RADIO_REQ_SEND_DTMF,
        RADIO_RESP_SEND_DTMF,
        ril_binder_radio_encode_string,
        NULL,
        "sendDtmf"
    },{
        RIL_REQUEST_SEND_SMS,
        RADIO_REQ_SEND_SMS,
        RADIO_RESP_SEND_SMS,
        ril_binder_radio_encode_gsm_sms_message,
        ril_binder_radio_decode_sms_send_result,
        "sendSms"
    },{
        RIL_REQUEST_SEND_SMS_EXPECT_MORE,
        RADIO_REQ_SEND_SMS_EXPECT_MORE,
        RADIO_RESP_SEND_SMS_EXPECT_MORE,
        ril_binder_radio_encode_gsm_sms_message,
        ril_binder_radio_decode_sms_send_result,
        "sendSMSExpectMore"
    },{
        RIL_REQUEST_SETUP_DATA_CALL,
        RADIO_REQ_SETUP_DATA_CALL,
        RADIO_RESP_SETUP_DATA_CALL,
        ril_binder_radio_encode_setup_data_call,
        ril_binder_radio_decode_setup_data_call_result,
        "setupDataCall"
    },{
        RIL_REQUEST_SIM_IO,
        RADIO_REQ_ICC_IO_FOR_APP,
        RADIO_RESP_ICC_IO_FOR_APP,
        ril_binder_radio_encode_icc_io,
        ril_binder_radio_decode_icc_io_result,
        "iccIOForApp"
    },{
        RIL_REQUEST_SEND_USSD,
        RADIO_REQ_SEND_USSD,
        RADIO_RESP_SEND_USSD,
        ril_binder_radio_encode_string,
        NULL,
        "sendUssd"
    },{
        RIL_REQUEST_CANCEL_USSD,
        RADIO_REQ_CANCEL_PENDING_USSD,
        RADIO_RESP_CANCEL_PENDING_USSD,
        ril_binder_radio_encode_serial,
        NULL,
        "cancelPendingUssd"
    },{
        RIL_REQUEST_GET_CLIR,
        RADIO_REQ_GET_CLIR,
        RADIO_RESP_GET_CLIR,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_int_2,
        "getClir"
    },{
        RIL_REQUEST_SET_CLIR,
        RADIO_REQ_SET_CLIR,
        RADIO_RESP_SET_CLIR,
        ril_binder_radio_encode_ints,
        NULL,
        "setClir"
    },{
        RIL_REQUEST_QUERY_CALL_FORWARD_STATUS,
        RADIO_REQ_GET_CALL_FORWARD_STATUS,
        RADIO_RESP_GET_CALL_FORWARD_STATUS,
        ril_binder_radio_encode_call_forward_info,
        ril_binder_radio_decode_call_forward_info_array,
        "getCallForwardStatus"
    },{
        RIL_REQUEST_SET_CALL_FORWARD,
        RADIO_REQ_SET_CALL_FORWARD,
        RADIO_RESP_SET_CALL_FORWARD,
        ril_binder_radio_encode_call_forward_info,
        NULL,
        "setCallForward"
    },{
        RIL_REQUEST_QUERY_CALL_WAITING,
        RADIO_REQ_GET_CALL_WAITING,
        RADIO_RESP_GET_CALL_WAITING,
        ril_binder_radio_encode_ints,
        ril_binder_radio_decode_call_waiting,
        "getCallWaiting"
    },{
        RIL_REQUEST_SET_CALL_WAITING,
        RADIO_REQ_SET_CALL_WAITING,
        RADIO_RESP_SET_CALL_WAITING,
        ril_binder_radio_encode_ints_to_bool_int,
        NULL,
        "setCallWaiting"
    },{
        RIL_REQUEST_SMS_ACKNOWLEDGE,
        RADIO_REQ_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS,
        RADIO_RESP_ACKNOWLEDGE_LAST_INCOMING_GSM_SMS,
        ril_binder_radio_encode_ints_to_bool_int,
        NULL,
        "acknowledgeLastIncomingGsmSms"
    },{
        RIL_REQUEST_ANSWER,
        RADIO_REQ_ACCEPT_CALL,
        RADIO_RESP_ACCEPT_CALL,
        ril_binder_radio_encode_serial,
        NULL,
        "acceptCall"
    },{
        RIL_REQUEST_DEACTIVATE_DATA_CALL,
        RADIO_REQ_DEACTIVATE_DATA_CALL,
        RADIO_RESP_DEACTIVATE_DATA_CALL,
        ril_binder_radio_encode_deactivate_data_call,
        NULL,
        "deactivateDataCall"
    },{
        RIL_REQUEST_QUERY_FACILITY_LOCK,
        RADIO_REQ_GET_FACILITY_LOCK_FOR_APP,
        RADIO_RESP_GET_FACILITY_LOCK_FOR_APP,
        ril_binder_radio_encode_get_facility_lock,
        ril_binder_radio_decode_int32,
        "getFacilityLockForApp"
    },{
        RIL_REQUEST_SET_FACILITY_LOCK,
        RADIO_REQ_SET_FACILITY_LOCK_FOR_APP,
        RADIO_RESP_SET_FACILITY_LOCK_FOR_APP,
        ril_binder_radio_encode_set_facility_lock,
        ril_binder_radio_decode_int_1,
        "setFacilityLockForApp"
    },{
        RIL_REQUEST_CHANGE_BARRING_PASSWORD,
        RADIO_REQ_SET_BARRING_PASSWORD,
        RADIO_RESP_SET_BARRING_PASSWORD,
        ril_binder_radio_encode_strings,
        NULL,
        "setBarringPassword"
    },{
        RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE,
        RADIO_REQ_GET_NETWORK_SELECTION_MODE,
        RADIO_RESP_GET_NETWORK_SELECTION_MODE,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_bool_to_int_array,
        "getNetworkSelectionMode"
    },{
        RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC,
        RADIO_REQ_SET_NETWORK_SELECTION_MODE_AUTOMATIC,
        RADIO_RESP_SET_NETWORK_SELECTION_MODE_AUTOMATIC,
        ril_binder_radio_encode_serial,
        NULL,
        "setNetworkSelectionModeAutomatic"
    },{
        RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL,
        RADIO_REQ_SET_NETWORK_SELECTION_MODE_MANUAL,
        RADIO_RESP_SET_NETWORK_SELECTION_MODE_MANUAL,
        ril_binder_radio_encode_string,
        NULL,
        "setNetworkSelectionModeManual"
    },{
        RIL_REQUEST_QUERY_AVAILABLE_NETWORKS,
        RADIO_REQ_GET_AVAILABLE_NETWORKS,
        RADIO_RESP_GET_AVAILABLE_NETWORKS,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_operator_info_list,
        "getAvailableNetworks"
    },{
        RIL_REQUEST_BASEBAND_VERSION,
        RADIO_REQ_GET_BASEBAND_VERSION,
        RADIO_RESP_GET_BASEBAND_VERSION,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_string,
        "getBasebandVersion"
    },{
        RIL_REQUEST_SEPARATE_CONNECTION,
        RADIO_REQ_SEPARATE_CONNECTION,
        RADIO_RESP_SEPARATE_CONNECTION,
        ril_binder_radio_encode_ints,
        NULL,
        "separateConnection"
    },{
        RIL_REQUEST_SET_MUTE,
        RADIO_REQ_SET_MUTE,
        RADIO_RESP_SET_MUTE,
        ril_binder_radio_encode_bool,
        NULL,
        "setMute"
    },{
        RIL_REQUEST_GET_MUTE,
        RADIO_REQ_GET_MUTE,
        RADIO_RESP_GET_MUTE,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_bool_to_int_array,
        "getMute"
    },{
        RIL_REQUEST_QUERY_CLIP,
        RADIO_REQ_GET_CLIP,
        RADIO_RESP_GET_CLIP,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_int_1,
        "getClip"
    },{
        RIL_REQUEST_DATA_CALL_LIST,
        RADIO_REQ_GET_DATA_CALL_LIST,
        RADIO_RESP_GET_DATA_CALL_LIST,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_data_call_list,
        "getDataCallList"
    },{
        RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION,
        RADIO_REQ_SET_SUPP_SERVICE_NOTIFICATIONS,
        RADIO_RESP_SET_SUPP_SERVICE_NOTIFICATIONS,
        ril_binder_radio_encode_int,
        NULL,
        "setSuppServiceNotifications"
    },{
        RIL_REQUEST_WRITE_SMS_TO_SIM,
        RADIO_REQ_WRITE_SMS_TO_SIM,
        RADIO_RESP_WRITE_SMS_TO_SIM,
        ril_binder_radio_encode_sms_write_args,
        ril_binder_radio_decode_int_1,
        "writeSmsToSim"
    },{
        RIL_REQUEST_DELETE_SMS_ON_SIM,
        RADIO_REQ_DELETE_SMS_ON_SIM,
        RADIO_RESP_DELETE_SMS_ON_SIM,
        ril_binder_radio_encode_ints,
        NULL,
        "deleteSmsOnSim"
    },{
        RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE,
        RADIO_REQ_GET_AVAILABLE_BAND_MODES,
        RADIO_RESP_GET_AVAILABLE_BAND_MODES,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_int_array,
        "getAvailableBandModes"
    },{
        RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND,
        RADIO_REQ_SEND_ENVELOPE,
        RADIO_RESP_SEND_ENVELOPE,
        ril_binder_radio_encode_string,
        ril_binder_radio_decode_string,
        "sendEnvelope"
    },{
        RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE,
        RADIO_REQ_SEND_TERMINAL_RESPONSE_TO_SIM,
        RADIO_RESP_SEND_TERMINAL_RESPONSE_TO_SIM,
        ril_binder_radio_encode_string,
        NULL,
        "sendTerminalResponseToSim"
    },{
        RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM,
        RADIO_REQ_HANDLE_STK_CALL_SETUP_REQUEST_FROM_SIM,
        RADIO_RESP_HANDLE_STK_CALL_SETUP_REQUEST_FROM_SIM,
        ril_binder_radio_encode_bool,
        NULL,
        "handleStkCallSetupRequestFromSim"
    },{
        RIL_REQUEST_EXPLICIT_CALL_TRANSFER,
        RADIO_REQ_EXPLICIT_CALL_TRANSFER,
        RADIO_RESP_EXPLICIT_CALL_TRANSFER,
        ril_binder_radio_encode_serial,
        NULL,
        "explicitCallTransfer"
    },{
        RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
        RADIO_REQ_SET_PREFERRED_NETWORK_TYPE,
        RADIO_RESP_SET_PREFERRED_NETWORK_TYPE,
        ril_binder_radio_encode_ints,
        NULL,
        "setPreferredNetworkType"
    },{
        RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
        RADIO_REQ_GET_PREFERRED_NETWORK_TYPE,
        RADIO_RESP_GET_PREFERRED_NETWORK_TYPE,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_pref_network_type,
        "getPreferredNetworkType"
    },{
        RIL_REQUEST_SCREEN_STATE, /* deprecated on 2017-01-10 */
        RADIO_REQ_SEND_DEVICE_STATE,
        /*
         * No resp_tx here, the one for RIL_REQUEST_SEND_DEVICE_STATE
         * will be used to handle the response. Both SCREEN_STATE and
         * SEND_DEVICE_STATE responses carry no payload and therefore
         * are processed identically. It's still a bit of a hack though :/
         */
        RADIO_RESP_NONE,
        ril_binder_radio_map_screen_state_to_device_state,
        NULL,
        "sendDeviceState"
    },{
        RIL_REQUEST_SET_LOCATION_UPDATES,
        RADIO_REQ_SET_LOCATION_UPDATES,
        RADIO_RESP_SET_LOCATION_UPDATES,
        ril_binder_radio_encode_bool,
        NULL,
        "setLocationUpdates"
    },{
        RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG,
        RADIO_REQ_GET_GSM_BROADCAST_CONFIG,
        RADIO_RESP_GET_GSM_BROADCAST_CONFIG,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_gsm_broadcast_sms_config,
        "getGsmBroadcastConfig"
    },{
        RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG,
        RADIO_REQ_SET_GSM_BROADCAST_CONFIG,
        RADIO_RESP_SET_GSM_BROADCAST_CONFIG,
        ril_binder_radio_encode_gsm_broadcast_sms_config,
        NULL,
        "setGsmBroadcastConfig"
    },{
        RIL_REQUEST_DEVICE_IDENTITY,
        RADIO_REQ_GET_DEVICE_IDENTITY,
        RADIO_RESP_GET_DEVICE_IDENTITY,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_device_identity,
        "getDeviceIdentity"
    },{
        RIL_REQUEST_GET_SMSC_ADDRESS,
        RADIO_REQ_GET_SMSC_ADDRESS,
        RADIO_RESP_GET_SMSC_ADDRESS,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_string,
        "getSmscAddress"
    },{
        RIL_REQUEST_SET_SMSC_ADDRESS,
        RADIO_REQ_SET_SMSC_ADDRESS,
        RADIO_RESP_SET_SMSC_ADDRESS,
        ril_binder_radio_encode_string,
        NULL,
        "setSmscAddress"
    },{
        RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING,
        RADIO_REQ_REPORT_STK_SERVICE_IS_RUNNING,
        RADIO_RESP_REPORT_STK_SERVICE_IS_RUNNING,
        ril_binder_radio_encode_serial,
        NULL,
        "reportStkServiceIsRunning"
    },{
        RIL_REQUEST_GET_CELL_INFO_LIST,
        RADIO_REQ_GET_CELL_INFO_LIST,
        RADIO_RESP_GET_CELL_INFO_LIST,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_cell_info_list,
        "getCellInfoList"
    },{
        RIL_REQUEST_SET_UNSOL_CELL_INFO_LIST_RATE,
        RADIO_REQ_SET_CELL_INFO_LIST_RATE,
        RADIO_RESP_SET_CELL_INFO_LIST_RATE,
        ril_binder_radio_encode_ints,
        NULL,
        "setCellInfoListRate"
    },{
        RIL_REQUEST_SET_INITIAL_ATTACH_APN,
        RADIO_REQ_SET_INITIAL_ATTACH_APN,
        RADIO_RESP_SET_INITIAL_ATTACH_APN,
        ril_binder_radio_encode_initial_attach_apn,
        NULL,
        "setInitialAttachApn"
    },{
        RIL_REQUEST_IMS_REGISTRATION_STATE,
        RADIO_REQ_GET_IMS_REGISTRATION_STATE,
        RADIO_RESP_GET_IMS_REGISTRATION_STATE,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_ims_registration_state,
        "getImsRegistrationState"
    },{
        RIL_REQUEST_SIM_OPEN_CHANNEL,
        RADIO_REQ_ICC_OPEN_LOGICAL_CHANNEL,
        RADIO_RESP_ICC_OPEN_LOGICAL_CHANNEL,
        ril_binder_radio_encode_icc_open_logical_channel,
        ril_binder_radio_decode_icc_open_logical_channel,
        "iccOpenLogicalChannel"
    },{
        RIL_REQUEST_SIM_CLOSE_CHANNEL,
        RADIO_REQ_ICC_CLOSE_LOGICAL_CHANNEL,
        RADIO_RESP_ICC_CLOSE_LOGICAL_CHANNEL,
        ril_binder_radio_encode_ints,
        NULL,
        "iccCloseLogicalChannel"
    },{
        RIL_REQUEST_SIM_TRANSMIT_APDU_CHANNEL,
        RADIO_REQ_ICC_TRANSMIT_APDU_LOGICAL_CHANNEL,
        RADIO_RESP_ICC_TRANSMIT_APDU_LOGICAL_CHANNEL,
        ril_binder_radio_encode_icc_transmit_apdu_logical_channel,
        ril_binder_radio_decode_icc_io_result,
        "iccTransmitApduLogicalChannel"
    },{
        RIL_REQUEST_SET_UICC_SUBSCRIPTION,
        RADIO_REQ_SET_UICC_SUBSCRIPTION,
        RADIO_RESP_SET_UICC_SUBSCRIPTION,
        ril_binder_radio_encode_uicc_sub,
        NULL,
        "setUiccSubscription"
    },{
        RIL_REQUEST_ALLOW_DATA,
        RADIO_REQ_SET_DATA_ALLOWED,
        RADIO_RESP_SET_DATA_ALLOWED,
        ril_binder_radio_encode_bool,
        NULL,
        "setDataAllowed"
    },{
        RIL_REQUEST_SET_DATA_PROFILE,
        RADIO_REQ_SET_DATA_PROFILE,
        RADIO_RESP_SET_DATA_PROFILE,
        ril_binder_radio_encode_data_profiles,
        NULL,
        "setDataProfile"
    },{
        RIL_REQUEST_GET_RADIO_CAPABILITY,
        RADIO_REQ_GET_RADIO_CAPABILITY,
        RADIO_RESP_GET_RADIO_CAPABILITY,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_radio_capability,
        "getRadioCapability"
    },{
        RIL_REQUEST_SET_RADIO_CAPABILITY,
        RADIO_REQ_SET_RADIO_CAPABILITY,
        RADIO_RESP_SET_RADIO_CAPABILITY,
        ril_binder_radio_encode_radio_capability,
        ril_binder_radio_decode_radio_capability,
        "setRadioCapability"
    },{
        RIL_REQUEST_SEND_DEVICE_STATE,
        RADIO_REQ_SEND_DEVICE_STATE,
        RADIO_RESP_SEND_DEVICE_STATE,
        ril_binder_radio_encode_device_state,
        NULL,
        "sendDeviceState"
    },{
        RIL_REQUEST_SET_UNSOLICITED_RESPONSE_FILTER,
        RADIO_REQ_SET_INDICATION_FILTER,
        RADIO_RESP_SET_INDICATION_FILTER,
        ril_binder_radio_encode_ints,
        NULL,
        "setIndicationFilter"
    },{
        RIL_RESPONSE_ACKNOWLEDGEMENT,
        RADIO_REQ_RESPONSE_ACKNOWLEDGEMENT,
        RADIO_RESP_NONE,
        NULL,
        NULL,
        "responseAcknowledgement"
    }
};

static const RilBinderRadioCall ril_binder_radio_calls_1_2[] = {
    {
        0,
        0,
        RADIO_RESP_GET_ICC_CARD_STATUS_1_2,
        NULL,
        ril_binder_radio_decode_icc_card_status_1_2,
        "getIccCardStatus_1_2"
    },{
        RIL_REQUEST_SETUP_DATA_CALL,
        RADIO_REQ_SETUP_DATA_CALL_1_2,
        0,
        ril_binder_radio_encode_setup_data_call_1_2,
        ril_binder_radio_decode_setup_data_call_result,
        "setupDataCall_1_2"
    },{
        RIL_REQUEST_DEACTIVATE_DATA_CALL,
        RADIO_REQ_DEACTIVATE_DATA_CALL_1_2,
        0,
        ril_binder_radio_encode_deactivate_data_call_1_2,
        NULL,
        "deactivateDataCall_1_2"
    },{
        0,
        0,
        RADIO_RESP_GET_VOICE_REGISTRATION_STATE_1_2,
        NULL,
        ril_binder_radio_decode_voice_reg_state,
        "getVoiceRegistrationState_1_2"
    },{
        0,
        0,
        RADIO_RESP_GET_DATA_REGISTRATION_STATE_1_2,
        NULL,
        ril_binder_radio_decode_data_reg_state,
        "getDataRegistrationState_1_2"
    },{
        0,
        0,
        RADIO_RESP_GET_CURRENT_CALLS_1_2,
        NULL,
        ril_binder_radio_decode_call_list_1_2,
        "getCurrentCalls_1_2"
    },{
        0,
        0,
        RADIO_RESP_GET_CELL_INFO_LIST_1_2,
        NULL,
        ril_binder_radio_decode_cell_info_list_1_2,
        "getCellInfoList_1_2"
    },{
        0,
        0,
        RADIO_RESP_GET_SIGNAL_STRENGTH_1_2,
        NULL,
        ril_binder_radio_decode_signal_strength_1_2,
        "getSignalStrength_1_2"
    }
};

static const RilBinderRadioCall ril_binder_radio_calls_1_4[] = {
    {
        0,
        0,
        RADIO_RESP_GET_ICC_CARD_STATUS_RESPONSE_1_4,
        NULL,
        ril_binder_radio_decode_icc_card_status_1_4,
        "getIccCardStatus_1_4"
    },{
        RIL_REQUEST_SETUP_DATA_CALL,
        RADIO_REQ_SETUP_DATA_CALL_1_2, /* Using setupDataCall_1_2 */
        RADIO_RESP_SETUP_DATA_CALL_RESPONSE_1_4,
        ril_binder_radio_encode_setup_data_call_1_2,
        ril_binder_radio_decode_setup_data_call_result_1_4,
        "setupDataCall_1_4"
    },{
        0,
        0,
        RADIO_RESP_GET_DATA_REGISTRATION_STATE_RESPONSE_1_4,
        NULL,
        ril_binder_radio_decode_data_reg_state_1_4,
        "getDataRegistrationState_1_4"
    },{
        0,
        0,
        RADIO_RESP_GET_DATA_CALL_LIST_RESPONSE_1_4,
        NULL,
        ril_binder_radio_decode_call_list_1_2,
        "getDataCallList_1_4"
    },{
        0,
        0,
        RADIO_RESP_GET_CELL_INFO_LIST_RESPONSE_1_4,
        NULL,
        ril_binder_radio_decode_cell_info_list_1_4,
        "getCellInfoList_1_4"
    },{
        0,
        0,
        RADIO_RESP_GET_SIGNAL_STRENGTH_1_4,
        NULL,
        ril_binder_radio_decode_signal_strength_1_4,
        "getSignalStrength_1_4"
    },{
        RADIO_REQ_SET_PREFERRED_NETWORK_TYPE,
        0,
        RADIO_RESP_SET_PREFERRED_NETWORK_TYPE_BITMAP,
        ril_binder_radio_encode_ints,
        NULL,
        "setPreferredNetworkTypeBitmap_1_4"
    },{
        0,
        0,
        RADIO_RESP_GET_PREFERRED_NETWORK_TYPE_BITMAP,
        NULL,
        ril_binder_radio_decode_pref_network_type_bitmap,
        "getPreferredNetworkTypeBitmap_1_4"
    }
};

/*==========================================================================*
 * Events
 *==========================================================================*/

static const RilBinderRadioEvent ril_binder_radio_events_1_0[] = {
    {
        RIL_UNSOL_RESPONSE_RADIO_STATE_CHANGED,
        RADIO_IND_RADIO_STATE_CHANGED,
        ril_binder_radio_decode_int32,
        "radioStateChanged"
    },{
        RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
        RADIO_IND_CALL_STATE_CHANGED,
        NULL,
        "callStateChanged"
    },{
        RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
        RADIO_IND_NETWORK_STATE_CHANGED,
        NULL,
        "networkStateChanged"
    },{
        RIL_UNSOL_RESPONSE_NEW_SMS,
        RADIO_IND_NEW_SMS,
        ril_binder_radio_decode_byte_array_to_hex,
        "newSms"
    },{
        RIL_UNSOL_RESPONSE_NEW_SMS_STATUS_REPORT,
        RADIO_IND_NEW_SMS_STATUS_REPORT,
        ril_binder_radio_decode_byte_array_to_hex,
        "newSmsStatusReport"
    },{
        RIL_UNSOL_ON_USSD,
        RADIO_IND_ON_USSD,
        ril_binder_radio_decode_ussd,
        "onUssd"
    },{
        RIL_UNSOL_NITZ_TIME_RECEIVED,
        RADIO_IND_NITZ_TIME_RECEIVED,
        ril_binder_radio_decode_string,
        "nitzTimeReceived"
    },{
        RIL_UNSOL_SIGNAL_STRENGTH,
        RADIO_IND_CURRENT_SIGNAL_STRENGTH,
        ril_binder_radio_decode_signal_strength,
        "currentSignalStrength"
    },{
        RIL_UNSOL_DATA_CALL_LIST_CHANGED,
        RADIO_IND_DATA_CALL_LIST_CHANGED,
        ril_binder_radio_decode_data_call_list,
        "dataCallListChanged"
    },{
        RIL_UNSOL_SUPP_SVC_NOTIFICATION,
        RADIO_IND_SUPP_SVC_NOTIFY,
        ril_binder_radio_decode_supp_svc_notification,
        "suppSvcNotify"
    },{
        RIL_UNSOL_STK_SESSION_END,
        RADIO_IND_STK_SESSION_END,
        NULL,
        "stkSessionEnd"
    },{
        RIL_UNSOL_STK_PROACTIVE_COMMAND,
        RADIO_IND_STK_PROACTIVE_COMMAND,
        ril_binder_radio_decode_string,
        "stkProactiveCommand"
    },{
        RIL_UNSOL_STK_EVENT_NOTIFY,
        RADIO_IND_STK_EVENT_NOTIFY,
        ril_binder_radio_decode_string,
        "stkEventNotify"
    },{
        RIL_UNSOL_SIM_REFRESH,
        RADIO_IND_SIM_REFRESH,
        ril_binder_radio_decode_sim_refresh,
        "simRefresh"
    },{
        RIL_UNSOL_CALL_RING,
        RADIO_IND_CALL_RING,
        NULL, /* No parameters for GSM calls */
        "callRing"
    },{
        RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
        RADIO_IND_SIM_STATUS_CHANGED,
        NULL,
        "simStatusChanged"
    },{
        RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS,
        RADIO_IND_NEW_BROADCAST_SMS,
        ril_binder_radio_decode_byte_array,
        "newBroadcastSms"
    },{
        RIL_UNSOL_RINGBACK_TONE,
        RADIO_IND_INDICATE_RINGBACK_TONE,
        ril_binder_radio_decode_bool_to_int_array,
        "indicateRingbackTone"
    },{
        RIL_UNSOL_VOICE_RADIO_TECH_CHANGED,
        RADIO_IND_VOICE_RADIO_TECH_CHANGED,
        ril_binder_radio_decode_int32,
        "voiceRadioTechChanged"
    },{
        RIL_UNSOL_CELL_INFO_LIST,
        RADIO_IND_CELL_INFO_LIST,
        ril_binder_radio_decode_cell_info_list,
        "cellInfoList"
    },{
        RIL_UNSOL_RESPONSE_IMS_NETWORK_STATE_CHANGED,
        RADIO_IND_IMS_NETWORK_STATE_CHANGED,
        NULL,
        "imsNetworkStateChanged"
    },{
        RIL_UNSOL_UICC_SUBSCRIPTION_STATUS_CHANGED,
        RADIO_IND_SUBSCRIPTION_STATUS_CHANGED,
        ril_binder_radio_decode_bool_to_int_array,
        "subscriptionStatusChanged"
    }
};

static const RilBinderRadioEvent ril_binder_radio_events_1_2[] = {
    {
        RIL_UNSOL_CELL_INFO_LIST,
        RADIO_IND_CELL_INFO_LIST_1_2,
        ril_binder_radio_decode_cell_info_list_1_2,
        "cellInfoList_1_2"
    },{
        RIL_UNSOL_SIGNAL_STRENGTH,
        RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_2,
        ril_binder_radio_decode_signal_strength_1_2,
        "currentSignalStrength_1_2"
    }
};

static const RilBinderRadioEvent ril_binder_radio_events_1_4[] = {
    {
        RIL_UNSOL_CELL_INFO_LIST,
        RADIO_IND_CELL_INFO_LIST_1_4,
        ril_binder_radio_decode_cell_info_list_1_4,
        "cellInfoList_1_4"
    },{
        RIL_UNSOL_DATA_CALL_LIST_CHANGED,
        RADIO_IND_DATA_CALL_LIST_CHANGED_1_4,
        ril_binder_radio_decode_data_call_list_1_4,
        "dataCallListChanged_1_4"
    },{
        RIL_UNSOL_SIGNAL_STRENGTH,
        RADIO_IND_CURRENT_SIGNAL_STRENGTH_1_4,
        ril_binder_radio_decode_signal_strength_1_4,
        "currentSignalStrength_1_4"
    }
};

/*==========================================================================*
 * Generic failure
 *==========================================================================*/

static
void
ril_binder_radio_generic_failure_run(
    gpointer data)
{
    RilBinderRadioFailureData* failure = data;
    GRilIoTransport* transport = failure->transport;

    grilio_transport_ref(transport);
    grilio_transport_signal_response(transport, GRILIO_RESPONSE_SOLICITED,
        failure->serial, RIL_E_GENERIC_FAILURE, NULL, 0);
    grilio_transport_unref(transport);
}

static
void
ril_binder_radio_generic_failure_free(
    gpointer data)
{
    g_slice_free1(sizeof(RilBinderRadioFailureData), data);
}

static
GRILIO_SEND_STATUS
ril_binder_radio_generic_failure(
    RilBinderRadio* self,
    GRilIoRequest* req)
{
    if (self->radio) {
        RilBinderRadioPriv* priv = self->priv;
        RilBinderRadioFailureData* failure =
            g_slice_new(RilBinderRadioFailureData);

        failure->transport = &self->parent;
        failure->serial = grilio_request_serial(req);
        gutil_idle_queue_add_full(priv->idle,
            ril_binder_radio_generic_failure_run, failure,
            ril_binder_radio_generic_failure_free);
        return GRILIO_SEND_OK;
    }
    return GRILIO_SEND_ERROR;
}

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static
void
ril_binder_radio_drop_radio(
    RilBinderRadio* self)
{
    RilBinderRadioPriv* priv = self->priv;

    if (self->radio) {
        radio_instance_remove_all_handlers(self->radio, priv->radio_event_id);
        radio_instance_unref(self->radio);
        self->radio = NULL;
    }
    if (priv->oemhook) {
        ril_binder_oemhook_remove_handler(priv->oemhook,
            priv->oemhook_raw_response_id);
        priv->oemhook_raw_response_id = 0;
        ril_binder_oemhook_free(priv->oemhook);
        priv->oemhook = NULL;
    }
}

static
gboolean
ril_binder_radio_handle_known_response(
    RilBinderRadio* self,
    const RilBinderRadioCall* call,
    const RadioResponseInfo* info,
    GBinderReader* reader)
{
    if (ril_binder_radio_decode_response(self, info, call->decode, reader)) {
        return TRUE;
    } else {
        GWARN("Failed to decode %s response", call->name);
        return FALSE;
    }
}

static
gboolean
ril_binder_radio_handle_known_indication(
    RilBinderRadio* self,
    const RilBinderRadioEvent* event,
    RADIO_IND_TYPE ind_type,
    GBinderReader* reader)
{
    if (ril_binder_radio_decode_indication(self, ind_type, event->code,
        event->decode, reader)) {
        return TRUE;
    } else {
        GWARN("Failed to decode %s indication", event->name);
        return FALSE;
    }
}

static
void
ril_binder_radio_connected(
    RilBinderRadio* self)
{
    GRilIoTransport* transport = &self->parent;

    DBG_(self, "connected");
    GASSERT(!transport->connected);
    transport->ril_version = self->radio->version;
    transport->connected = TRUE;
    grilio_transport_signal_connected(transport);
}

static
gboolean
ril_binder_radio_indication_handler(
    RadioInstance* radio,
    RADIO_IND code,
    RADIO_IND_TYPE type,
    const GBinderReader* args,
    gpointer user_data)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(user_data);
    RilBinderRadioClass* klass = RIL_BINDER_RADIO_GET_CLASS(self);

    return klass->handle_indication(self, code, type, args);
}

static
gboolean
ril_binder_radio_response_handler(
    RadioInstance* radio,
    RADIO_RESP code,
    const RadioResponseInfo* info,
    const GBinderReader* args,
    gpointer user_data)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(user_data);
    RilBinderRadioClass* klass = RIL_BINDER_RADIO_GET_CLASS(self);

    return klass->handle_response(self, code, info, args);
}

static
void
ril_binder_radio_ack_handler(
    RadioInstance* radio,
    guint32 serial,
    gpointer user_data)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(user_data);

    DBG_(self, "IRadioResponse acknowledgeRequest");
    grilio_transport_signal_response(&self->parent,
        GRILIO_RESPONSE_SOLICITED_ACK, serial, RIL_E_SUCCESS, NULL, 0);
}

static
void
ril_binder_radio_radio_died(
    RadioInstance* radio,
    void* user_data)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(user_data);
    GRilIoTransport* transport = &self->parent;

    GERR("%sradio died", transport->log_prefix);
    ril_binder_radio_drop_radio(self);
    grilio_transport_signal_disconnected(transport);
}

static
void
ril_binder_radio_enabled_changed(
    GRilIoChannel* channel,
    void* user_data)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(user_data);

    DBG_(self, "%sabled", channel->enabled ? "en" : "dis");
    radio_instance_set_enabled(self->radio, channel->enabled);
}

static
GRILIO_RESPONSE_TYPE
ril_binder_radio_convert_resp_type(
    RADIO_RESP_TYPE type)
{
    switch (type) {
    case RADIO_RESP_SOLICITED:
        return GRILIO_RESPONSE_SOLICITED;
    case RADIO_RESP_SOLICITED_ACK:
        return GRILIO_RESPONSE_SOLICITED_ACK;
    case RADIO_RESP_SOLICITED_ACK_EXP:
        return GRILIO_RESPONSE_SOLICITED_ACK_EXP;
    }
    GDEBUG("Unexpected response type %u", type);
    return GRILIO_RESPONSE_NONE;
}

static
void
ril_binder_radio_handle_oemhook_raw_response(
    RilBinderOemHook* hook,
    const RadioResponseInfo* info,
    const GUtilData* data,
    gpointer user_data)
{
    GRILIO_RESPONSE_TYPE type = ril_binder_radio_convert_resp_type(info->type);

    if (type != GRILIO_RESPONSE_NONE) {
        grilio_transport_signal_response(GRILIO_TRANSPORT(user_data), type,
            info->serial, info->error, data->bytes, data->size);
    }
}

/*==========================================================================*
 * Methods
 *==========================================================================*/

static
gboolean
ril_binder_radio_handle_response(
    RilBinderRadio* self,
    RADIO_RESP code,
    const RadioResponseInfo* info,
    const GBinderReader* args)
{
    RilBinderRadioPriv* priv = self->priv;
    int i = MIN(self->radio->version, RADIO_INTERFACE_COUNT - 1);
    const RilBinderRadioCall* call = NULL;

    while (i >= 0 && !call) {
        GHashTable* map = priv->resp_map[i--];

        if (map) {
            call = g_hash_table_lookup(map, GINT_TO_POINTER(code));
        }
    }

    if (call) {
        GBinderReader copy;

        /* This is a known response */
        gbinder_reader_copy(&copy, args);
        DBG_(self, "IRadioResponse %u %s", code, call->name);
        return ril_binder_radio_handle_known_response(self, call, info, &copy);
    } else {
        DBG_(self, "IRadioResponse %u", code);
        GWARN("Unexpected response transaction %u", code);
        return FALSE;
    }
}

static
gboolean
ril_binder_radio_handle_indication(
    RilBinderRadio* self,
    RADIO_IND code,
    RADIO_IND_TYPE type,
    const GBinderReader* args)
{
    /* CONNECTED indication is slightly special */
    if (code == RADIO_IND_RIL_CONNECTED) {
        DBG_(self, "IRadioIndication %u rilConnected", code);
        ril_binder_radio_connected(self);
        return TRUE;
    } else {
        RilBinderRadioPriv* priv = self->priv;
        int i = MIN(self->radio->version, RADIO_INTERFACE_COUNT - 1);
        const RilBinderRadioEvent* event = NULL;

        while (i >= 0 && !event) {
            GHashTable* map = priv->unsol_map[i--];

            if (map) {
                event = g_hash_table_lookup(map, GINT_TO_POINTER(code));
            }
        }

        if (event) {
            GBinderReader reader;
            GRilIoTransport* transport = &self->parent;

            /* Not all HALs bother to send rilConnected */
            if (!transport->connected) {
                DBG_(self, "Simulating rilConnected");
                ril_binder_radio_connected(self);
            }

            gbinder_reader_copy(&reader, args);
            DBG_(self, "IRadioIndication %u %s", code, event->name);
            return ril_binder_radio_handle_known_indication(self, event,
                type, &reader);
        } else {
            DBG_(self, "IRadioIndication %u", code);
            return FALSE;
        }
    }
}

static
GRILIO_SEND_STATUS
ril_binder_radio_send(
    GRilIoTransport* transport,
    GRilIoRequest* req,
    guint code)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(transport);
    RilBinderRadioPriv* priv = self->priv;
    int i = MIN(self->radio->version, RADIO_INTERFACE_COUNT - 1);
    const RilBinderRadioCall* call = NULL;

    while (i >= 0 && !call) {
        GHashTable* map = priv->req_map[i--];

        if (map) {
            call = g_hash_table_lookup(map, GINT_TO_POINTER(code));
        }
    }

    if (call) {
        /* This is a known request */
        GBinderLocalRequest* txreq = radio_instance_new_request(self->radio,
            call->req_tx);

        if (!call->encode || call->encode(req, txreq)) {
            if (radio_instance_send_request_sync(self->radio, call->req_tx,
                txreq)) {
                /* Transaction succeeded */
                gbinder_local_request_unref(txreq);
                return GRILIO_SEND_OK;
            }
        } else {
            GWARN("Failed to encode %s() arguments", call->name);
        }
        gbinder_local_request_unref(txreq);
    } else if (code == RIL_REQUEST_OEM_HOOK_RAW) {
        /*
         * This needs to be special-cased, because OEM_HOOK functionality
         * was moved to separate IOemHook interface.
         */
        if (priv->oemhook) {
            if (ril_binder_oemhook_send_request_raw(priv->oemhook, req)) {
                return GRILIO_SEND_OK;
            }
        } else {
            GWARN("No OEM hook to handle OEM_HOOK_RAW request");
        }
    } else {
        GWARN("Unknown RIL command %u", code);
    }

    /* All kinds of failures are mapped to RIL_E_GENERIC_FAILURE */
    return ril_binder_radio_generic_failure(self, req);
}

static
void
ril_binder_radio_shutdown(
    GRilIoTransport* transport,
    gboolean flush)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(transport);
    const gboolean was_connected = (self->radio != NULL);

    ril_binder_radio_drop_radio(self);
    if (was_connected) {
        grilio_transport_signal_disconnected(transport);
    }
}

static
void
ril_binder_radio_set_channel(
    GRilIoTransport* transport,
    GRilIoChannel* channel)
{
    GRilIoTransportClass* klass = GRILIO_TRANSPORT_CLASS(PARENT_CLASS);
    RilBinderRadio* self = RIL_BINDER_RADIO(transport);

    if (channel) {
        /*
         * N.B. There's no need to remove this handler (and therefore keep
         * its id) because set_channel(NULL) will be invoked from channel's
         * finalize method when all signal connections have already been
         * killed and this id would no longer be valid anyway.
         */
        grilio_channel_add_enabled_changed_handler(channel,
            ril_binder_radio_enabled_changed, self);
        klass->set_channel(transport, channel);
        radio_instance_set_enabled(self->radio, channel->enabled);
    } else {
        radio_instance_set_enabled(self->radio, FALSE);
        klass->set_channel(transport, NULL);
    }
}

/*==========================================================================*
 * API
 *==========================================================================*/

void
ril_binder_radio_decode_data_call(
    GByteArray* out,
    const RadioDataCall* call)
{
    grilio_encode_int32(out, call->status);
    grilio_encode_int32(out, call->suggestedRetryTime);
    grilio_encode_int32(out, call->cid);
    grilio_encode_int32(out, call->active);
    grilio_encode_utf8(out, call->type.data.str);
    grilio_encode_utf8(out, call->ifname.data.str);
    grilio_encode_utf8(out, call->addresses.data.str);
    grilio_encode_utf8(out, call->dnses.data.str);
    grilio_encode_utf8(out, call->gateways.data.str);
    grilio_encode_utf8(out, call->pcscf.data.str);
    grilio_encode_int32(out, call->mtu);
}

gboolean
ril_binder_radio_decode_response(
    RilBinderRadio* self,
    const RadioResponseInfo* info,
    RilBinderRadioDecodeFunc decode,
    GBinderReader* reader)
{
    RilBinderRadioPriv* priv = self->priv;
    GByteArray* buf = priv->buf;
    gboolean signaled = FALSE;

    /* Protection against hypothetical recursion */
    if (buf) {
        priv->buf = NULL;
    } else {
        buf = g_byte_array_new();
    }

    /* Decode the response */
    g_byte_array_set_size(buf, 0);
    if (!decode || decode(reader, buf)) {
        GRilIoTransport* transport = &self->parent;
        GRILIO_RESPONSE_TYPE type = ril_binder_radio_convert_resp_type
            (info->type);

        if (type != GRILIO_RESPONSE_NONE) {
            grilio_transport_signal_response(transport, type, info->serial,
                info->error, buf->data, buf->len);
            signaled = TRUE;
        }
    }

    g_byte_array_set_size(buf, 0);
    if (priv->buf) {
        g_byte_array_unref(priv->buf);
    }
    priv->buf = buf;
    return signaled;
}

gboolean
ril_binder_radio_decode_indication(
    RilBinderRadio* self,
    RADIO_IND_TYPE ind_type,
    guint ril_code,
    RilBinderRadioDecodeFunc decode,
    GBinderReader* reader)
{
    RilBinderRadioPriv* priv = self->priv;
    GByteArray* buf = priv->buf;
    gboolean signaled = FALSE;

    /* Protection against hypothetical recursion */
    if (buf) {
        priv->buf = NULL;
    } else {
        buf = g_byte_array_new();
    }

    /* Decode the event */
    g_byte_array_set_size(buf, 0);
    if (!decode || decode(reader, buf)) {
        GRILIO_INDICATION_TYPE type = (ind_type == RADIO_IND_ACK_EXP) ?
            GRILIO_INDICATION_UNSOLICITED_ACK_EXP :
            GRILIO_INDICATION_UNSOLICITED;

        grilio_transport_signal_indication(&self->parent, type, ril_code,
            buf->data, buf->len);
        signaled = TRUE;
    }

    g_byte_array_set_size(buf, 0);
    if (priv->buf) {
        g_byte_array_unref(priv->buf);
    }
    priv->buf = buf;
    return signaled;
}

GRilIoTransport*
ril_binder_radio_new(
    GHashTable* args)
{
    RilBinderRadio* self = g_object_new(RIL_TYPE_BINDER_RADIO, NULL);
    GRilIoTransport* transport = &self->parent;

    if (ril_binder_radio_init_base(self, args)) {
        return transport;
    } else {
        grilio_transport_unref(transport);
        return NULL;
    }
}

const char*
ril_binder_radio_arg_modem(
    GHashTable* args)
{
    return ril_binder_radio_arg_value(args, RIL_BINDER_KEY_MODEM,
        RIL_BINDER_DEFAULT_MODEM);
}

const char*
ril_binder_radio_arg_dev(
    GHashTable* args)
{
    return ril_binder_radio_arg_value(args, RIL_BINDER_KEY_DEV,
        RIL_BINDER_DEFAULT_DEV);
}

const char*
ril_binder_radio_arg_name(
    GHashTable* args)
{
    return ril_binder_radio_arg_value(args, RIL_BINDER_KEY_NAME,
        RIL_BINDER_DEFAULT_NAME);
}

static
RADIO_INTERFACE
ril_binder_radio_arg_interface(
    GHashTable* args)
{
    const char *name = ril_binder_radio_arg_value(args,
        RIL_BINDER_KEY_INTERFACE, NULL);

    if (name) {
        RADIO_INTERFACE i;

        for (i = RADIO_INTERFACE_1_0; i < RADIO_INTERFACE_COUNT; i++ ) {
            if (!g_strcmp0(name, ril_binder_radio_interface_name(i))) {
                return i;
            }
        }
    }
    return DEFAULT_INTERFACE;
}

gboolean
ril_binder_radio_init_base(
    RilBinderRadio* self,
    GHashTable* args)
{
    const char* dev = ril_binder_radio_arg_dev(args);
    const char* name = ril_binder_radio_arg_name(args);
    const RADIO_INTERFACE interface = ril_binder_radio_arg_interface(args);

    GDEBUG("%s %s %s %s %s", self->parent.log_prefix,
        ril_binder_radio_arg_modem(args), dev, name,
        ril_binder_radio_interface_name(interface));
    self->radio = radio_instance_new_with_version(dev, name, interface);
    if (self->radio) {
        RilBinderRadioPriv* priv = self->priv;
        GBinderServiceManager* sm = gbinder_servicemanager_new(dev);
        RADIO_INTERFACE v;

        /* android.hardware.radio@1.0 */
        v = RADIO_INTERFACE_1_0;
        priv->req_map[v] = g_hash_table_new(g_direct_hash, g_direct_equal);
        priv->resp_map[v] = g_hash_table_new(g_direct_hash, g_direct_equal);
        priv->unsol_map[v] = g_hash_table_new(g_direct_hash, g_direct_equal);
        ril_binder_radio_init_call_maps(priv->req_map[v], priv->resp_map[v],
            ARRAY_AND_COUNT(ril_binder_radio_calls_1_0));
        ril_binder_radio_init_unsol_map(priv->unsol_map[v],
            ARRAY_AND_COUNT(ril_binder_radio_events_1_0));

        if (self->radio->version >= RADIO_INTERFACE_1_2) {
            /* android.hardware.radio@1.2 */
            v = RADIO_INTERFACE_1_2;
            priv->req_map[v] = g_hash_table_new(g_direct_hash, g_direct_equal);
            priv->resp_map[v] = g_hash_table_new(g_direct_hash, g_direct_equal);
            priv->unsol_map[v] = g_hash_table_new(g_direct_hash,g_direct_equal);
            ril_binder_radio_init_call_maps(priv->req_map[v], priv->resp_map[v],
                ARRAY_AND_COUNT(ril_binder_radio_calls_1_2));
            ril_binder_radio_init_unsol_map(priv->unsol_map[v],
                ARRAY_AND_COUNT(ril_binder_radio_events_1_2));
        }

        if (self->radio->version >= RADIO_INTERFACE_1_4) {
            /* android.hardware.radio@1.4 */
            v = RADIO_INTERFACE_1_4;
            priv->req_map[v] = g_hash_table_new(g_direct_hash, g_direct_equal);
            priv->resp_map[v] = g_hash_table_new(g_direct_hash, g_direct_equal);
            priv->unsol_map[v] = g_hash_table_new(g_direct_hash,g_direct_equal);
            ril_binder_radio_init_call_maps(priv->req_map[v], priv->resp_map[v],
                ARRAY_AND_COUNT(ril_binder_radio_calls_1_4));
            ril_binder_radio_init_unsol_map(priv->unsol_map[v],
                ARRAY_AND_COUNT(ril_binder_radio_events_1_4));
        }

        priv->oemhook = ril_binder_oemhook_new(sm, self->radio);
        if (priv->oemhook) {
            priv->oemhook_raw_response_id =
                ril_binder_oemhook_add_raw_response_handler(priv->oemhook,
                    ril_binder_radio_handle_oemhook_raw_response, self);
        }

        priv->radio_event_id[RADIO_EVENT_INDICATION] =
            radio_instance_add_indication_handler(self->radio, RADIO_IND_ANY,
                ril_binder_radio_indication_handler, self);
        priv->radio_event_id[RADIO_EVENT_RESPONSE] =
            radio_instance_add_response_handler(self->radio, RADIO_RESP_ANY,
                ril_binder_radio_response_handler, self);
        priv->radio_event_id[RADIO_EVENT_ACK] =
            radio_instance_add_ack_handler(self->radio,
                ril_binder_radio_ack_handler, self);
        priv->radio_event_id[RADIO_EVENT_DEATH] =
            radio_instance_add_death_handler(self->radio,
                ril_binder_radio_radio_died, self);
        gbinder_servicemanager_unref(sm);
        return TRUE;
    }
    return FALSE;
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
ril_binder_radio_init(
    RilBinderRadio* self)
{
    RilBinderRadioPriv* priv = G_TYPE_INSTANCE_GET_PRIVATE
        (self, RIL_TYPE_BINDER_RADIO, RilBinderRadioPriv);

    self->priv = priv;
    priv->idle = gutil_idle_queue_new();
    priv->buf = g_byte_array_new();
}

static
void
ril_binder_radio_finalize(
    GObject* object)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(object);
    RilBinderRadioPriv* priv = self->priv;
    int i;

    ril_binder_radio_drop_radio(self);
    gutil_idle_queue_cancel_all(priv->idle);
    gutil_idle_queue_unref(priv->idle);
    for (i = 0; i < RADIO_INTERFACE_COUNT; i++) {
        if (priv->req_map[i]) {
            g_hash_table_destroy(priv->req_map[i]);
        }
        if (priv->resp_map[i]) {
            g_hash_table_destroy(priv->resp_map[i]);
        }
        if (priv->unsol_map[i]) {
            g_hash_table_destroy(priv->unsol_map[i]);
        }
    }
    g_byte_array_unref(priv->buf);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
ril_binder_radio_class_init(
    RilBinderRadioClass* klass)
{
    GRilIoTransportClass* transport = GRILIO_TRANSPORT_CLASS(klass);

    transport->ril_version_offset = 100;
    transport->send = ril_binder_radio_send;
    transport->shutdown = ril_binder_radio_shutdown;
    transport->set_channel = ril_binder_radio_set_channel;
    klass->handle_response = ril_binder_radio_handle_response;
    klass->handle_indication = ril_binder_radio_handle_indication;
    g_type_class_add_private(klass, sizeof(RilBinderRadioPriv));
    G_OBJECT_CLASS(klass)->finalize = ril_binder_radio_finalize;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
