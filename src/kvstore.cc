/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2010 Couchbase, Inc
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "config.h"

#include <map>
#include <string>

#include "blackhole-kvstore/blackhole.h"
#include "common.h"
#ifdef HAVE_LIBCOUCHSTORE
#include "couch-kvstore/couch-kvstore.h"
#else
#include "couch-kvstore/couch-kvstore-dummy.h"
#endif
#include "ep_engine.h"
#include "kvstore.h"
#include "stats.h"
#include "warmup.h"


KVStore *KVStoreFactory::create(EventuallyPersistentEngine &theEngine,
                                bool read_only) {
    Configuration &c = theEngine.getConfiguration();

    KVStore *ret = NULL;
    std::string backend = c.getBackend();
    if (backend.compare("couchdb") == 0) {
        ret = new CouchKVStore(theEngine, read_only);
    } else if (backend.compare("blackhole") == 0) {
        ret = new BlackholeKVStore(read_only);
    } else {
        LOG(EXTENSION_LOG_WARNING, "Unknown backend: [%s]", backend.c_str());
    }

    if (ret != NULL) {
        ret->setEngine(&theEngine);
    }

    return ret;
}

struct WarmupCookie {
    WarmupCookie(KVStore *s, Callback<GetValue>&c) :
        store(s), cb(c), engine(s->getEngine()), loaded(0), skipped(0), error(0)
    { /* EMPTY */ }
    KVStore *store;
    Callback<GetValue> &cb;
    EventuallyPersistentEngine *engine;
    size_t loaded;
    size_t skipped;
    size_t error;
};

static void warmupCallback(void *arg, uint16_t vb,
                           const std::string &key, uint64_t rowid)
{
    WarmupCookie *cookie = static_cast<WarmupCookie*>(arg);

    if (cookie->engine->stillWarmingUp()) {
        RememberingCallback<GetValue> cb;
        cookie->store->get(key, rowid, vb, cb);
        cb.waitForValue();

        if (cb.val.getStatus() == ENGINE_SUCCESS) {
            cookie->cb.callback(cb.val);
            cookie->loaded++;
        } else {
            LOG(EXTENSION_LOG_WARNING, "Warning: warmup failed to load data for "
                "vBucket = %d key = %s error = %X", vb, key.c_str(),
                cb.val.getStatus());
            cookie->error++;
        }
    } else {
        cookie->skipped++;
    }
}

size_t KVStore::warmup(MutationLog &lf,
                       const std::map<uint16_t, vbucket_state> &vbmap,
                       Callback<GetValue> &cb,
                       Callback<size_t> &estimate)
{
    MutationLogHarvester harvester(lf);
    std::map<uint16_t, vbucket_state>::const_iterator it;
    for (it = vbmap.begin(); it != vbmap.end(); ++it) {
        harvester.setVBucket(it->first);
    }

    hrtime_t start = gethrtime();
    if (!harvester.load()) {
        return -1;
    }
    hrtime_t end = gethrtime();

    size_t total = harvester.total();
    estimate.callback(total);
    LOG(EXTENSION_LOG_DEBUG, "Completed log read in %s with %ld entries",
        hrtime2text(end - start).c_str(), total);

    WarmupCookie cookie(this, cb);
    start = gethrtime();
    harvester.apply(&cookie, &warmupCallback);
    end = gethrtime();

    LOG(EXTENSION_LOG_DEBUG,"Populated log in %s with (l: %ld, s: %ld, e: %ld)",
        hrtime2text(end - start).c_str(), cookie.loaded, cookie.skipped,
        cookie.error);

    return cookie.loaded;
}

bool KVStore::getEstimatedItemCount(size_t &) {
    // Not supported
    return false;
}
