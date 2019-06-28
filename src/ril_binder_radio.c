/*
 * Copyright (C) 2018-2019 Jolla Ltd.
 * Copyright (C) 2018-2019 Slava Monich <slava.monich@jolla.com>
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

#include "ril_binder_radio.h"
#include "ril_binder_radio_impl.h"

#include <ofono/ril-constants.h>
#include <ofono/log.h>

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
#define GLOG_MODULE_NAME ril_binder_radio_log
#include <gutil_log.h>
GLOG_MODULE_DEFINE("grilio-binder");

#define RIL_BINDER_KEY_MODEM      "modem"
#define RIL_BINDER_KEY_DEV        "dev"
#define RIL_BINDER_KEY_NAME       "name"

#define RIL_BINDER_DEFAULT_MODEM  "/ril_0"
#define RIL_BINDER_DEFAULT_DEV    "/dev/hwbinder"
#define RIL_BINDER_DEFAULT_NAME   "slot1"

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
    char* modem;
    GUtilIdleQueue* idle;
    GHashTable* req_map;      /* code -> RilBinderRadioCall */
    GHashTable* resp_map;     /* resp_tx -> RilBinderRadioCall */
    GHashTable* unsol_map;    /* unsol_tx -> RilBinderRadioEvent */
    GByteArray* buf;
    gulong radio_event_id[RADIO_EVENT_COUNT];
};

G_DEFINE_TYPE(RilBinderRadio, ril_binder_radio, GRILIO_TYPE_TRANSPORT)

#define PARENT_CLASS ril_binder_radio_parent_class

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
            (profile_id == RADIO_DATA_PROFILE_DEFAULT) ?
                RADIO_APN_TYPE_DEFAULT : RADIO_APN_TYPE_MMS;

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
            gint32 profile_id, auth_type, enabled;
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
                grilio_parser_get_int32(&parser, &dp->type) &&
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
                dp->roamingProtocol = dp->protocol;
                dp->profileId = profile_id;
                dp->authType = auth_type;
                dp->enabled = enabled;
                dp->supportedApnTypesBitmap =
                    (profile_id == RADIO_DATA_PROFILE_DEFAULT) ?
                        (RADIO_APN_TYPE_DEFAULT | RADIO_APN_TYPE_SUPL |
                         RADIO_APN_TYPE_IA) : RADIO_APN_TYPE_MMS;
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

/**
 * @param cardStatus ICC card status as defined by CardStatus in types.hal
 */
static
gboolean
ril_binder_radio_decode_icc_card_status(
    GBinderReader* in,
    GByteArray* out)
{
    gboolean ok = FALSE;
    const RadioCardStatus* sim = gbinder_reader_read_hidl_struct
        (in, RadioCardStatus);

    if (sim) {
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
ril_binder_radio_decode_icc_result(
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
            const RadioCall* call = calls + i;

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
        /* GW_SignalStrength */
        grilio_encode_int32(out, strength->gw.signalStrength);
        grilio_encode_int32(out, strength->gw.bitErrorRate);

        /* CDMA_SignalStrength */
        grilio_encode_int32(out, strength->cdma.dbm);
        grilio_encode_int32(out, strength->cdma.ecio);

        /* EVDO_SignalStrength */
        grilio_encode_int32(out, strength->evdo.dbm);
        grilio_encode_int32(out, strength->evdo.ecio);
        grilio_encode_int32(out, strength->evdo.signalNoiseRatio);

        /* LTE_SignalStrength_v8 */
        grilio_encode_int32(out, strength->lte.signalStrength);
        grilio_encode_int32(out, strength->lte.rsrp);
        grilio_encode_int32(out, strength->lte.rsrq);
        grilio_encode_int32(out, strength->lte.rssnr);
        grilio_encode_int32(out, strength->lte.cqi);
        grilio_encode_int32(out, strength->lte.timingAdvance);

        /* TD_SCDMA_SignalStrength */
        grilio_encode_int32(out, strength->tdScdma.rscp);
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
    const RadioCellInfo* cell)
{
    const RadioCellInfoGsm* info =  cell->gsm.data.ptr;
    guint i, count = cell->gsm.count;

    for (i = 0; i < count; i++) {
        const RadioCellIdentityGsm* id = &info[i].cellIdentityGsm;
        const RadioSignalStrengthGsm* ss = &info[i].signalStrengthGsm;
        int mcc, mnc;

        ril_binder_radio_decode_cell_info_header(out, cell);
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
}

static
void
ril_binder_radio_decode_cell_info_cdma(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    const RadioCellInfoCdma* info = cell->cdma.data.ptr;
    guint i, count = cell->cdma.count;

    for (i = 0; i < count; i++) {
        const RadioCellIdentityCdma* id = &info[i].cellIdentityCdma;
        const RadioSignalStrengthCdma* ss = &info[i].signalStrengthCdma;
        const RadioSignalStrengthEvdo* evdo = &info[i].signalStrengthEvdo;

        ril_binder_radio_decode_cell_info_header(out, cell);
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
}

static
void
ril_binder_radio_decode_cell_info_lte(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    const RadioCellInfoLte* info = cell->lte.data.ptr;
    guint i, count = cell->lte.count;

    for (i = 0; i < count; i++) {
        const RadioCellIdentityLte* id = &info[i].cellIdentityLte;
        const RadioSignalStrengthLte* ss = &info[i].signalStrengthLte;
        int mcc, mnc;

        ril_binder_radio_decode_cell_info_header(out, cell);
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
}

static
void
ril_binder_radio_decode_cell_info_wcdma(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    const RadioCellInfoWcdma* info = cell->wcdma.data.ptr;
    guint i, count = cell->wcdma.count;

    for (i = 0; i < count; i++) {
        const RadioCellIdentityWcdma* id = &info[i].cellIdentityWcdma;
        const RadioSignalStrengthWcdma* ss = &info[i].signalStrengthWcdma;
        int mcc, mnc;

        ril_binder_radio_decode_cell_info_header(out, cell);
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
}

static
void
ril_binder_radio_decode_cell_info_tdscdma(
    GByteArray* out,
    const RadioCellInfo* cell)
{
    const RadioCellInfoTdscdma* info = cell->tdscdma.data.ptr;
    guint i, count = cell->tdscdma.count;

    for (i = 0; i < count; i++) {
        const RadioCellIdentityTdscdma* id = &info[i].cellIdentityTdscdma;
        const RadioSignalStrengthTdScdma* ss = &info[i].signalStrengthTdscdma;
        int mcc, mnc;

        ril_binder_radio_decode_cell_info_header(out, cell);
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
        grilio_encode_int32(out, id->cpid);
        grilio_encode_int32(out, ss->rscp);
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
                ril_binder_radio_decode_cell_info_gsm(out, cell);
                break;
            case RADIO_CELL_INFO_CDMA:
                ril_binder_radio_decode_cell_info_cdma(out, cell);
                break;
            case RADIO_CELL_INFO_LTE:
                ril_binder_radio_decode_cell_info_lte(out, cell);
                break;
            case RADIO_CELL_INFO_WCDMA:
                ril_binder_radio_decode_cell_info_wcdma(out, cell);
                break;
            case RADIO_CELL_INFO_TD_SCDMA:
                ril_binder_radio_decode_cell_info_tdscdma(out, cell);
                break;
            }
        }
        ok = TRUE;
    }
    return ok;
}

/*==========================================================================*
 * Calls
 *==========================================================================*/

static const RilBinderRadioCall ril_binder_radio_calls[] = {
    {
        RIL_REQUEST_GET_SIM_STATUS,
        RADIO_REQ_GET_ICC_CARD_STATUS,
        RADIO_RESP_GET_ICC_CARD_STATUS,
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_icc_card_status,
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
        ril_binder_radio_decode_icc_result,
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
        RADIO_REQ_SET_LOCATION_UPDATES,
        RADIO_RESP_SET_LOCATION_UPDATES,
        ril_binder_radio_encode_bool,
        NULL,
        "setLocationUpdates"
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
        RIL_RESPONSE_ACKNOWLEDGEMENT,
        RADIO_REQ_RESPONSE_ACKNOWLEDGEMENT,
        RADIO_RESP_NONE,
        NULL,
        NULL,
        "responseAcknowledgement"
    }
};

/*==========================================================================*
 * Events
 *==========================================================================*/

static const RilBinderRadioEvent ril_binder_radio_events[] = {
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
        RIL_UNSOL_UICC_SUBSCRIPTION_STATUS_CHANGED,
        RADIO_IND_SUBSCRIPTION_STATUS_CHANGED,
        ril_binder_radio_decode_bool_to_int_array,
        "subscriptionStatusChanged"
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
        ofono_warn("Failed to decode %s response", call->name);
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
        ofono_warn("Failed to decode %s indication", event->name);
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

    ofono_error("%sradio died", transport->log_prefix);
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
    const RilBinderRadioCall* call = g_hash_table_lookup(priv->resp_map,
        GINT_TO_POINTER(code));

    if (call) {
        GBinderReader copy;

        /* This is a known response */
        gbinder_reader_copy(&copy, args);
        DBG_(self, "IRadioResponse %u %s", code, call->name);
        return ril_binder_radio_handle_known_response(self, call, info, &copy);
    } else {
        DBG_(self, "IRadioResponse %u", code);
        ofono_warn("Unexpected response transaction %u", code);
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
    RilBinderRadioPriv* priv = self->priv;

    /* CONNECTED indication is slightly special */
    if (code == RADIO_IND_RIL_CONNECTED) {
        DBG_(self, "IRadioIndication %u rilConnected", code);
        ril_binder_radio_connected(self);
        return TRUE;
    } else {
        const RilBinderRadioEvent* event = g_hash_table_lookup(priv->unsol_map,
            GINT_TO_POINTER(code));

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
    const RilBinderRadioCall* call = g_hash_table_lookup(priv->req_map,
        GINT_TO_POINTER(code));

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
            ofono_warn("Failed to encode %s() arguments", call->name);
        }
        gbinder_local_request_unref(txreq);
    } else {
        ofono_warn("Unknown RIL command %u", code);
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
        GRILIO_RESPONSE_TYPE type;

        switch (info->type) {
        default:
            DBG_(self, "Unexpected response type %u", info->type);
            type = GRILIO_RESPONSE_NONE;
            break;
        case RADIO_RESP_SOLICITED:
            type = GRILIO_RESPONSE_SOLICITED;
            break;
        case RADIO_RESP_SOLICITED_ACK:
            type = GRILIO_RESPONSE_SOLICITED_ACK;
            break;
        case RADIO_RESP_SOLICITED_ACK_EXP:
            type = GRILIO_RESPONSE_SOLICITED_ACK_EXP;
            break;
        }

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

gboolean
ril_binder_radio_init_base(
    RilBinderRadio* self,
    GHashTable* args)
{
    RilBinderRadioPriv* priv = self->priv;
    const char* modem = ril_binder_radio_arg_modem(args);
    const char* dev = ril_binder_radio_arg_dev(args);
    const char* name = ril_binder_radio_arg_name(args);

    DBG("%s %s %s", modem, dev, name);

    self->modem = priv->modem = g_strdup(modem);
    self->radio = radio_instance_new(dev, name);
    if (self->radio) {
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
    guint i;
    RilBinderRadioPriv* priv = G_TYPE_INSTANCE_GET_PRIVATE
        (self, RIL_TYPE_BINDER_RADIO, RilBinderRadioPriv);

    self->priv = priv;
    priv->idle = gutil_idle_queue_new();
    priv->req_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->resp_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    priv->unsol_map = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (i = 0; i < G_N_ELEMENTS(ril_binder_radio_calls); i++) {
        const RilBinderRadioCall* call = ril_binder_radio_calls + i;

        g_hash_table_insert(priv->req_map, GINT_TO_POINTER(call->code),
            (gpointer)call);
        if (call->resp_tx) {
            g_hash_table_insert(priv->resp_map, GINT_TO_POINTER(call->resp_tx),
            (gpointer)call);
        }
    }

    for (i = 0; i < G_N_ELEMENTS(ril_binder_radio_events); i++) {
        const RilBinderRadioEvent* event = ril_binder_radio_events + i;

        g_hash_table_insert(priv->unsol_map, GINT_TO_POINTER(event->unsol_tx),
            (gpointer)event);
    }
}

static
void
ril_binder_radio_finalize(
    GObject* object)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(object);
    RilBinderRadioPriv* priv = self->priv;

    ril_binder_radio_drop_radio(self);
    gutil_idle_queue_cancel_all(priv->idle);
    gutil_idle_queue_unref(priv->idle);
    g_hash_table_destroy(priv->req_map);
    g_hash_table_destroy(priv->resp_map);
    g_hash_table_destroy(priv->unsol_map);
    if (priv->buf) {
        g_byte_array_unref(priv->buf);
    }
    g_free(priv->modem);
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
