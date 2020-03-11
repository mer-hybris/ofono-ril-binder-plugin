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

#ifndef RIL_BINDER_OEMHOOK_H
#define RIL_BINDER_OEMHOOK_H

#include <radio_types.h>
#include <gbinder_types.h>
#include <grilio_types.h>

typedef struct ril_binder_oemhook RilBinderOemHook;

typedef
void
(*RilBinderOemHookRawResponseFunc)(
    RilBinderOemHook* hook,
    const RadioResponseInfo* info,
    const GUtilData* data,
    gpointer user_data);

typedef
gboolean
(*RilBinderOemHookRawFunc)(
    RilBinderOemHook* hook,
    const GUtilData* data,
    gpointer user_data);

RilBinderOemHook*
ril_binder_oemhook_new(
    GBinderServiceManager* sm,
    RadioInstance* radio)
    G_GNUC_INTERNAL;

void
ril_binder_oemhook_free(
    RilBinderOemHook* hook)
    G_GNUC_INTERNAL;

gboolean
ril_binder_oemhook_send_request_raw(
    RilBinderOemHook* hook,
    GRilIoRequest* req)
    G_GNUC_INTERNAL;

gulong
ril_binder_oemhook_add_raw_response_handler(
    RilBinderOemHook* hook,
    RilBinderOemHookRawResponseFunc func,
    gpointer user_data)
    G_GNUC_INTERNAL;

void
ril_binder_oemhook_remove_handler(
    RilBinderOemHook* hook,
    gulong id)
    G_GNUC_INTERNAL;

#endif /* RIL_BINDER_OEMHOOK_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
