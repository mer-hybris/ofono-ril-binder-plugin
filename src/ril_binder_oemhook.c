/*
 * Copyright (C) 2020 Jolla Ltd.
 * Copyright (C) 2020 Slava Monich <slava.monich@jolla.com>
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

#include "ril_binder_oemhook.h"
#include "ril_binder_log.h"

#include <radio_instance.h>
#include <grilio_request.h>

#include <gbinder.h>
#include <gutil_log.h>
#include <gutil_misc.h>

typedef GObjectClass RilBinderOemHookClass;
struct ril_binder_oemhook {
    GObject parent;
    const char* name;
    RadioInstance* radio;
    GBinderClient* client;
    GBinderRemoteObject* remote;
    GBinderLocalObject* response;
    GBinderLocalObject* indication;
    gulong death_id;
};

G_DEFINE_TYPE(RilBinderOemHook, ril_binder_oemhook, G_TYPE_OBJECT)
#define RIL_BINDER_TYPE_OEMHOOK (ril_binder_oemhook_get_type())
#define RIL_BINDER_OEMHOOK(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), \
        RIL_BINDER_TYPE_OEMHOOK, RilBinderOemHook))

enum ril_binder_oemhook_signal {
    SIGNAL_RESP_SEND_REQUEST_RAW,
    SIGNAL_COUNT
};

#define SIGNAL_RESP_SEND_REQUEST_RAW_NAME "oemhook-resp-send-request-raw"

static guint ril_binder_oemhook_signals[SIGNAL_COUNT] = { 0 };

/* Logging */
#define OEMHOOK_LOG ril_binder_oemhook_log
static const GLOG_MODULE_DEFINE2_(OEMHOOK_LOG, "oemhook", GLOG_MODULE_NAME);
#undef GLOG_MODULE_NAME
#define GLOG_MODULE_NAME OEMHOOK_LOG
#define DBG_(self,fmt,args...) GDEBUG("%s " fmt, (self)->name, ##args)

/* android.hardware.radio.deprecated@1.0::IOemHook */
enum ril_binder_oemhook_req {
    /* setResponseFunctions(IOemHookResponse, IOemHookIndication); */
    OEMHOOK_REQ_SET_RESPONSE_FUNCTIONS = GBINDER_FIRST_CALL_TRANSACTION,
    /* oneway sendRequestRaw(int32_t serial, vec<uint8_t> data); */
    OEMHOOK_REQ_SEND_REQUEST_RAW,
    /* oneway sendRequestStrings(int32_t serial, vec<string> data); */
    OEMHOOK_REQ_SEND_REQUEST_STRINGS
};

/* android.hardware.radio.deprecated@1.0::IOemHookResponse */
enum ril_binder_oemhook_resp {
    /* oneway sendRequestRawResponse(RadioResponseInfo, vec<uint8_t>); */
    OEMHOOK_RESP_SEND_REQUEST_RAW = GBINDER_FIRST_CALL_TRANSACTION,
    /* oneway sendRequestStringsResponse(RadioResponseInfo, vec<string>); */
    OEMHOOK_RESP_SEND_REQUEST_STRINGS
};

/* android.hardware.radio.deprecated@1.0::IOemHookIndication */
enum ril_binder_oemhook_ind {
    /* oneway oemHookRaw(RadioIndicationType, vec<uint8_t> data); */
    OEMHOOK_IND_OEM_HOOK_RAW = GBINDER_FIRST_CALL_TRANSACTION
};

#define OEMHOOK_IFACE(x)   "android.hardware.radio.deprecated@1.0::" x
#define OEMHOOK_REMOTE     OEMHOOK_IFACE("IOemHook")
#define OEMHOOK_RESPONSE   OEMHOOK_IFACE("IOemHookResponse")
#define OEMHOOK_INDICATION OEMHOOK_IFACE("IOemHookIndication")

static
void
ril_binder_oemhook_drop_objects(
    RilBinderOemHook* self)
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
    if (self->radio) {
        radio_instance_unref(self->radio);
        self->radio = NULL;
    }
    if (self->client) {
        gbinder_client_unref(self->client);
        self->client = NULL;
    }
}

static
void
ril_binder_oemhook_died(
    GBinderRemoteObject* obj,
    void* user_data)
{
    RilBinderOemHook* self = user_data;

    GERR("%s oemhook died", self->name);
    ril_binder_oemhook_drop_objects(self);
}

/* oneway sendRequestRawResponse(RadioResponseInfo, vec<uint8_t>); */
static
void
ril_binder_oemhook_handle_send_request_raw_response(
    RilBinderOemHook* self,
    const RadioResponseInfo* info,
    GBinderReader* in)
{
    GUtilData data;

    data.bytes = gbinder_reader_read_hidl_byte_vec(in, &data.size);
    GASSERT(data.bytes);
    if (data.bytes) {
        g_signal_emit(self, ril_binder_oemhook_signals
            [SIGNAL_RESP_SEND_REQUEST_RAW], 0, info, &data);
    }
}

static
GBinderLocalReply*
ril_binder_oemhook_response(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    RilBinderOemHook* self = user_data;
    const char* iface = gbinder_remote_request_interface(req);

    if (!g_strcmp0(iface, OEMHOOK_RESPONSE)) {
        GBinderReader reader;
        const RadioResponseInfo* info;

        /* All these should be one-way transactions */
        GASSERT(flags & GBINDER_TX_FLAG_ONEWAY);

        /* And have RadioResponseInfo as the first parameter */
        gbinder_remote_request_init_reader(req, &reader);
        info = gbinder_reader_read_hidl_struct(&reader, RadioResponseInfo);
        GASSERT(info);
        if (info) {
            switch (code) {
            case OEMHOOK_RESP_SEND_REQUEST_RAW:
                DBG_(self, OEMHOOK_RESPONSE " %u sendRequestRawResponse", code);
                ril_binder_oemhook_handle_send_request_raw_response(self,
                    info, &reader);
                break;
            case OEMHOOK_RESP_SEND_REQUEST_STRINGS:
                /*
                 * No need to handle sendRequestStringsResponse() just yet
                 * because we never call sendRequestStrings()
                 *
                 * Fall though.
                 */
            default:
                DBG_(self, OEMHOOK_RESPONSE " %u", code);
                break;
            }
        }
        *status = GBINDER_STATUS_OK;
    } else {
        DBG_(self, "%s %u (unexpected interface)", iface, code);
        *status = GBINDER_STATUS_FAILED;
    }
    return NULL;
}

/* oneway oemHookRaw(RadioIndicationType, vec<uint8_t> data); */
static
void
ril_binder_oemhook_handle_oem_hook_raw(
    RilBinderOemHook* self,
    GBinderReader* in)
{
    GUtilData data;

    data.bytes = gbinder_reader_read_hidl_byte_vec(in, &data.size);
    GASSERT(data.bytes);
    if (data.bytes) {
        if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
            char hex[GUTIL_HEXDUMP_BUFSIZE];
            char prefix = '>';
            guint off = 0;

            while (off < data.size) {
                const guint consumed = gutil_hexdump(hex,
                    data.bytes + off, data.size - off);

                GDEBUG("%s%c %04x: %s", self->name, prefix, off, hex);
                prefix = ' ';
                off += consumed;
            }
        }
        /* Can emit a signal here if needed */
    }
}

static
GBinderLocalReply*
ril_binder_oemhook_indication(
    GBinderLocalObject* obj,
    GBinderRemoteRequest* req,
    guint code,
    guint flags,
    int* status,
    void* user_data)
{
    RilBinderOemHook* self = user_data;
    const char* iface = gbinder_remote_request_interface(req);

    if (!g_strcmp0(iface, OEMHOOK_INDICATION)) {
        GBinderReader reader;
        guint32 type;

        /* All these should be one-way */
        GASSERT(flags & GBINDER_TX_FLAG_ONEWAY);

        gbinder_remote_request_init_reader(req, &reader);
        if (gbinder_reader_read_uint32(&reader, &type) &&
            (type == RADIO_IND_UNSOLICITED || type == RADIO_IND_ACK_EXP)) {
            if (code == OEMHOOK_IND_OEM_HOOK_RAW) {
                DBG_(self, OEMHOOK_INDICATION " %u oemHookRaw", code);
                ril_binder_oemhook_handle_oem_hook_raw(self, &reader);
            } else {
                DBG_(self, OEMHOOK_INDICATION " %u", code);
            }
            if (type == RADIO_IND_ACK_EXP) {
                GVERBOSE("%s ack", self->name);
                radio_instance_ack(self->radio);
            }
        } else {
            DBG_(self, OEMHOOK_INDICATION " %u", code);
            GWARN("Failed to decode indication %u", code);
        }
        *status = GBINDER_STATUS_OK;
    } else {
        DBG_(self, "%s %u", iface, code);
        *status = GBINDER_STATUS_FAILED;
    }
    return NULL;
}

/*==========================================================================*
 * API
 *==========================================================================*/

gboolean
ril_binder_oemhook_send_request_raw(
    RilBinderOemHook* self,
    GRilIoRequest* in)
{
    GBinderLocalRequest* req = gbinder_client_new_request(self->client);
    GBinderWriter writer;
    gulong id;

    gbinder_local_request_init_writer(req, &writer);
    gbinder_writer_append_int32(&writer, grilio_request_serial(in));
    gbinder_writer_append_hidl_vec(&writer, grilio_request_data(in),
        grilio_request_size(in), 1);
    id = gbinder_client_transact(self->client, OEMHOOK_REQ_SEND_REQUEST_RAW,
        GBINDER_TX_FLAG_ONEWAY, req, NULL, NULL, NULL);

    gbinder_local_request_unref(req);
    return (id != 0);
}

RilBinderOemHook*
ril_binder_oemhook_new(
    GBinderServiceManager* sm,
    RadioInstance* radio)
{
    if (radio) {
        int status = 0;
        RilBinderOemHook* self = g_object_new(RIL_BINDER_TYPE_OEMHOOK, NULL);
        char* fqname = g_strconcat(OEMHOOK_REMOTE "/", radio->slot, NULL);

        /* Fetch remote reference from hwservicemanager */
        self->name = radio->slot;
        self->remote = gbinder_servicemanager_get_service_sync(sm, fqname,
            &status);
        if (self->remote) {
            DBG_(self, "Connected to %s", fqname);
        }
        g_free(fqname);
        if (self->remote) {
            GBinderLocalRequest* req;
            GBinderRemoteReply* reply;

            /* get_service returns auto-released reference,
             * we need to add a reference of our own */
            gbinder_remote_object_ref(self->remote);
            self->radio = radio_instance_ref(radio);
            self->client = gbinder_client_new(self->remote, OEMHOOK_REMOTE);
            self->death_id = gbinder_remote_object_add_death_handler
                (self->remote, ril_binder_oemhook_died, self);
            self->indication = gbinder_servicemanager_new_local_object(sm,
                OEMHOOK_INDICATION, ril_binder_oemhook_indication, self);
            self->response = gbinder_servicemanager_new_local_object(sm,
                OEMHOOK_RESPONSE, ril_binder_oemhook_response, self);

            /* IOemHook::setResponseFunctions */
            req = gbinder_client_new_request(self->client);
            gbinder_local_request_append_local_object(req, self->response);
            gbinder_local_request_append_local_object(req, self->indication);
            reply = gbinder_client_transact_sync_reply(self->client,
                OEMHOOK_REQ_SET_RESPONSE_FUNCTIONS, req, &status);
            DBG_(self, "setResponseFunctions status %d", status);
            gbinder_local_request_unref(req);
            gbinder_remote_reply_unref(reply);

            return self;
        }
        g_object_unref(self);
    }
    return NULL;
}

void
ril_binder_oemhook_free(
    RilBinderOemHook* self)
{
    if (G_LIKELY(self)) {
        g_object_unref(RIL_BINDER_OEMHOOK(self));
    }
}

gulong
ril_binder_oemhook_add_raw_response_handler(
    RilBinderOemHook* self,
    RilBinderOemHookRawResponseFunc func,
    gpointer user_data)
{
    return (G_LIKELY(self) && G_LIKELY(func)) ? g_signal_connect(self,
        SIGNAL_RESP_SEND_REQUEST_RAW_NAME, G_CALLBACK(func), user_data) : 0;
}

void
ril_binder_oemhook_remove_handler(
    RilBinderOemHook* self,
    gulong id)
{
    if (G_LIKELY(self) && G_LIKELY(id)) {
        g_signal_handler_disconnect(self, id);
    }
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
ril_binder_oemhook_init(
    RilBinderOemHook* self)
{
}

static
void
ril_binder_oemhook_finalize(
    GObject* object)
{
    RilBinderOemHook* self = RIL_BINDER_OEMHOOK(object);

    ril_binder_oemhook_drop_objects(self);
    G_OBJECT_CLASS(ril_binder_oemhook_parent_class)->finalize(object);
}

static
void
ril_binder_oemhook_class_init(
    RilBinderOemHookClass* klass)
{
    GType type = G_OBJECT_CLASS_TYPE(klass);

    ril_binder_oemhook_signals[SIGNAL_RESP_SEND_REQUEST_RAW] = 
        g_signal_new(SIGNAL_RESP_SEND_REQUEST_RAW_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE,
            2, G_TYPE_POINTER, G_TYPE_POINTER);
    G_OBJECT_CLASS(klass)->finalize = ril_binder_oemhook_finalize;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
