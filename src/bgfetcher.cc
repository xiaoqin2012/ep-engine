/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include "ep.hh"

const double BgFetcher::sleepInterval = 1.0;

bool BgFetcherCallback::callback(Dispatcher &, TaskId &t) {
    return bgfetcher->run(t);
}

void BgFetcher::start() {
    LockHolder lh(taskMutex);
    dispatcher->schedule(shared_ptr<BgFetcherCallback>(new BgFetcherCallback(this)),
                         &task, Priority::BgFetcherPriority);
    assert(task.get());
}

void BgFetcher::stop() {
    LockHolder lh(taskMutex);
    assert(task.get());
    dispatcher->cancel(task);
}

void BgFetcher::doFetch(uint16_t vbId) {
    hrtime_t startTime(gethrtime());
    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                     "BgFetcher is fetching data, vBucket = %d numDocs = %d, "
                     "startTime = %lld\n",
                     vbId, items2fetch.size(), startTime/1000000);

    store->getROUnderlying()->getMulti(vbId, items2fetch);

    int totalfetches = 0;
    std::vector<VBucketBGFetchItem *> fetchedItems;
    vb_bgfetch_queue_t::iterator itr = items2fetch.begin();
    for (; itr != items2fetch.end(); itr++) {
        std::list<VBucketBGFetchItem *> &requestedItems = (*itr).second;
        std::list<VBucketBGFetchItem *>::iterator itm = requestedItems.begin();
        for(; itm != requestedItems.end(); itm++) {
            if ((*itm)->value.getStatus() != ENGINE_SUCCESS &&
                (*itm)->canRetry()) {
                // underlying kvstore failed to fetch requested data
                // don't return the failed request yet. Will requeue
                // it for retry later
                getLogger()->log(EXTENSION_LOG_WARNING, NULL,
                    "Warning: bgfetcher failed to fetch data for vb = %d "
                    "seq = %lld key = %s retry = %d\n", vbId, (*itr).first,
                     (*itm)->key.c_str(), (*itm)->getRetryCount());
                continue;
            }
            fetchedItems.push_back(*itm);
            ++totalfetches;
        }
    }

    if (totalfetches > 0) {
        store->completeBGFetchMulti(vbId, fetchedItems, startTime);
        stats.getMultiHisto.add((gethrtime()-startTime)/1000, totalfetches);
        total_num_fetched_items += totalfetches;
    }

    // failed requests will get requeued for retry within clearItems()
    clearItems(vbId);
}

void BgFetcher::clearItems(uint16_t vbId) {
    vb_bgfetch_queue_t::iterator itr = items2fetch.begin();
    size_t numRequeuedItems = 0;

    for(; itr != items2fetch.end(); itr++) {
        // every fetched item belonging to the same seq_id shares
        // a single data buffer, just delete it from the first fetched item
        std::list<VBucketBGFetchItem *> &doneItems = (*itr).second;
        VBucketBGFetchItem *firstItem = doneItems.front();
        firstItem->delValue();

        std::list<VBucketBGFetchItem *>::iterator dItr = doneItems.begin();
        for (; dItr != doneItems.end(); dItr++) {
            if ((*dItr)->value.getStatus() == ENGINE_SUCCESS ||
                !(*dItr)->canRetry()) {
                delete *dItr;
            } else {
                RCPtr<VBucket> vb = store->getVBuckets().getBucket(vbId);
                assert(vb);
                (*dItr)->incrRetryCount();
                getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                    "BgFetcher is re-queueing failed request for vb = %d "
                    "seq = %lld key = %s retry = %d\n",
                     vbId, (*itr).first, (*dItr)->key.c_str(),
                     (*dItr)->getRetryCount());
                ++numRequeuedItems;
                vb->queueBGFetchItem(*dItr, this, false);
            }
        }
    }

    total_num_requeued_items += numRequeuedItems;
}

bool BgFetcher::run(TaskId &tid) {
    assert(tid.get());
    size_t total_num_items2fetch = 0;

    total_num_fetched_items = 0;
    total_num_requeued_items = 0;

    if(stats.numRemainingBgJobs.get()) {
        const VBucketMap &vbMap = store->getVBuckets();
        size_t numVbuckets = vbMap.getSize();
        size_t num_items2fetch;
        for (size_t vbid = 0; vbid < numVbuckets; vbid++) {
            RCPtr<VBucket> vb = vbMap.getBucket(vbid);
            num_items2fetch = 0;
            if (vb && (num_items2fetch =
                       vb->getBGFetchItems(items2fetch))) {
                doFetch(vbid);
                total_num_items2fetch += num_items2fetch;
                items2fetch.clear();
            }
        }

        stats.numRemainingBgJobs.decr(total_num_fetched_items);

        /*getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                         "BgFetcher: total_num_items2fetch = %d "
                         "total_num_fetched_items = %d "
                         "totaL_num_requeued_items = %d\n",
                         total_num_items2fetch,
                         total_num_fetched_items,
                         total_num_requeued_items);*/
    }

    if(!stats.numRemainingBgJobs.get()) {
        // wait a bit until next fetch request arrives
        double sleep = std::max(store->getBGFetchDelay(), sleepInterval);
        dispatcher->snooze(tid, sleep);

        if (stats.numRemainingBgJobs.get()) {
            // check again numRemainingBgJobs, a new fetch request
            // could have arrvied right before calling above snooze()
            dispatcher->snooze(tid, 0);
        }
    }

    return true;
}

bool BgFetcher::pendingJob() {
    const VBucketMap &vbMap = store->getVBuckets();
    size_t numVbuckets = vbMap.getSize();
    for (size_t vbid = 0; vbid < numVbuckets; ++vbid) {
        RCPtr<VBucket> vb = vbMap.getBucket(vbid);
        if (vb && vb->hasPendingBGFetchItems()) {
            return true;
        }
    }
    return false;
}
