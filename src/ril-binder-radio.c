/*
 * Copyright (C) 2018 Jolla Ltd.
 * Copyright (C) 2018 Slava Monich <slava.monich@jolla.com>
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
 *   3. Neither the name of Jolla Ltd nor the names of its contributors may
 *      be used to endorse or promote products derived from this software
 *      without specific prior written permission.
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

#include "ril-binder-radio.h"
#include "ril-binder-oemhook.h"

#include <ofono/ril-constants.h>
#include <ofono/log.h>

#include "grilio_encode.h"
#include "grilio_parser.h"
#include "grilio_request.h"
#include "grilio_transport_p.h"

#include <gbinder.h>

#include <gutil_idlequeue.h>
#include <gutil_log.h>
#include <gutil_misc.h>

typedef GRilIoTransportClass RilBinderRadioClass;

typedef struct ril_binder_radio_call {
    guint code;
    guint req_tx;
    guint resp_tx;
    gboolean (*encode)(GRilIoRequest* in, GBinderLocalRequest* out);
    gboolean (*decode)(GBinderReader* in, GByteArray* out);
    const char *name;
} RilBinderRadioCall;

typedef struct ril_binder_radio_event {
    guint code;
    guint unsol_tx;
    gboolean (*decode)(GBinderReader* in, GByteArray* out);
    const char* name;
} RilBinderRadioEvent;

typedef struct ril_binder_radio_failure_data {
    GRilIoTransport* transport;
    guint serial;
} RilBinderRadioFailureData;

struct ril_binder_radio {
    GRilIoTransport parent;
    GUtilIdleQueue* idle;
    GHashTable* req_map;      /* code -> RilBinderRadioCall */
    GHashTable* resp_map;     /* resp_tx -> RilBinderRadioCall */
    GHashTable* unsol_map;    /* unsol_tx -> RilBinderRadioEvent */
    GBinderServiceManager* sm;
    GBinderClient* client;
    GBinderRemoteObject* remote;
    GBinderLocalObject* response;
    GBinderLocalObject* indication;
    gulong death_id;
    RilBinderOemHook* oemhook;
    GByteArray* buf;
    char* fqname;
};

G_DEFINE_TYPE(RilBinderRadio, ril_binder_radio, GRILIO_TYPE_TRANSPORT)

#define PARENT_CLASS ril_binder_radio_parent_class
#define RIL_TYPE_BINDER_RADIO (ril_binder_radio_get_type())
#define RIL_BINDER_RADIO(obj) G_TYPE_CHECK_INSTANCE_CAST((obj), \
        RIL_TYPE_BINDER_RADIO, RilBinderRadio)

#define RADIO_IFACE(x)     "android.hardware.radio@1.0::" x
#define RADIO_REMOTE       RADIO_IFACE("IRadio")
#define RADIO_RESPONSE     RADIO_IFACE("IRadioResponse")
#define RADIO_INDICATION   RADIO_IFACE("IRadioIndication")

#define DBG_(self,fmt,args...) DBG("%s" fmt, (self)->parent.log_prefix, ##args)

/*==========================================================================*
 * Calls
 *==========================================================================*/

/* android.hardware.radio@1.0::IRadio */
#define RADIO_REQ_SET_RESPONSE_FUNCTIONS     (1) /* setResponseFunctions */
#define RADIO_REQ_RESPONSE_ACKNOWLEDGEMENT (130) /* responseAcknowledgement */

/* android.hardware.radio@1.0::IRadioResponse */
#define RADIO_RESP_ACKNOWLEDGE_REQUEST     (129) /* acknowledgeRequest */

/* android.hardware.radio@1.0::IRadioIndication */
#define RADIO_IND_RIL_CONNECTED             (33) /* rilConnected */

/* Types defined in types.hal */
typedef struct radio_card_status {
    gint32 cardState ALIGNED(4);
    gint32 universalPinState ALIGNED(4);
    gint32 gsmUmtsSubscriptionAppIndex ALIGNED(4);
    gint32 cdmaSubscriptionAppIndex ALIGNED(4);
    gint32 imsSubscriptionAppIndex ALIGNED(4);
    RadioVector apps ALIGNED(8); /* vec<RadioAppStatus> */
} ALIGNED(8) RadioCardStatus;
G_STATIC_ASSERT(sizeof(RadioCardStatus) == 40);

typedef struct radio_app_status {
    gint32 appType ALIGNED(4);
    gint32 appState ALIGNED(4);
    gint32 persoSubstate ALIGNED(4);
    RadioString aid ALIGNED(8);
    RadioString label ALIGNED(8);
    gint32 pinReplaced ALIGNED(4);
    gint32 pin1 ALIGNED(4);
    gint32 pin2 ALIGNED(4);
} ALIGNED(8) RadioAppStatus;
G_STATIC_ASSERT(sizeof(RadioAppStatus) == 64);

typedef struct radio_uus_info {
    gint32 uusType ALIGNED(4);
    gint32 uusDcs ALIGNED(4);
    RadioString uusData ALIGNED(8);
} ALIGNED(8) RadioUusInfo;
G_STATIC_ASSERT(sizeof(RadioUusInfo) == 24);

typedef struct radio_call {
    gint32 state ALIGNED(4);
    gint32 index ALIGNED(4);
    gint32 toa ALIGNED(4);
    guint8 isMpty ALIGNED(1);
    guint8 isMT ALIGNED(1);
    guint8 als ALIGNED(1);
    guint8 isVoice ALIGNED(1);
    guint8 isVoicePrivacy ALIGNED(1);
    RadioString number ALIGNED(8);
    gint32 numberPresentation ALIGNED(4);
    RadioString name ALIGNED(8);
    gint32 namePresentation ALIGNED(4);
    RadioVector uusInfo ALIGNED(8); /* vec<RadioUusInfo> */
} ALIGNED(8) RadioCall;
G_STATIC_ASSERT(sizeof(RadioCall) == 88);

typedef struct radio_dial {
    RadioString address ALIGNED(8);
    gint32 clir ALIGNED(4);
    RadioVector uusInfo ALIGNED(8); /* vec<RadioUusInfo> */
} ALIGNED(8) RadioDial;
G_STATIC_ASSERT(sizeof(RadioDial) == 40);

typedef struct radio_last_call_fail_cause_info {
    gint32 causeCode ALIGNED(4);
    RadioString vendorCause ALIGNED(8);
} ALIGNED(8) RadioLastCallFailCauseInfo;
G_STATIC_ASSERT(sizeof(RadioLastCallFailCauseInfo) == 24);

enum radio_operator_status {
    RADIO_OP_STATUS_UNKNOWN = 0,
    RADIO_OP_AVAILABLE,
    RADIO_OP_CURRENT,
    RADIO_OP_FORBIDDEN
};

typedef struct radio_operator_info {
    RadioString alphaLong ALIGNED(8);
    RadioString alphaShort ALIGNED(8);
    RadioString operatorNumeric ALIGNED(8);
    gint32 status ALIGNED(4);
} ALIGNED(8) RadioOperatorInfo;
G_STATIC_ASSERT(sizeof(RadioOperatorInfo) == 56);

typedef struct radio_data_profile {
    gint32 profileId ALIGNED(4);
    RadioString apn ALIGNED(8);
    RadioString protocol ALIGNED(8);
    RadioString roamingProtocol ALIGNED(8);
    gint32 authType ALIGNED(4);
    RadioString user ALIGNED(8);
    RadioString password ALIGNED(8);
    gint32 type ALIGNED(4);
    gint32 maxConnsTime ALIGNED(4);
    gint32 maxConns ALIGNED(4);
    gint32 waitTime ALIGNED(4);
    guint8 enabled ALIGNED(1);
    gint32 supportedApnTypesBitmap ALIGNED(4);
    gint32 bearerBitmap ALIGNED(4);
    gint32 mtu ALIGNED(4);
    gint32 mvnoType ALIGNED(4);
    RadioString mvnoMatchData ALIGNED(8);
} ALIGNED(8) RadioDataProfile;
G_STATIC_ASSERT(sizeof(RadioDataProfile) == 152);

typedef struct radio_data_call {
    gint32 status ALIGNED(4);
    gint32 suggestedRetryTime ALIGNED(4);
    gint32 cid ALIGNED(4);
    gint32 active ALIGNED(4);
    RadioString type ALIGNED(8);
    RadioString ifname ALIGNED(8);
    RadioString addresses ALIGNED(8);
    RadioString dnses ALIGNED(8);
    RadioString gateways ALIGNED(8);
    RadioString pcscf ALIGNED(8);
    gint32 mtu ALIGNED(4);
} ALIGNED(8) RadioDataCall;
G_STATIC_ASSERT(sizeof(RadioDataCall) == 120);
#define DATA_CALL_VERSION (11)

#define DATA_CALL_VERSION (11)

typedef struct radio_sms_write_args {
    gint32 status ALIGNED(4);
    RadioString pdu ALIGNED(8);
    RadioString smsc ALIGNED(8);
} ALIGNED(8) RadioSmsWriteArgs;
G_STATIC_ASSERT(sizeof(RadioSmsWriteArgs) == 40);

typedef struct GsmSmsMessage {
    RadioString smscPdu ALIGNED(8);
    RadioString pdu ALIGNED(8);
} ALIGNED(8) RadioGsmSmsMessage;
G_STATIC_ASSERT(sizeof(RadioGsmSmsMessage) == 32);

typedef struct SendSmsResult {
    gint32 messageRef ALIGNED(4);
    RadioString ackPDU ALIGNED(8);
    gint32 errorCode ALIGNED(4);
} ALIGNED(8) RadioSendSmsResult;
G_STATIC_ASSERT(sizeof(RadioSendSmsResult) == 32);

typedef struct radio_icc_io {
    gint32 command ALIGNED(4);
    gint32 fileId ALIGNED(4);
    RadioString path ALIGNED(8);
    gint32 p1 ALIGNED(4);
    gint32 p2 ALIGNED(4);
    gint32 p3 ALIGNED(4);
    RadioString data ALIGNED(8);
    RadioString pin2 ALIGNED(8);
    RadioString aid ALIGNED(8);
} ALIGNED(8) RadioIccIo;
G_STATIC_ASSERT(sizeof(RadioIccIo) == 88);

typedef struct radio_icc_io_result {
    gint32 sw1 ALIGNED(4);
    gint32 sw2 ALIGNED(4);
    RadioString response ALIGNED(8);
} ALIGNED(8) RadioIccIoResult;
G_STATIC_ASSERT(sizeof(RadioIccIoResult) == 24);


typedef struct radio_call_forward_info {
    gint32 status ALIGNED(4);
    gint32 reason ALIGNED(4);
    gint32 serviceClass ALIGNED(4);
    gint32 toa ALIGNED(4);
    RadioString number ALIGNED(8);
    gint32 timeSeconds ALIGNED(4);
} ALIGNED(8) RadioCallForwardInfo;
G_STATIC_ASSERT(sizeof(RadioCallForwardInfo) == 40);

typedef struct radio_cell_identity {
    guint32 cellInfoType ALIGNED(4);
    RadioVector gsm ALIGNED(8);     /* vec<RadioCellIdentityGsm> */
    RadioVector wcdma ALIGNED(8);   /* vec<RadioCellIdentityWcdma> */
    RadioVector cdma ALIGNED(8);    /* vec<RadioCellIdentityCdma> */
    RadioVector lte ALIGNED(8);     /* vec<RadioCellIdentityLte> */
    RadioVector tdscdma ALIGNED(8); /* vec<RadioCellIdentityTdscdma> */
} ALIGNED(8) RadioCellIdentity;
G_STATIC_ASSERT(sizeof(RadioCellIdentity) == 88);

typedef struct radio_cell_identity_gsm {
    RadioString mcc ALIGNED(8);
    RadioString mnc ALIGNED(8);
    gint32 lac ALIGNED(4);
    gint32 cid ALIGNED(4);
    gint32 arfcn ALIGNED(4);
    guint8 bsic ALIGNED(1);
} ALIGNED(8) RadioCellIdentityGsm;
G_STATIC_ASSERT(sizeof(RadioCellIdentityGsm) == 48);

typedef struct radio_cell_identity_wcdma {
    RadioString mcc ALIGNED(8);
    RadioString mnc ALIGNED(8);
    gint32 lac ALIGNED(4);
    gint32 cid ALIGNED(4);
    gint32 psc ALIGNED(4);
    gint32 uarfcn ALIGNED(4);
} ALIGNED(8) RadioCellIdentityWcdma;
G_STATIC_ASSERT(sizeof(RadioCellIdentityWcdma) == 48);

typedef struct radio_cell_identity_cdma {
    gint32 networkId ALIGNED(4);
    gint32 systemId ALIGNED(4);
    gint32 baseStationId ALIGNED(4);
    gint32 longitude ALIGNED(4);
    gint32 latitude ALIGNED(4);
} ALIGNED(4) RadioCellIdentityCdma;
G_STATIC_ASSERT(sizeof(RadioCellIdentityCdma) == 20);

typedef struct radio_cell_identity_lte {
    RadioString mcc ALIGNED(8);
    RadioString mnc ALIGNED(8);
    gint32 ci ALIGNED(4);
    gint32 pci ALIGNED(4);
    gint32 tac ALIGNED(4);
    gint32 earfcn ALIGNED(4);
} ALIGNED(8) RadioCellIdentityLte;
G_STATIC_ASSERT(sizeof(RadioCellIdentityLte) == 48);

typedef struct radio_cell_identity_tdscdma {
    RadioString mcc ALIGNED(8);
    RadioString mnc ALIGNED(8);
    gint32 lac ALIGNED(4);
    gint32 cid ALIGNED(4);
    gint32 cpid ALIGNED(4);
} ALIGNED(8) RadioCellIdentityTdscdma;
G_STATIC_ASSERT(sizeof(RadioCellIdentityTdscdma) == 48);

typedef struct radio_voice_reg_state_result {
    gint32 regState ALIGNED(4);
    gint32 rat ALIGNED(4);
    guint8 cssSupported ALIGNED(1);
    gint32 roamingIndicator ALIGNED(4);
    gint32 systemIsInPrl ALIGNED(4);
    gint32 defaultRoamingIndicator ALIGNED(4);
    gint32 reasonForDenial ALIGNED(4);
    RadioCellIdentity cellIdentity ALIGNED(8);
} ALIGNED(8) RadioVoiceRegStateResult;
G_STATIC_ASSERT(sizeof(RadioVoiceRegStateResult) == 120);

typedef struct radio_DataRegStateResult {
    gint32 regState ALIGNED(4);
    gint32 rat ALIGNED(4);
    gint32 reasonDataDenied ALIGNED(4);
    gint32 maxDataCalls ALIGNED(4);
    RadioCellIdentity cellIdentity ALIGNED(8);
} ALIGNED(8) RadioDataRegStateResult;
G_STATIC_ASSERT(sizeof(RadioDataRegStateResult) == 104);

typedef struct radio_gsm_signal_strength {
    guint32 signalStrength ALIGNED(4);
    guint32 bitErrorRate ALIGNED(4);
    gint32 timingAdvance ALIGNED(4);
} ALIGNED(4) RadioGsmSignalStrength;
G_STATIC_ASSERT(sizeof(RadioGsmSignalStrength) == 12);

typedef struct radio_wcdma_signal_strength {
    gint32 signalStrength ALIGNED(4);
    gint32 bitErrorRate ALIGNED(4);
} ALIGNED(4) RadioWcdmaSignalStrength;
G_STATIC_ASSERT(sizeof(RadioWcdmaSignalStrength) == 8);

typedef struct radio_cdma_signal_strength {
    guint32 dbm ALIGNED(4);
    guint32 ecio ALIGNED(4);
} ALIGNED(4) RadioCdmaSignalStrength;
G_STATIC_ASSERT(sizeof(RadioCdmaSignalStrength) == 8);

typedef struct radio_evdo_signal_strength {
    guint32 dbm ALIGNED(4);
    guint32 ecio ALIGNED(4);
    guint32 signalNoiseRatio ALIGNED(4);
} ALIGNED(4) RadioEvdoSignalStrength;
G_STATIC_ASSERT(sizeof(RadioEvdoSignalStrength) == 12);

typedef struct radio_lte_signal_strength {
    guint32 signalStrength ALIGNED(4);
    guint32 rsrp ALIGNED(4);
    guint32 rsrq ALIGNED(4);
    gint32 rssnr ALIGNED(4);
    guint32 cqi ALIGNED(4);
    guint32 timingAdvance ALIGNED(4);
} ALIGNED(4) RadioLteSignalStrength;
G_STATIC_ASSERT(sizeof(RadioLteSignalStrength) == 24);

typedef struct radio_tdscdma_signal_strength {
    guint32 rscp ALIGNED(4);
} ALIGNED(4) RadioTdScdmaSignalStrength;
G_STATIC_ASSERT(sizeof(RadioTdScdmaSignalStrength) == 4);

typedef struct radio_signal_strength {
    RadioGsmSignalStrength gw ALIGNED(4);
    RadioCdmaSignalStrength cdma ALIGNED(4);
    RadioEvdoSignalStrength evdo ALIGNED(4);
    RadioLteSignalStrength lte ALIGNED(4);
    RadioTdScdmaSignalStrength tdScdma ALIGNED(4);
} ALIGNED(4) RadioSignalStrength;
G_STATIC_ASSERT(sizeof(RadioSignalStrength) == 60);

typedef struct radio_gsm_broadcast_sms_config {
    gint32 fromServiceId ALIGNED(4);
    gint32 toServiceId ALIGNED(4);
    gint32 fromCodeScheme ALIGNED(4);
    gint32 toCodeScheme ALIGNED(4);
    guint8 selected ALIGNED(1);
} ALIGNED(4) RadioGsmBroadcastSmsConfig;
G_STATIC_ASSERT(sizeof(RadioGsmBroadcastSmsConfig) == 20);

typedef struct radio_supp_svc_notification {
    guint8 isMT ALIGNED(1);
    gint32 code ALIGNED(4);
    gint32 index ALIGNED(4);
    gint32 type ALIGNED(4);
    RadioString number ALIGNED(8);
} ALIGNED(8) RadioSuppSvcNotification;
G_STATIC_ASSERT(sizeof(RadioSuppSvcNotification) == 32);

typedef struct radio_sim_refresh {
    gint32 type ALIGNED(4);
    gint32 efId ALIGNED(4);
    RadioString aid ALIGNED(8);
} ALIGNED(8) RadioSimRefresh;
G_STATIC_ASSERT(sizeof(RadioSimRefresh) == 24);

/*==========================================================================*
 * Utilities
 *==========================================================================*/

static
gboolean
ril_binder_radio_string_init(
    RadioString* str,
    const char* chars)
{
    str->owns_buffer = TRUE;
    if (chars) {
        str->data.str = chars;
        str->len = strlen(chars);
        return TRUE;
    } else {
        /* Replace NULL strings with empty strings */
        str->data.str = "";
        str->len = 0;
        return FALSE;
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
 * @param serial Serial number of request.
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
        RadioDial* dial = g_new0(RadioDial, 1);

        ril_binder_radio_string_init(&dial->address, number);

        /* Pointers must be alive for the lifetime of the request */
        gbinder_local_request_cleanup(out, g_free, dial);
        gbinder_local_request_cleanup(out, g_free, number);

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent.index = gbinder_writer_append_buffer_object(&writer,
            dial, sizeof(*dial));

        /* Strings are NULL-terminated, hence len + 1 */
        parent.offset = G_STRUCT_OFFSET(RadioDial, address.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            dial->address.data.str, dial->address.len + 1, &parent);

        /* UUS information is empty but we still need to write a buffer */
        parent.offset = G_STRUCT_OFFSET(RadioDial, uusInfo.data.ptr);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            dial /* arbitrary pointer */, 0, &parent);
        return TRUE;
    }
    return FALSE;
}

/**
 * @param serial Serial number of request.
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
        RadioGsmSmsMessage* sms = g_new0(RadioGsmSmsMessage, 1);
        GBinderWriter writer;
        GBinderParent parent;

        /* Pointers must be alive for the lifetime of the request */
        gbinder_local_request_cleanup(out, g_free, sms);
        if (ril_binder_radio_string_init(&sms->smscPdu, smsc)) {
            gbinder_local_request_cleanup(out, g_free, smsc);
        }
        if (ril_binder_radio_string_init(&sms->pdu, pdu)) {
            gbinder_local_request_cleanup(out, g_free, pdu);
        }

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent.index = gbinder_writer_append_buffer_object(&writer,
            sms, sizeof(*sms));

        /* Strings are NULL-terminated, hence len + 1 */
        parent.offset = G_STRUCT_OFFSET(RadioGsmSmsMessage, smscPdu.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            sms->smscPdu.data.str, sms->smscPdu.len + 1, &parent);

        parent.offset = G_STRUCT_OFFSET(RadioGsmSmsMessage, pdu.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
	    sms->pdu.data.str, sms->pdu.len + 1, &parent);
        return TRUE;
    }

    g_free(smsc);
    g_free(pdu);
    return FALSE;
}

/**
 * @param serial Serial number of request.
 * @param radioTechnology Radio technology to use.
 * @param dataProfileInfo data profile info.
 * @param modemCognitive Indicating this profile was sent to the modem
 *                       through setDataProfile earlier.
 * @param roamingAllowed Indicating data roaming is allowed or not by the user.
 * @param isRoaming Indicating the device is roaming or not.
 */
static
gboolean
ril_binder_radio_encode_setup_data_call(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    gboolean ok = FALSE;
    GRilIoParser parser;
    gint32 count, tech, auth;
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
        grilio_parser_skip_string(&parser) &&
        (apn = grilio_parser_get_utf8(&parser)) != NULL &&
        (user = grilio_parser_get_utf8(&parser)) != NULL &&
        (password = grilio_parser_get_utf8(&parser)) != NULL &&
        (auth_str = grilio_parser_get_utf8(&parser)) != NULL &&
        gutil_parse_int(auth_str, 10, &auth) &&
        (proto = grilio_parser_get_utf8(&parser)) != NULL) {
        GBinderWriter writer;
        GBinderParent parent;
        RadioDataProfile* profile = g_new0(RadioDataProfile, 1);

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

        profile->authType = auth;

        /* Pointers must be alive for the lifetime of the request */
        gbinder_local_request_cleanup(out, g_free, profile);
        if (ril_binder_radio_string_init(&profile->apn, apn)) {
            gbinder_local_request_cleanup(out, g_free, apn);
        }
        ril_binder_radio_string_init(&profile->roamingProtocol, proto);
        if (ril_binder_radio_string_init(&profile->protocol, proto)) {
            gbinder_local_request_cleanup(out, g_free, proto);
        }
        if (ril_binder_radio_string_init(&profile->user, user)) {
            gbinder_local_request_cleanup(out, g_free, user);
        }
        if (ril_binder_radio_string_init(&profile->password, password)) {
            gbinder_local_request_cleanup(out, g_free, password);
        }

        /* Write the parcel */
        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        gbinder_writer_append_int32(&writer, tech); /* radioTechnology */
        parent.index = gbinder_writer_append_buffer_object(&writer,
            profile, sizeof(*profile));             /* dataProfileInfo */

        /* Strings are NULL-terminated, hence len + 1 */
        parent.offset = G_STRUCT_OFFSET(RadioDataProfile, apn.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
	    profile->apn.data.str, profile->apn.len + 1, &parent);

        parent.offset = G_STRUCT_OFFSET(RadioDataProfile, protocol.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
	    profile->protocol.data.str, profile->protocol.len + 1, &parent);

        parent.offset = G_STRUCT_OFFSET(RadioDataProfile,
            roamingProtocol.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
	    profile->roamingProtocol.data.str,
            profile->roamingProtocol.len + 1, &parent);

        parent.offset = G_STRUCT_OFFSET(RadioDataProfile, user.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
	    profile->user.data.str, profile->user.len + 1, &parent);

        parent.offset = G_STRUCT_OFFSET(RadioDataProfile, password.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
	    profile->password.data.str, profile->password.len + 1, &parent);

        profile->mvnoMatchData.data.str = "";
        parent.offset = G_STRUCT_OFFSET(RadioDataProfile,
            mvnoMatchData.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
	    profile->mvnoMatchData.data.str,
            profile->mvnoMatchData.len + 1, &parent);

        /* Done with DataProfileInfo */
        gbinder_writer_append_bool(&writer, FALSE); /* modemCognitive */
        gbinder_writer_append_bool(&writer, TRUE);  /* roamingAllowed */
        gbinder_writer_append_bool(&writer, FALSE); /* isRoaming */

        ok = TRUE;
    } else {
        g_free(apn);
        g_free(user);
        g_free(password);
        g_free(proto);
    }

    g_free(tech_str);
    g_free(auth_str);
    return ok;
}

/**
 * @param serial Serial number of request.
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
        GBinderParent parent;

        /* Pointers must be alive for the lifetime of the request */
        gbinder_local_request_cleanup(out, g_free, sms);
        if (ril_binder_radio_string_init(&sms->pdu, pdu)) {
            gbinder_local_request_cleanup(out, g_free, pdu);
        }
        if (ril_binder_radio_string_init(&sms->smsc, smsc)) {
            gbinder_local_request_cleanup(out, g_free, smsc);
        }

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent.index = gbinder_writer_append_buffer_object(&writer,
            sms, sizeof(*sms));

        /* Strings are NULL-terminated, hence len + 1 */
        parent.offset = G_STRUCT_OFFSET(RadioSmsWriteArgs, pdu.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
	    sms->pdu.data.str, sms->pdu.len + 1, &parent);

        parent.offset = G_STRUCT_OFFSET(RadioSmsWriteArgs, smsc.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            sms->smsc.data.str, sms->smsc.len + 1, &parent);

        return TRUE;
    }

    g_free(smsc);
    g_free(pdu);
    g_free(sms);
    return FALSE;
}

/**
 * @param serial Serial number of request.
 * @param iccIo IccIo
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
        GBinderParent parent;

        /* Pointers must be alive for the lifetime of the request */
        gbinder_local_request_cleanup(out, g_free, io);
        if (ril_binder_radio_string_init(&io->path, path)) {
            gbinder_local_request_cleanup(out, g_free, path);
        }
        if (ril_binder_radio_string_init(&io->data, data)) {
            gbinder_local_request_cleanup(out, g_free, data);
        }
        if (ril_binder_radio_string_init(&io->pin2, pin2)) {
            gbinder_local_request_cleanup(out, g_free, pin2);
        }
        if (ril_binder_radio_string_init(&io->aid, aid)) {
            gbinder_local_request_cleanup(out, g_free, aid);
        }

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent.index = gbinder_writer_append_buffer_object(&writer,
            io, sizeof(*io));

        /* Strings are NULL-terminated, hence len + 1 */
        parent.offset = G_STRUCT_OFFSET(RadioIccIo, path.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            io->path.data.str, io->path.len + 1, &parent);

        parent.offset = G_STRUCT_OFFSET(RadioIccIo, data.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            io->data.data.str, io->data.len + 1, &parent);

        parent.offset = G_STRUCT_OFFSET(RadioIccIo, pin2.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            io->pin2.data.str, io->pin2.len + 1, &parent);

        parent.offset = G_STRUCT_OFFSET(RadioIccIo, aid.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            io->aid.data.str, io->aid.len + 1, &parent);
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
 * @param serial Serial number of request.
 * @param callInfo CallForwardInfo
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
        GBinderParent parent;

        /* Pointers must be alive for the lifetime of the request */
        gbinder_local_request_cleanup(out, g_free, info);
        if (ril_binder_radio_string_init(&info->number, number)) {
            gbinder_local_request_cleanup(out, g_free, number);
        }

        gbinder_local_request_init_writer(out, &writer);
        gbinder_writer_append_int32(&writer, grilio_request_serial(in));

        /* Write the parent structure */
        parent.index = gbinder_writer_append_buffer_object(&writer,
            info, sizeof(*info));

        /* Strings are NULL-terminated, hence len + 1 */
        parent.offset = G_STRUCT_OFFSET(RadioCallForwardInfo, number.data.str);
        gbinder_writer_append_buffer_object_with_parent(&writer,
            info->number.data.str, info->number.len + 1, &parent);
        return TRUE;
    }

    g_free(info);
    g_free(number);
    return FALSE;
}

/**
 * @param serial Serial number of request
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
 * @param serial Serial number of request.
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
    if (grilio_parser_get_int32(&parser, &count) && count == 4 &&
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
 * @param serial Serial number of request.
 * @param configInfo Setting of GSM/WCDMA Cell broadcast config
 */
static
gboolean
ril_binder_radio_encode_gsm_broadcast_sms_config(
    GRilIoRequest* in,
    GBinderLocalRequest* out)
{
    gboolean ok = FALSE;
    GRilIoParser parser;
    gint32 count;

    ril_binder_radio_init_parser(&parser, in);
    if (grilio_parser_get_int32(&parser, &count)) {
        GBinderWriter writer;
        GBinderParent parent;
        RadioVector* vec = g_new0(RadioVector, 1);
        RadioGsmBroadcastSmsConfig* configs = NULL;
        guint i;

        vec->count = count;
        vec->owns_buffer = TRUE;
        gbinder_local_request_cleanup(out, g_free, vec);
        if (count > 0) {
            configs = g_new0(RadioGsmBroadcastSmsConfig, count);
            gbinder_local_request_cleanup(out, g_free, configs);
            vec->data.ptr = configs;
        }

        ok = TRUE;
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
            /* The entire input has been successfully parsed */
            gbinder_local_request_init_writer(out, &writer);
            gbinder_writer_append_int32(&writer, grilio_request_serial(in));

            /* Write the parent structure */
            parent.offset = G_STRUCT_OFFSET(RadioVector, data.ptr);
            parent.index = gbinder_writer_append_buffer_object(&writer,
                vec, sizeof(*vec));

            if (count > 0) {
                gbinder_writer_append_buffer_object_with_parent(&writer,
                    configs, sizeof(configs[0]) * count, &parent);
            }
        }
    }
    return ok;
}


/*==========================================================================*
 * Decoders (binder -> plugin)
 *==========================================================================*/

#define ril_binder_radio_decode_array(type,in,count) \
	((type*)ril_binder_radio_decode_array1(in, sizeof(type),count))

static
const void*
ril_binder_radio_decode_array1(
    GBinderReader* in,
    guint elem_size,
    guint32* count)
{
    const void* result = NULL;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioVector)) {
        const RadioVector* array = buf->data;

        /* The contents comes as another buffer */
        if (array->data.ptr) {
            GBinderBuffer* data = gbinder_reader_read_buffer(in);

            if (data && data->data == array->data.ptr &&
                data->size == array->count * elem_size) {
                if (count) {
                    *count = array->count;
                }
                result = data->data;
            }
            gbinder_buffer_free(data);
        } else {
            static const gsize dummy = 0;

            if (count) {
                *count = 0;
            }
            result = &dummy;
        }
    }
    gbinder_buffer_free(buf);
    return result;
}

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
    char* str = gbinder_reader_read_hidl_string(in);

    if (str) {
        grilio_encode_utf8(out, str);
        g_free(str);
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
        char* str = gbinder_reader_read_hidl_string(in);

        if (str) {
            grilio_encode_utf8(out, str);
            g_free(str);
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
    guint32 count = 0;
    const gint32* values = ril_binder_radio_decode_array(gint32, in, &count);

    if (values) {
        guint i;

        grilio_encode_int32(out, count);
        for (i = 0; i < count; i++) {
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
    guint32 size = 0;
    const guint8* ptr = ril_binder_radio_decode_array(guint8, in, &size);

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
    guint32 size = 0;
    const guint8* bytes = ril_binder_radio_decode_array(guint8, in, &size);

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
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioCardStatus)) {
        const RadioCardStatus* sim = buf->data;
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
    gbinder_buffer_free(buf);
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
    gboolean ok = FALSE;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioVoiceRegStateResult)) {
        const RadioVoiceRegStateResult* reg = buf->data;

        grilio_encode_int32(out, 5);
        grilio_encode_format(out, "%d", reg->regState);
        grilio_encode_utf8(out, ""); /* slac */
        grilio_encode_utf8(out, ""); /* sci */
        grilio_encode_format(out, "%d", reg->rat);
        grilio_encode_format(out, "%d", reg->reasonForDenial);
        ok = TRUE;
    }
    gbinder_buffer_free(buf);
    return ok;
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
    gboolean ok = FALSE;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioDataRegStateResult)) {
        const RadioDataRegStateResult* reg = buf->data;

        grilio_encode_int32(out, 6);
        grilio_encode_format(out, "%d", reg->regState);
        grilio_encode_utf8(out, ""); /* slac */
        grilio_encode_utf8(out, ""); /* sci */
        grilio_encode_format(out, "%d", reg->rat);
        grilio_encode_format(out, "%d", reg->reasonDataDenied);
        grilio_encode_format(out, "%d", reg->maxDataCalls);
        ok = TRUE;
    }
    gbinder_buffer_free(buf);
    return ok;
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
    gboolean ok = FALSE;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioSendSmsResult)) {
        const RadioSendSmsResult* result = buf->data;

        grilio_encode_int32(out, result->messageRef);
        grilio_encode_utf8(out, result->ackPDU.data.str);
        grilio_encode_int32(out, result->errorCode);
        ok = TRUE;
    }
    gbinder_buffer_free(buf);
    return ok;
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
    gboolean ok = FALSE;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioIccIoResult)) {
        const RadioIccIoResult* result = buf->data;

        grilio_encode_int32(out, result->sw1);
        grilio_encode_int32(out, result->sw2);
        grilio_encode_utf8(out, result->response.data.str);
        ok = TRUE;
    }
    gbinder_buffer_free(buf);
    return ok;
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
    guint32 count = 0;
    const RadioCallForwardInfo* infos =
        ril_binder_radio_decode_array(RadioCallForwardInfo, in, &count);

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
    guint32 count = 0;
    const RadioCall* calls =
        ril_binder_radio_decode_array(RadioCall, in, &count);

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
    gboolean ok = FALSE;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioLastCallFailCauseInfo)) {
        const RadioLastCallFailCauseInfo* info = buf->data;

        grilio_encode_int32(out, info->causeCode);
        grilio_encode_utf8(out, info->vendorCause.data.str);
        ok = TRUE;
    }
    gbinder_buffer_free(buf);
    return ok;
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
    guint32 count = 0;
    const RadioOperatorInfo* ops =
        ril_binder_radio_decode_array(RadioOperatorInfo, in, &count);

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

static
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
    guint32 count = 0;
    const RadioDataCall* calls =
        ril_binder_radio_decode_array(RadioDataCall, in, &count);

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
    gboolean ok = FALSE;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioDataCall)) {
        grilio_encode_int32(out, DATA_CALL_VERSION);
        grilio_encode_int32(out, 1);
        ril_binder_radio_decode_data_call(out, buf->data);
        ok = TRUE;
    }
    gbinder_buffer_free(buf);
    return ok;
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
    guint32 count = 0;
    const RadioGsmBroadcastSmsConfig* configs =
        ril_binder_radio_decode_array(RadioGsmBroadcastSmsConfig, in, &count);

    if (configs) {
        guint i;
		
        grilio_encode_int32(out, count);
        for (i = 0; i < count; i++) {
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
    gboolean ok = FALSE;
    char* imei = gbinder_reader_read_hidl_string(in);
    char* imeisv = gbinder_reader_read_hidl_string(in);
    char* esn = gbinder_reader_read_hidl_string(in);
    char* meid = gbinder_reader_read_hidl_string(in);

    if (imei || imeisv || esn || meid) {
        grilio_encode_int32(out, 4);
        grilio_encode_utf8(out, imei);
        grilio_encode_utf8(out, imeisv);
        grilio_encode_utf8(out, esn);
        grilio_encode_utf8(out, meid);
        ok = TRUE;
    }

    g_free(imei);
    g_free(imeisv);
    g_free(esn);
    g_free(meid);
    return ok;
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
        char* msg = gbinder_reader_read_hidl_string(in);

        grilio_encode_int32(out, 2);
        grilio_encode_format(out, "%u", code);
        grilio_encode_utf8(out, msg);
        g_free(msg);
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
    gboolean ok = FALSE;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioSignalStrength)) {
        const RadioSignalStrength* strength = buf->data;

        /* RIL_SignalStrength_v6 */
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

        /* LTE_SignalStrength */
        grilio_encode_int32(out, strength->lte.signalStrength);
        grilio_encode_int32(out, strength->lte.rsrp);
        /* The rest is ignored */

        ok = TRUE;
    }
    gbinder_buffer_free(buf);
    return ok;
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
    gboolean ok = FALSE;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioSuppSvcNotification)) {
        const RadioSuppSvcNotification* notify = buf->data;

        grilio_encode_int32(out, notify->isMT);
        grilio_encode_int32(out, notify->code);
        grilio_encode_int32(out, notify->index);
        grilio_encode_int32(out, notify->type);
        grilio_encode_utf8(out, notify->number.data.str);
        ok = TRUE;
    }
    gbinder_buffer_free(buf);
    return ok;
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
    gboolean ok = FALSE;
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(RadioSimRefresh)) {
        const RadioSimRefresh* refresh = buf->data;

        grilio_encode_int32(out, refresh->type);
        grilio_encode_int32(out, refresh->efId);
        grilio_encode_utf8(out, refresh->aid.data.str);
        ok = TRUE;
    }
    gbinder_buffer_free(buf);
    return ok;
}

/*==========================================================================*
 * Calls
 *==========================================================================*/

static const RilBinderRadioCall ril_binder_radio_calls[] = {
    {
        RIL_REQUEST_GET_SIM_STATUS,
        2, /* getIccCardStatus */
        1, /* getIccCardStatusResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_icc_card_status,
        "getIccCardStatus"
    },{
        RIL_REQUEST_ENTER_SIM_PIN,
        3, /* supplyIccPinForApp */
        2, /* supplyIccPinForAppResponse */
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyIccPinForApp"
    },{
        RIL_REQUEST_ENTER_SIM_PUK,
        4, /* supplyIccPukForApp */
        3, /* supplyIccPukForAppResponse */
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyIccPukForApp"
    },{
        RIL_REQUEST_ENTER_SIM_PIN2,
        5, /* supplyIccPin2ForApp */
        4, /* supplyIccPin2ForAppResponse */
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyIccPin2ForApp"
    },{
        RIL_REQUEST_ENTER_SIM_PUK2,
        6, /* supplyIccPuk2ForApp */
        5, /* supplyIccPuk2ForAppResponse */
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyIccPuk2ForApp"
    },{
        RIL_REQUEST_CHANGE_SIM_PIN,
        7, /* changeIccPinForApp */
        6, /* changeIccPinForAppResponse */
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "changeIccPinForApp"
    },{
        RIL_REQUEST_CHANGE_SIM_PIN2,
        8, /* changeIccPin2ForApp */
        7, /* changeIccPin2ForAppResponse */
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "changeIccPin2ForApp"
    },{
        RIL_REQUEST_ENTER_NETWORK_DEPERSONALIZATION,
        9, /* supplyNetworkDepersonalization */
        8, /* supplyNetworkDepersonalizationResponse */
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_int_1,
        "supplyNetworkDepersonalization"
    },{
        RIL_REQUEST_GET_CURRENT_CALLS,
        10, /* getCurrentCalls */
        9,  /* getCurrentCallsResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_call_list,
        "getCurrentCalls"
    },{
        RIL_REQUEST_DIAL,
        11, /* dial */
        10, /* dialResponse */
        ril_binder_radio_encode_dial,
        NULL,
        "dial"
    },{
        RIL_REQUEST_GET_IMSI,
        12, /* getImsiForApp */
        11, /* getIMSIForAppResponse */
        ril_binder_radio_encode_strings,
        ril_binder_radio_decode_string,
        "getImsiForApp"
    },{
        RIL_REQUEST_HANGUP,
        13, /* hangup */
        12, /* hangupConnectionResponse */
        ril_binder_radio_encode_ints,
        NULL,
        "hangup"
    },{
        RIL_REQUEST_HANGUP_WAITING_OR_BACKGROUND,
        14, /* hangupWaitingOrBackground */
        13, /* hangupWaitingOrBackgroundResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "hangupWaitingOrBackground"
    },{
        RIL_REQUEST_HANGUP_FOREGROUND_RESUME_BACKGROUND,
        15, /* hangupForegroundResumeBackground */
        14, /* hangupForegroundResumeBackgroundResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "hangupForegroundResumeBackground"
    },{
        RIL_REQUEST_SWITCH_HOLDING_AND_ACTIVE,
        16, /* switchWaitingOrHoldingAndActive */
        15, /* switchWaitingOrHoldingAndActiveResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "switchWaitingOrHoldingAndActive"
    },{
        RIL_REQUEST_CONFERENCE,
        17, /* conference */
        16, /* conferenceResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "conference"
    },{
        RIL_REQUEST_UDUB,
        18, /* rejectCall */
        17, /* rejectCallResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "rejectCall"
    },{
        RIL_REQUEST_LAST_CALL_FAIL_CAUSE,
        19, /* getLastCallFailCause */
        18, /* getLastCallFailCauseResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_last_call_fail_cause,
        "getLastCallFailCause"
    },{
        RIL_REQUEST_SIGNAL_STRENGTH,
        20, /* getSignalStrength */
        19, /* getSignalStrengthResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_signal_strength,
        "getSignalStrength"
    },{
        RIL_REQUEST_VOICE_REGISTRATION_STATE,
        21, /* getVoiceRegistrationState */
        20, /* getVoiceRegistrationStateResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_voice_reg_state,
        "getVoiceRegistrationState"
    },{
        RIL_REQUEST_DATA_REGISTRATION_STATE,
        22, /* getDataRegistrationState */
        21, /* getDataRegistrationStateResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_data_reg_state,
        "getDataRegistrationState"
    },{
        RIL_REQUEST_OPERATOR,
        23, /* getOperator */
        22, /* getOperatorResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_string_3,
        "getOperator"
    },{
        RIL_REQUEST_RADIO_POWER,
        24, /* setRadioPower */
        23, /* setRadioPowerResponse */
        ril_binder_radio_encode_bool,
        NULL,
        "setRadioPower"
    },{
        RIL_REQUEST_DTMF,
        25, /* sendDtmf */
        24, /* sendDtmfResponse */
        ril_binder_radio_encode_string,
        NULL,
        "sendDtmf"
    },{
        RIL_REQUEST_SEND_SMS,
        26, /* sendSms */
        25, /* sendSmsResponse */
        ril_binder_radio_encode_gsm_sms_message,
        ril_binder_radio_decode_sms_send_result,
        "sendSms"
    },{
        RIL_REQUEST_SEND_SMS_EXPECT_MORE,
        27, /* sendSMSExpectMore */
        26, /* sendSMSExpectMoreResponse */
        ril_binder_radio_encode_gsm_sms_message,
        ril_binder_radio_decode_sms_send_result,
        "sendSMSExpectMore"
    },{
        RIL_REQUEST_SETUP_DATA_CALL,
        28, /* setupDataCall */
        27, /* setupDataCallResponse */
        ril_binder_radio_encode_setup_data_call,
        ril_binder_radio_decode_setup_data_call_result,
        "setupDataCall"
    },{
        RIL_REQUEST_SIM_IO,
        29, /* iccIOForApp */
        28, /* iccIOForAppResponse */
        ril_binder_radio_encode_icc_io,
        ril_binder_radio_decode_icc_result,
        "iccIOForApp"
    },{
        RIL_REQUEST_SEND_USSD,
        30, /* sendUssd */
        29, /* sendUssdResponse */
        ril_binder_radio_encode_string,
        NULL,
        "sendUssd"
    },{
        RIL_REQUEST_CANCEL_USSD,
        31, /* cancelPendingUssd */
        30, /* cancelPendingUssdResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "cancelPendingUssd"
    },{
        RIL_REQUEST_GET_CLIR,
        32, /* getClir */
        31, /* getClirResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_int_2,
        "getClir"
    },{
        RIL_REQUEST_SET_CLIR,
        33, /* setClir */
        32, /* setClirResponse */
        ril_binder_radio_encode_ints,
        NULL,
        "setClir"
    },{
        RIL_REQUEST_QUERY_CALL_FORWARD_STATUS,
        34, /* getCallForwardStatus */
        33, /* getCallForwardStatusResponse */
        ril_binder_radio_encode_call_forward_info,
        ril_binder_radio_decode_call_forward_info_array,
        "getCallForwardStatus"
    },{
        RIL_REQUEST_SET_CALL_FORWARD,
        35, /* setCallForward */
        34, /* setCallForwardResponse */
        ril_binder_radio_encode_call_forward_info,
        NULL,
        "setCallForward"
    },{
        RIL_REQUEST_QUERY_CALL_WAITING,
        36, /* getCallWaiting */
        35, /* getCallWaitingResponse */
        ril_binder_radio_encode_ints,
        ril_binder_radio_decode_call_waiting,
        "getCallWaiting"
    },{
        RIL_REQUEST_SET_CALL_WAITING,
        37, /* setCallWaiting */
        36, /* setCallWaitingResponse */
        ril_binder_radio_encode_ints_to_bool_int,
        NULL,
        "setCallWaiting"
    },{
        RIL_REQUEST_SMS_ACKNOWLEDGE,
        38, /* acknowledgeLastIncomingGsmSms */
        37, /* acknowledgeLastIncomingGsmSmsResponse */
        ril_binder_radio_encode_ints_to_bool_int,
        NULL,
        "acknowledgeLastIncomingGsmSms"
    },{
        RIL_REQUEST_ANSWER,
        39, /* acceptCall */
        38, /* acceptCallResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "acceptCall"
    },{
        RIL_REQUEST_DEACTIVATE_DATA_CALL,
        40, /* deactivateDataCall */
        39, /* deactivateDataCallResponse */
        ril_binder_radio_encode_deactivate_data_call,
        NULL,
        "deactivateDataCall"
    },{
        RIL_REQUEST_QUERY_FACILITY_LOCK,
        41, /* getFacilityLockForApp */
        40, /* getFacilityLockForAppResponse */
        ril_binder_radio_encode_get_facility_lock,
        ril_binder_radio_decode_int32,
        "getFacilityLockForApp"
    },{
        RIL_REQUEST_SET_FACILITY_LOCK,
        42, /* setFacilityLockForApp */
        41, /* setFacilityLockForAppResponse */
        ril_binder_radio_encode_set_facility_lock,
        ril_binder_radio_decode_int_1,
        "setFacilityLockForApp"
    },{
        RIL_REQUEST_CHANGE_BARRING_PASSWORD,
        43, /* setBarringPassword */
        42, /* setBarringPasswordResponse */
        ril_binder_radio_encode_strings,
        NULL,
        "setBarringPassword"
    },{
        RIL_REQUEST_QUERY_NETWORK_SELECTION_MODE,
        44, /* getNetworkSelectionMode */
        43, /* getNetworkSelectionModeResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_bool_to_int_array,
        "getNetworkSelectionMode"
    },{
        RIL_REQUEST_SET_NETWORK_SELECTION_AUTOMATIC,
        45, /* setNetworkSelectionModeAutomatic */
        44, /* setNetworkSelectionModeAutomaticResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "setNetworkSelectionModeAutomatic"
    },{
        RIL_REQUEST_SET_NETWORK_SELECTION_MANUAL,
        46, /* setNetworkSelectionModeManual */
        45, /* setNetworkSelectionModeManualResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "setNetworkSelectionModeManual"
    },{
        RIL_REQUEST_QUERY_AVAILABLE_NETWORKS,
        47, /* getAvailableNetworks */
        46, /* getAvailableNetworksResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_operator_info_list,
        "getAvailableNetworks"
    },{
        RIL_REQUEST_BASEBAND_VERSION,
        50, /* getBasebandVersion */
        49, /* getBasebandVersionResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_string,
        "getBasebandVersion"
    },{
        RIL_REQUEST_SEPARATE_CONNECTION,
        51, /* separateConnection */
        50, /* separateConnectionResponse */
        ril_binder_radio_encode_ints,
        NULL,
        "separateConnection"
    },{
        RIL_REQUEST_SET_MUTE,
        52, /* setMute */
        51, /* setMuteResponse */
        ril_binder_radio_encode_bool,
        NULL,
        "setMute"
    },{
        RIL_REQUEST_GET_MUTE,
        53, /* getMute */
        52, /* getMuteResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_bool_to_int_array,
        "getMute"
    },{
        RIL_REQUEST_QUERY_CLIP,
        54, /* getClip */
        53, /* getClipResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_int_1,
        "getClip"
    },{
        RIL_REQUEST_DATA_CALL_LIST,
        55, /* getDataCallList */
        54, /* getDataCallListResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_data_call_list,
        "getDataCallList"
    },{
        RIL_REQUEST_SET_SUPP_SVC_NOTIFICATION,
        56, /* setSuppServiceNotifications */
        55, /* setSuppServiceNotificationsResponse */
        ril_binder_radio_encode_int,
        NULL,
        "setSuppServiceNotifications"
    },{
        RIL_REQUEST_WRITE_SMS_TO_SIM,
        57, /* writeSmsToSim */
        56, /* writeSmsToSimResponse */
        ril_binder_radio_encode_sms_write_args,
        ril_binder_radio_decode_int_1,
        "writeSmsToSim"
    },{
        RIL_REQUEST_DELETE_SMS_ON_SIM,
        58, /* deleteSmsOnSim */
        57, /* deleteSmsOnSimResponse */
        ril_binder_radio_encode_ints,
        NULL,
        "deleteSmsOnSim"
    },{
        RIL_REQUEST_QUERY_AVAILABLE_BAND_MODE,
        60, /* getAvailableBandModes */
        59, /* getAvailableBandModesResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_int_array,
        "getAvailableBandModes"
    },{
        RIL_REQUEST_STK_SEND_ENVELOPE_COMMAND,
        61, /* sendEnvelope */
        60, /* sendEnvelopeResponse */
        ril_binder_radio_encode_string,
        ril_binder_radio_decode_string,
        "sendEnvelope"
    },{
        RIL_REQUEST_STK_SEND_TERMINAL_RESPONSE,
        62, /* sendTerminalResponseToSim */
        61, /* sendTerminalResponseToSimResponse */
        ril_binder_radio_encode_string,
        NULL,
        "sendTerminalResponseToSim"
    },{
        RIL_REQUEST_STK_HANDLE_CALL_SETUP_REQUESTED_FROM_SIM,
        63, /* handleStkCallSetupRequestFromSim */
        62, /* handleStkCallSetupRequestFromSimResponse */
        ril_binder_radio_encode_bool,
        NULL,
        "handleStkCallSetupRequestFromSim"
    },{
        RIL_REQUEST_EXPLICIT_CALL_TRANSFER,
        64, /* explicitCallTransfer */
        63, /* explicitCallTransferResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "explicitCallTransfer"
    },{
        RIL_REQUEST_SET_PREFERRED_NETWORK_TYPE,
        65, /* setPreferredNetworkType */
        64, /* setPreferredNetworkTypeResponse */
        ril_binder_radio_encode_ints,
        NULL,
        "setPreferredNetworkType"
    },{
        RIL_REQUEST_GET_PREFERRED_NETWORK_TYPE,
        66, /* getPreferredNetworkType */
        65, /* getPreferredNetworkTypeResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_pref_network_type,
        "getPreferredNetworkType"
    },{
        RIL_REQUEST_GSM_GET_BROADCAST_SMS_CONFIG,
        80, /* getGsmBroadcastConfig */
        79, /* getGsmBroadcastConfigResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_gsm_broadcast_sms_config,
        "getGsmBroadcastConfig"
    },{
        RIL_REQUEST_GSM_SET_BROADCAST_SMS_CONFIG,
        81, /* setGsmBroadcastConfig */
        80, /* setGsmBroadcastConfigResponse */
        ril_binder_radio_encode_gsm_broadcast_sms_config,
        NULL,
        "setGsmBroadcastConfig"
    },{
        RIL_REQUEST_DEVICE_IDENTITY,
        89, /* getDeviceIdentity */
        88, /* getDeviceIdentityResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_device_identity,
        "getDeviceIdentity"
    },{
        RIL_REQUEST_GET_SMSC_ADDRESS,
        91, /* getSmscAddress */
        90, /* getSmscAddressResponse */
        ril_binder_radio_encode_serial,
        ril_binder_radio_decode_string,
        "getSmscAddress"
    },{
        RIL_REQUEST_SET_SMSC_ADDRESS,
        92, /* setSmscAddress */
        91, /* setSmscAddressResponse */
        ril_binder_radio_encode_string,
        NULL,
        "setSmscAddress"
    },{
        RIL_REQUEST_REPORT_STK_SERVICE_IS_RUNNING,
        94, /* reportStkServiceIsRunning */
        93, /* reportStkServiceIsRunningResponse */
        ril_binder_radio_encode_serial,
        NULL,
        "reportStkServiceIsRunning"
    },{
        RIL_REQUEST_ALLOW_DATA,
        114, /* setDataAllowed */
        113, /* setDataAllowedResponse */
        ril_binder_radio_encode_bool,
        NULL,
        "setDataAllowed"
    },{
        RIL_RESPONSE_ACKNOWLEDGEMENT,
        RADIO_REQ_RESPONSE_ACKNOWLEDGEMENT,
        0,
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
        1,/* radioStateChanged */
        ril_binder_radio_decode_int32,
        "radioStateChanged"
    },{
        RIL_UNSOL_RESPONSE_CALL_STATE_CHANGED,
        2, /* callStateChanged */
        NULL,
        "callStateChanged"
    },{
        RIL_UNSOL_RESPONSE_VOICE_NETWORK_STATE_CHANGED,
        3, /* networkStateChanged */
        NULL,
        "networkStateChanged"
    },{
        RIL_UNSOL_RESPONSE_NEW_SMS,
        4, /* newSms */
        ril_binder_radio_decode_byte_array_to_hex,
        "newSms"
    },{
        RIL_UNSOL_ON_USSD,
        7, /* onUssd */
        ril_binder_radio_decode_ussd,
        "onUssd"
    },{
        RIL_UNSOL_NITZ_TIME_RECEIVED,
        8, /* nitzTimeReceived */
        ril_binder_radio_decode_string,
        "nitzTimeReceived"
    },{
        RIL_UNSOL_SIGNAL_STRENGTH,
        9, /* currentSignalStrength */
        ril_binder_radio_decode_signal_strength,
        "currentSignalStrength"
    },{
        RIL_UNSOL_DATA_CALL_LIST_CHANGED,
        10, /* dataCallListChanged */
        ril_binder_radio_decode_data_call_list,
        "dataCallListChanged"
    },{
        RIL_UNSOL_SUPP_SVC_NOTIFICATION,
        11, /* suppSvcNotify */
        ril_binder_radio_decode_supp_svc_notification,
        "suppSvcNotify"
    },{
        RIL_UNSOL_STK_SESSION_END,
        12, /* stkSessionEnd */
        NULL,
        "stkSessionEnd"
    },{
        RIL_UNSOL_STK_PROACTIVE_COMMAND,
        13, /* stkProactiveCommand */
        ril_binder_radio_decode_string,
        "stkProactiveCommand"
    },{
        RIL_UNSOL_STK_EVENT_NOTIFY,
        14, /* stkEventNotify */
        ril_binder_radio_decode_string,
        "stkEventNotify"
    },{
        RIL_UNSOL_SIM_REFRESH,
        17, /* simRefresh */
        ril_binder_radio_decode_sim_refresh,
        "simRefresh"
    },{
        RIL_UNSOL_CALL_RING,
        18, /* callRing */
        NULL, /* No parameters for GSM calls */
        "callRing"
    },{
        RIL_UNSOL_RESPONSE_SIM_STATUS_CHANGED,
        19, /* simStatusChanged */
        NULL,
        "simStatusChanged"
    },{
        RIL_UNSOL_RESPONSE_NEW_BROADCAST_SMS,
        21, /* newBroadcastSms */
        ril_binder_radio_decode_byte_array,
        "newBroadcastSms"
    },{
        RIL_UNSOL_RINGBACK_TONE,
        28, /* indicateRingbackTone */
        ril_binder_radio_decode_bool_to_int_array,
        "indicateRingbackTone"
    },{
        RIL_UNSOL_VOICE_RADIO_TECH_CHANGED,
        34, /* voiceRadioTechChanged */
        ril_binder_radio_decode_int32,
        "voiceRadioTechChanged"
    },{
        RIL_UNSOL_UICC_SUBSCRIPTION_STATUS_CHANGED,
        37, /* subscriptionStatusChanged */
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
    if (self->remote) {
        RilBinderRadioFailureData* failure =
            g_slice_new(RilBinderRadioFailureData);

        failure->transport = &self->parent;
        failure->serial = grilio_request_serial(req);
        gutil_idle_queue_add_full(self->idle,
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
    if (self->indication) {
        gbinder_local_object_drop(self->indication);
        self->indication = NULL;
    }
    if (self->response) {
        gbinder_local_object_drop(self->response);
        self->response = NULL;
    }
    if (self->remote) {
        gbinder_remote_object_remove_handler(self->remote, self->death_id);
        gbinder_remote_object_unref(self->remote);
        self->death_id = 0;
        self->remote = NULL;
    }
}

static
gboolean
ril_binder_radio_handle_indication(
    RilBinderRadio* self,
    const RilBinderRadioEvent* event,
    RadioIndicationType ind_type,
    GBinderReader* reader)
{
    GByteArray* buf = self->buf;
    gboolean signaled = FALSE;

    /* Protection against hypothetical recucrsion */
    if (buf) {
        self->buf = NULL;
    } else {
        buf = g_byte_array_new();
    }

    /* Decode the response */
    g_byte_array_set_size(buf, 0);
    if (!event->decode || event->decode(reader, buf)) {
        GRILIO_INDICATION_TYPE type = (ind_type == IND_ACK_EXP) ?
            GRILIO_INDICATION_UNSOLICITED_ACK_EXP :
            GRILIO_INDICATION_UNSOLICITED;

        grilio_transport_signal_indication(&self->parent, type, event->code,
            buf->data, buf->len);
        signaled = TRUE;
    } else {
        ofono_warn("Failed to decode %s indication", event->name);
    }

    g_byte_array_set_size(buf, 0);
    if (self->buf) {
        g_byte_array_unref(self->buf);
    }
    self->buf = buf;
    return signaled;
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
GBinderLocalReply*
ril_binder_radio_indication(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(user_data);
    const char* iface = gbinder_remote_request_interface(req);

    if (!g_strcmp0(iface, RADIO_INDICATION)) {
        GBinderReader reader;
        guint type;

        /* All these should be one-way */
        GASSERT(flags & GBINDER_TX_FLAG_ONEWAY);
        gbinder_remote_request_init_reader(req, &reader);
        if (gbinder_reader_read_uint32(&reader, &type) &&
            (type == IND_UNSOLICITED || type == IND_ACK_EXP)) {
            gboolean signaled = FALSE;
            const RilBinderRadioEvent* event =
                g_hash_table_lookup(self->unsol_map, GINT_TO_POINTER(code));

            /* CONNECTED indication is slightly special */
            if (code == RADIO_IND_RIL_CONNECTED) {
                DBG_(self, RADIO_INDICATION " %u rilConnected", code);
                if (event) {
                    signaled = ril_binder_radio_handle_indication(self,
                        event, type, &reader);
                }
                ril_binder_radio_connected(self);
            } else if (event) {
                DBG_(self, RADIO_INDICATION " %u %s", code, event->name);
                signaled = ril_binder_radio_handle_indication(self,
                    event, type, &reader);
            } else {
                DBG_(self, RADIO_INDICATION " %u", code);
            }
            if (type == IND_ACK_EXP && !signaled) {
                DBG_(self, "ack");
                ril_binder_radio_ack(self);
            }
        } else {
            DBG_(self, RADIO_INDICATION " %u", code);
            ofono_warn("Failed to decode indication %u", code);
        }
        *status = GBINDER_STATUS_OK;
    } else {
        DBG_(self, "%s %u", iface, code);
        *status = GBINDER_STATUS_FAILED;
    }
    return NULL;
}

static
void
ril_binder_radio_handle_ack(
    RilBinderRadio* self,
    GBinderRemoteRequest* req)
{
    gint32 serial;

    /* oneway acknowledgeRequest(int32_t serial) */
    if (gbinder_remote_request_read_int32(req, &serial)) {
        grilio_transport_signal_response(&self->parent,
            GRILIO_RESPONSE_SOLICITED_ACK, serial, RIL_E_SUCCESS, NULL, 0);
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
    GByteArray* buf = self->buf;
    gboolean signaled = FALSE;

    /* Protection against hypothetical recucrsion */
    if (buf) {
        self->buf = NULL;
    } else {
        buf = g_byte_array_new();
    }

    /* Decode the response */
    g_byte_array_set_size(buf, 0);
    if (!call->decode || call->decode(reader, buf)) {
        GRilIoTransport* transport = &self->parent;
        GRILIO_RESPONSE_TYPE type;

        switch (info->type) {
        default:
            DBG_(self, "Unexpected response type %u", info->type);
            type = GRILIO_RESPONSE_NONE;
            break;
        case RESP_SOLICITED:
            type = GRILIO_RESPONSE_SOLICITED;
            break;
        case RESP_SOLICITED_ACK:
            type = GRILIO_RESPONSE_SOLICITED_ACK;
            break;
        case RESP_SOLICITED_ACK_EXP:
            type = GRILIO_RESPONSE_SOLICITED_ACK_EXP;
            break;
        }

        if (type != GRILIO_RESPONSE_NONE) {
            grilio_transport_signal_response(transport, type, info->serial,
                info->error, buf->data, buf->len);
            signaled = TRUE;
        }
    } else {
        ofono_warn("Failed to decode %s response", call->name);
    }

    g_byte_array_set_size(buf, 0);
    if (self->buf) {
        g_byte_array_unref(self->buf);
    }
    self->buf = buf;
    return signaled;
}

static
void
ril_binder_radio_handle_response(
    RilBinderRadio* self,
    GBinderRemoteRequest* req,
    guint code)
{
    GBinderReader reader;
    GBinderBuffer* buf;

    /*
     * We assume that all responses must start with RadioResponseInfo.
     * One exception is acknowledgeRequest which is handled separately.
     */
    gbinder_remote_request_init_reader(req, &reader);
    buf = gbinder_reader_read_buffer(&reader);
    GASSERT(buf && buf->size == sizeof(RadioResponseInfo));
    if (buf && buf->size == sizeof(RadioResponseInfo)) {
        gboolean signaled = FALSE;
        const RadioResponseInfo* info = buf->data;
        const RilBinderRadioCall* call = g_hash_table_lookup(self->resp_map,
            GINT_TO_POINTER(code));

        if (call) {
            /* This is a known response */
            DBG_(self, RADIO_RESPONSE " %u %s", code, call->name);
            signaled = ril_binder_radio_handle_known_response(self, call, info,
                &reader);
        } else {
            DBG_(self, RADIO_RESPONSE " %u", code);
            ofono_warn("Unexpected response transaction %u", code);
        }
        if (info->type == RESP_SOLICITED_ACK_EXP && !signaled) {
            DBG_(self, "ack");
            ril_binder_radio_ack(self);
        }
    } else {
        DBG_(self, RADIO_RESPONSE " %u", code);
    }
    gbinder_buffer_free(buf);
}

static
GBinderLocalReply*
ril_binder_radio_response(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(user_data);
    const char* iface = gbinder_remote_request_interface(req);

    if (!g_strcmp0(iface, RADIO_RESPONSE)) {
        /* All these should be one-way transactions */
        GASSERT(flags & GBINDER_TX_FLAG_ONEWAY);
        if (code == RADIO_RESP_ACKNOWLEDGE_REQUEST) {
            /* acknowledgeRequest has no RadioResponseInfo */
            DBG_(self, RADIO_RESPONSE " %u acknowledgeRequest", code);
            ril_binder_radio_handle_ack(self, req);
        } else {
            /* All other responses have it */
            ril_binder_radio_handle_response(self, req, code);
        }
        *status = GBINDER_STATUS_OK;
    } else {
        DBG_(self, "%s %u", iface, code);
        *status = GBINDER_STATUS_FAILED;
    }
    return NULL;
}

static
void
ril_binder_radio_radio_died(
    GBinderRemoteObject* obj,
    void* user_data)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(user_data);
    GRilIoTransport* transport = &self->parent;

    ofono_error("%sradio died", transport->log_prefix);
    ril_binder_radio_drop_radio(self);
    grilio_transport_signal_disconnected(transport);
}

/*==========================================================================*
 * Methods
 *==========================================================================*/

static
GRILIO_SEND_STATUS
ril_binder_radio_send(
    GRilIoTransport* transport,
    GRilIoRequest* req, guint code)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(transport);
    const RilBinderRadioCall* call = g_hash_table_lookup(self->req_map,
        GINT_TO_POINTER(code));

    if (call) {
        /* This is a known request */
        GBinderLocalRequest* txreq = gbinder_client_new_request(self->client);

        if (!call->encode || call->encode(req, txreq)) {
            /* All requests are one-way */
            const int status = gbinder_client_transact_sync_oneway(self->client,
                call->req_tx, txreq);

            if (status >= 0) {
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
    const gboolean was_connected = (self->remote != NULL);

    ril_binder_radio_drop_radio(self);
    if (self->oemhook) {
        ril_binder_oemhook_free(self->oemhook);
        self->oemhook = NULL;
    }
    if (was_connected) {
        grilio_transport_signal_disconnected(transport);
    }
}

/*==========================================================================*
 * API
 *==========================================================================*/

void
ril_binder_radio_ack(
    RilBinderRadio* self)
{
    /* Internal ack */
    gbinder_client_transact_sync_oneway(self->client,
        RADIO_REQ_RESPONSE_ACKNOWLEDGEMENT, NULL);
}

GRilIoTransport*
ril_binder_radio_new(
    const char* dev,
    const char* name,
    const char* hook)
{
    RilBinderRadio* self = g_object_new(RIL_TYPE_BINDER_RADIO, NULL);
    GRilIoTransport* transport = &self->parent;

    self->sm = gbinder_servicemanager_new(dev);
    if (self->sm) {
        int status = 0;
        GBinderLocalRequest* req;
        GBinderRemoteReply* reply;

        /* Fetch remote reference from hwservicemanager */
        self->fqname = g_strconcat(RADIO_REMOTE "/", name, NULL);
        self->remote = gbinder_servicemanager_get_service_sync(self->sm,
            self->fqname, &status);
        if (self->remote) {
            DBG_(self, "Connected to %s", self->fqname);
            /* get_service returns auto-released reference,
             * we need to add a reference of our own */
            gbinder_remote_object_ref(self->remote);
            self->client = gbinder_client_new(self->remote, RADIO_REMOTE);
            self->death_id = gbinder_remote_object_add_death_handler
                (self->remote, ril_binder_radio_radio_died, self);
            self->indication = gbinder_servicemanager_new_local_object
                (self->sm, RADIO_INDICATION, ril_binder_radio_indication, self);
            self->response = gbinder_servicemanager_new_local_object
                (self->sm, RADIO_RESPONSE, ril_binder_radio_response, self);

            /* IRadio::setResponseFunctions */
            req = gbinder_client_new_request(self->client);
            gbinder_local_request_append_local_object(req, self->response);
            gbinder_local_request_append_local_object(req, self->indication);
            reply = gbinder_client_transact_sync_reply(self->client,
                RADIO_REQ_SET_RESPONSE_FUNCTIONS, req, &status);
            DBG_(self, "setResponseFunctions status %d", status);
            gbinder_local_request_unref(req);
            gbinder_remote_reply_unref(reply);

            /* Add the hook */
            if (!g_strcmp0(hook, "qcom")) {
                self->oemhook = ril_binder_oemhook_new_qcom(self->sm, self,
                    dev, name);
            }
            return transport;
        }
    }
    grilio_transport_unref(transport);
    return NULL;
}

/*==========================================================================*
 * Logging
 *==========================================================================*/

static
void
ril_binder_radio_log_notify(
    struct ofono_debug_desc* desc)
{
    gbinder_log.level = (desc->flags & OFONO_DEBUG_FLAG_PRINT) ?
        GLOG_LEVEL_VERBOSE : GLOG_LEVEL_INHERIT;
}

static struct ofono_debug_desc gbinder_debug OFONO_DEBUG_ATTR = {
    .name = "gbinder",
    .flags = OFONO_DEBUG_FLAG_DEFAULT,
    .notify = ril_binder_radio_log_notify
};

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
ril_binder_radio_init(
    RilBinderRadio* self)
{
    guint i;

    self->idle = gutil_idle_queue_new();
    self->req_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->resp_map = g_hash_table_new(g_direct_hash, g_direct_equal);
    self->unsol_map = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (i = 0; i < G_N_ELEMENTS(ril_binder_radio_calls); i++) {
        const RilBinderRadioCall* call = ril_binder_radio_calls + i;

        g_hash_table_insert(self->req_map, GINT_TO_POINTER(call->code),
            (gpointer)call);
        if (call->resp_tx) {
            g_hash_table_insert(self->resp_map, GINT_TO_POINTER(call->resp_tx),
            (gpointer)call);
        }
    }

    for (i = 0; i < G_N_ELEMENTS(ril_binder_radio_events); i++) {
        const RilBinderRadioEvent* event = ril_binder_radio_events + i;

        g_hash_table_insert(self->unsol_map, GINT_TO_POINTER(event->unsol_tx),
            (gpointer)event);
    }
}

static
void ril_binder_radio_finalize(
    GObject* object)
{
    RilBinderRadio* self = RIL_BINDER_RADIO(object);

    ril_binder_radio_drop_radio(self);
    ril_binder_oemhook_free(self->oemhook);
    gbinder_servicemanager_unref(self->sm);
    gbinder_client_unref(self->client);
    gutil_idle_queue_cancel_all(self->idle);
    gutil_idle_queue_unref(self->idle);
    g_hash_table_destroy(self->req_map);
    g_hash_table_destroy(self->resp_map);
    g_hash_table_destroy(self->unsol_map);
    if (self->buf) {
        g_byte_array_unref(self->buf);
    }
    g_free(self->fqname);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
ril_binder_radio_class_init(
    RilBinderRadioClass* klass)
{
    klass->ril_version_offset = 100;
    klass->send = ril_binder_radio_send;
    klass->shutdown = ril_binder_radio_shutdown;
    G_OBJECT_CLASS(klass)->finalize = ril_binder_radio_finalize;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
