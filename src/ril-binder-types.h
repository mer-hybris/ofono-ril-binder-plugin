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

#ifndef RIL_BINDER_TYPES_H
#define RIL_BINDER_TYPES_H

#include <gutil_types.h>

#define ALIGNED(x) __attribute__ ((aligned(x)))

typedef struct ril_binder_radio RilBinderRadio;
typedef struct ril_binder_oemhook RilBinderOemHook;

typedef struct radio_response_info {
    guint32 type;
    guint32 serial;
    guint32 error;
} RadioResponseInfo;

typedef enum radio_response_type {
    RESP_SOLICITED,
    RESP_SOLICITED_ACK,
    RESP_SOLICITED_ACK_EXP
} RadioResponseType;

typedef enum radio_indication_type {
    IND_UNSOLICITED,
    IND_ACK_EXP
} RadioIndicationType;

typedef struct radio_string {
    union {
        guint64 value;
        const char* str;
    } data;
    guint32 len;
    guint8 owns_buffer;
} ALIGNED(4) RadioString;

typedef struct radio_vector {
    union {
        guint64 value;
        const void* ptr;
    } data;
    guint32 count;
    guint8 owns_buffer;
} ALIGNED(4) RadioVector;

#endif /* RIL_BINDER_TYPES_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
