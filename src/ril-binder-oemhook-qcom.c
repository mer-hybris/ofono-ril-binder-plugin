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

#include "ril-binder-oemhook.h"
#include "ril-binder-radio.h"

#include <ofono/log.h>

#include <gbinder.h>
#include <gutil_log.h>
#include <gutil_misc.h>

struct ril_binder_oemhook {
    char* name;
    char* fqname;
    RilBinderRadio* radio;
    GBinderClient* client;
    GBinderRemoteObject* remote;
    GBinderLocalObject* response;
    GBinderLocalObject* indication;
    gulong death_id;
};

#define OEMHOOK_IFACE(x)   "android.hardware.radio.deprecated@1.0::" x
#define OEMHOOK_REMOTE     OEMHOOK_IFACE("IOemHook")
#define OEMHOOK_RESPONSE   OEMHOOK_IFACE("IOemHookResponse")
#define OEMHOOK_INDICATION OEMHOOK_IFACE("IOemHookIndication")

#define DBG_(self,fmt,args...) DBG("%s " fmt, (self)->name, ##args)

/* android.hardware.radio.deprecated@1.0::IOemHook */
enum ril_binder_oemhook_req {
    /**
     * Set response functions for oem hook requests & oem hook indications.
     *
     * @param oemHookResponse Object containing response functions
     * @param oemHookIndication Object containing oem hook indications
     *
     * setResponseFunctions(IOemHookResponse oemHookResponse,
     *                      IOemHookIndication oemHookIndication);
     */
    OEMHOOK_REQ_SET_RESPONSE_FUNCTIONS = 1,

    /**
     * This request passes raw byte arrays between framework and vendor code.
     *
     * @param serial Serial number of request.
     * @param data data passed as raw bytes
     *
     * Response function is IOemHookResponse.sendRequestRawResponse()
     *
     * oneway sendRequestRaw(int32_t serial, vec<uint8_t> data);
     */
    OEMHOOK_REQ_SEND_REQUEST_RAW,

    /**
     * This request passes strings between framework and vendor code.
     *
     * @param serial Serial number of request.
     * @param data data passed as strings
     *
     * Response function is IOemHookResponse.sendRequestStringsResponse()
     *
     * oneway sendRequestStrings(int32_t serial, vec<string> data);
     */
    OEMHOOK_REQ_SEND_REQUEST_STRINGS
};

/* android.hardware.radio.deprecated@1.0::IOemHookIndication */
enum ril_binder_oemhook_ind {
   /**
    * This is for OEM specific use.
    *
    * @param type Type of radio indication
    * @param data data passed as raw bytes
    *
    * oneway oemHookRaw(RadioIndicationType type, vec<uint8_t> data);
    */
    OEMHOOK_IND_OEM_HOK_RAW = 1
};

static
void
ril_binder_oemhook_qcom_drop_objects(
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
}

static
void
ril_binder_oemhook_qcom_died(
    GBinderRemoteObject* obj,
    void* user_data)
{
    RilBinderOemHook* self = user_data;

    ofono_error("%s oemhook died", self->name);
    ril_binder_oemhook_qcom_drop_objects(self);
}

static
GBinderLocalReply*
ril_binder_oemhook_qcom_response(
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
        /* All these should be one-way transactions */
        GASSERT(flags & GBINDER_TX_FLAG_ONEWAY);
        DBG_(self, OEMHOOK_RESPONSE " %u", code);
        *status = GBINDER_STATUS_OK;
    } else {
        DBG_(self, "%s %u", iface, code);
        *status = GBINDER_STATUS_FAILED;
    }
    return NULL;
}

static
void
ril_binder_oemhook_qcom_handle_oem_hook_raw(
    RilBinderOemHook* self,
    GBinderReader* in)
{
    GBinderBuffer* buf = gbinder_reader_read_buffer(in);

    if (buf && buf->size == sizeof(GBinderHidlVec)) {
        const GBinderHidlVec* vec = buf->data;

        /* The contents comes in another buffer */
        gbinder_buffer_free(buf);
        buf = gbinder_reader_read_buffer(in);

        if (buf && buf->data == vec->data.ptr && buf->size == vec->count) {
            static struct ofono_debug_desc debug_desc OFONO_DEBUG_ATTR = {
                .file = __FILE__,
                .flags = OFONO_DEBUG_FLAG_DEFAULT,
            };
            if (debug_desc.flags & OFONO_DEBUG_FLAG_PRINT) {
                char hexbuf[GUTIL_HEXDUMP_BUFSIZE];
                const guint8* data = buf->data;
                char prefix = '>';
                guint off = 0;

                while (off < buf->size) {
                    off += gutil_hexdump(hexbuf, data + off, buf->size - off);
                    DBG_(self, "%c %s", prefix, hexbuf);
                    prefix = ' ';
                }
            }
        }
    }
    gbinder_buffer_free(buf);
}

static
GBinderLocalReply*
ril_binder_oemhook_qcom_indication(
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
            (type == IND_UNSOLICITED || type == IND_ACK_EXP)) {
            if (code == OEMHOOK_IND_OEM_HOK_RAW) {
                DBG_(self, OEMHOOK_INDICATION " %u oemHookRaw", code);
                ril_binder_oemhook_qcom_handle_oem_hook_raw(self, &reader);
            } else {
                DBG_(self, OEMHOOK_INDICATION " %u", code);
            }
            if (type == IND_ACK_EXP) {
                DBG_(self, "ack");
                ril_binder_radio_ack(self->radio);
            }
        } else {
            DBG_(self, OEMHOOK_INDICATION " %u", code);
            ofono_warn("Failed to decode indication %u", code);
        }
        *status = GBINDER_STATUS_OK;
    } else {
        DBG_(self, "%s %u", iface, code);
        *status = GBINDER_STATUS_FAILED;
    }
    return NULL;
}

RilBinderOemHook*
ril_binder_oemhook_new_qcom(
    GBinderServiceManager* sm,
    RilBinderRadio* radio,
    const char* dev,
    const char* name)
{
    int status = 0;
    GBinderLocalRequest* req;
    GBinderRemoteReply* reply;
    RilBinderOemHook* self = g_new0(RilBinderOemHook, 1);

    /* Fetch remote reference from hwservicemanager */
    self->name = g_strdup(name);
    self->fqname = g_strconcat(OEMHOOK_REMOTE "/", name, NULL);
    self->remote = gbinder_servicemanager_get_service_sync(sm, self->fqname,
        &status);
    if (self->remote) {
        DBG_(self, "Connected to %s", self->fqname);
        /* get_service returns auto-released reference,
         * we need to add a reference of our own */
        gbinder_remote_object_ref(self->remote);
        self->radio = radio;
        self->client = gbinder_client_new(self->remote, OEMHOOK_REMOTE);
        self->death_id = gbinder_remote_object_add_death_handler(self->remote,
            ril_binder_oemhook_qcom_died, self);
        self->indication = gbinder_servicemanager_new_local_object(sm,
            OEMHOOK_INDICATION, ril_binder_oemhook_qcom_indication, self);
        self->response = gbinder_servicemanager_new_local_object(sm,
            OEMHOOK_RESPONSE, ril_binder_oemhook_qcom_response, self);

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
    ril_binder_oemhook_free(self);
    return NULL;
}

void
ril_binder_oemhook_free(
    RilBinderOemHook* self)
{
    if (self) {
        ril_binder_oemhook_qcom_drop_objects(self);
        gbinder_client_unref(self->client);
        g_free(self->fqname);
        g_free(self->name);
        g_free(self);
    }
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
