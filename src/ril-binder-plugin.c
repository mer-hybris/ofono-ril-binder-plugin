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

#define OFONO_API_SUBJECT_TO_CHANGE
#include <ofono/plugin.h>
#include <ofono/ril-transport.h>
#include <ofono/log.h>

#include "ril-binder-radio.h"

#define RIL_BINDER_DEFAULT_DEV  "/dev/hwbinder"
#define RIL_BINDER_DEFAULT_NAME "slot1"

static
struct grilio_transport*
ril_binder_transport_connect(
    GHashTable *args)
{
    const char* dev = g_hash_table_lookup(args, "dev");
    const char* name = g_hash_table_lookup(args, "name");
    const char* hook = g_hash_table_lookup(args, "oemhook");

    if (!dev) dev = RIL_BINDER_DEFAULT_DEV;
    if (!name) name = RIL_BINDER_DEFAULT_NAME;
    DBG("%s %s %s", dev, name, hook ? hook : "");
    return ril_binder_radio_new(dev, name, hook);
}

static const struct ofono_ril_transport ril_binder_transport = {
    .name = "binder",
    .api_version = OFONO_RIL_TRANSPORT_API_VERSION,
    .connect = ril_binder_transport_connect
};

static
int
ril_binder_plugin_init()
{
    ofono_info("Initializing RIL binder transport plugin.");
    ofono_ril_transport_register(&ril_binder_transport);
    return 0;
}

static
void
ril_binder_plugin_exit()
{
    DBG("");
    ofono_ril_transport_unregister(&ril_binder_transport);
}

OFONO_PLUGIN_DEFINE(ril_binder, "RIL binder transport plugin",
    OFONO_VERSION, OFONO_PLUGIN_PRIORITY_DEFAULT,
    ril_binder_plugin_init, ril_binder_plugin_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
