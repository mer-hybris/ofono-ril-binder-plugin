/*
 * Copyright (C) 2019 Jolla Ltd.
 * Copyright (C) 2019 Slava Monich <slava.monich@jolla.com>
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

#ifndef RIL_BINDER_RADIO_IMPL_H
#define RIL_BINDER_RADIO_IMPL_H

#include <gbinder_types.h>

#include <radio_types.h>

#include <grilio_transport_impl.h>

typedef struct ril_binder_radio_priv RilBinderRadioPriv;

typedef struct ril_binder_radio {
    GRilIoTransport parent;
    RilBinderRadioPriv* priv;
    RadioInstance* radio;
    const char* modem;
} RilBinderRadio;

typedef struct ril_binder_radio_class {
    GRilIoTransportClass parent;
    gboolean (*handle_response)(RilBinderRadio* radio, RADIO_RESP code,
        const RadioResponseInfo* info, const GBinderReader* args);
    gboolean (*handle_indication)(RilBinderRadio* obj, RADIO_IND code,
        RADIO_IND_TYPE type, const GBinderReader* args);

    /* Padding for future expansion */
    void (*reserved1)(void);
    void (*reserved2)(void);
    void (*reserved3)(void);
    void (*reserved4)(void);
    void (*reserved5)(void);
    void (*reserved6)(void);
    void (*reserved7)(void);
    void (*reserved8)(void);
    void (*reserved9)(void);
    void (*reserved10)(void);
} RilBinderRadioClass;

GType ril_binder_radio_get_type(void);
#define RIL_TYPE_BINDER_RADIO (ril_binder_radio_get_type())
#define RIL_BINDER_RADIO(obj) G_TYPE_CHECK_INSTANCE_CAST((obj), \
        RIL_TYPE_BINDER_RADIO, RilBinderRadio)
#define RIL_BINDER_RADIO_GET_CLASS(obj) \
        G_TYPE_INSTANCE_GET_CLASS((obj), RIL_TYPE_BINDER_RADIO, \
        RilBinderRadioClass)
#define RIL_BINDER_RADIO_CLASS(klass) \
        G_TYPE_CHECK_CLASS_CAST((klass), RIL_TYPE_BINDER_RADIO, \
        RilBinderRadioClass)

typedef
gboolean
(*RilBinderRadioDecodeFunc)(
    GBinderReader* in,
    GByteArray* out);

gboolean
ril_binder_radio_init_base(
    RilBinderRadio* self,
    GHashTable* args);

const char*
ril_binder_radio_arg_modem(
    GHashTable* args);

const char*
ril_binder_radio_arg_dev(
    GHashTable* args);

const char*
ril_binder_radio_arg_name(
    GHashTable* args);

gboolean
ril_binder_radio_decode_response(
    RilBinderRadio* radio,
    const RadioResponseInfo* info,
    RilBinderRadioDecodeFunc decode,
    GBinderReader* reader);

gboolean
ril_binder_radio_decode_indication(
    RilBinderRadio* radio,
    RADIO_IND_TYPE ind_type,
    guint ril_code,
    RilBinderRadioDecodeFunc decode,
    GBinderReader* reader);

void
ril_binder_radio_decode_data_call(
    GByteArray* out,
    const RadioDataCall* call);

#endif /* RIL_BINDER_RADIO_IMPL_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
