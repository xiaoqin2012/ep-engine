/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#ifndef STORED_VALUE_H
#define STORED_VALUE_H 1

#include <climits>
#include <cstring>
#include <algorithm>

#include "common.hh"
#include "item.hh"
#include "locks.hh"

extern "C" {
    extern rel_time_t (*ep_current_time)();
}

// Forward declaration for StoredValue
class HashTable;
class StoredValueFactory;

// One of the following structs overlays at the end of StoredItem.
// This is figured out dynamically and stored in one bit in
// StoredValue so it can figure it out at runtime.

struct small_data {
    uint8_t      keylen;
    char         keybytes[1];
};

struct feature_data {
    uint64_t     cas;
    uint32_t     flags;
    rel_time_t   exptime;
    rel_time_t   lock_expiry;
    bool         locked;
    uint8_t      keylen;
    char         keybytes[1];
};

union stored_value_bodies {
    struct small_data   small;
    struct feature_data feature;
};

class StoredValue {
public:

    void operator delete(void* p) {
        ::operator delete(p);
     }

    ~StoredValue() {
        reduceCurrentSize(size());
    }

    void markDirty() {
        reDirty(ep_current_time());
    }

    void reDirty(rel_time_t dataAge) {
        dirtiness = dataAge >> 2;
        _isDirty = 1;
    }

    // returns time this object was dirtied.
    void markClean(rel_time_t *dataAge) {
        if (dataAge) {
            *dataAge = dirtiness << 2;
        }
        _isDirty = 0;
    }

    bool isDirty() const {
        return _isDirty;
    }

    bool isClean() const {
        return !isDirty();
    }

    const char* getKeyBytes() const {
        if (_isSmall) {
            return extra.small.keybytes;
        } else {
            return extra.feature.keybytes;
        }
    }

    uint8_t getKeyLen() const {
        if (_isSmall) {
            return extra.small.keylen;
        } else {
            return extra.feature.keylen;
        }
    }

    bool hasKey(const std::string &k) const {
        return k.length() == getKeyLen()
            && (std::memcmp(k.data(), getKeyBytes(), getKeyLen()) == 0);
    }

    const std::string getKey() const {
        return std::string(getKeyBytes(), getKeyLen());
    }

    value_t getValue() const {
        return value;
    }

    rel_time_t getExptime() const {
        if (_isSmall) {
            return 0;
        } else {
            return extra.feature.exptime;
        }
    }

    uint32_t getFlags() const {
        if (_isSmall) {
            return 0;
        } else {
            return extra.feature.flags;
        }
    }

    void setValue(value_t v,
                  uint32_t newFlags, rel_time_t newExp, uint64_t theCas) {
        reduceCurrentSize(size());
        value = v;
        if (!_isSmall) {
            extra.feature.cas = theCas;
            extra.feature.flags = newFlags;
            extra.feature.exptime = newExp;
        }
        markDirty();
        increaseCurrentSize(size());
    }

    uint64_t getCas() const {
        if (_isSmall) {
            return 0;
        } else {
            return extra.feature.cas;
        }
    }

    rel_time_t getDataAge() const {
        return dirtiness << 2;
    }

    void setCas(uint64_t c) {
        if (!_isSmall) {
            extra.feature.cas = c;
        }
    }

    void lock(rel_time_t expiry) {
        if (!_isSmall) {
            extra.feature.locked = true;
            extra.feature.lock_expiry = expiry;
        }
    }

    void unlock() {
        if (!_isSmall) {
            extra.feature.locked = false;
            extra.feature.lock_expiry = 0;
        }
    }

    bool hasId() {
        return id > 0;
    }

    int64_t getId() {
        return id;
    }

    void setId(int64_t to) {
        assert(!hasId());
        id = to;
    }

    size_t size() {
        return sizeOf(_isSmall) + getKeyLen() + value->length();
    }

    bool isLocked(rel_time_t curtime) {
        if (_isSmall) {
            return false;
        } else {
            if (extra.feature.locked && (curtime > extra.feature.lock_expiry)) {
                extra.feature.locked = false;
                return false;
            }
            return extra.feature.locked;
        }
    }

    /**
     * Get the size of a StoredValue object.
     *
     * This method exists because the size of a StoredValue as used
     * cannot be determined entirely at compile time due to the two
     * different extras sections that are used.
     *
     * @param small if true, we want the small variety, otherwise featured
     *
     * @return the size in bytes required (minus key) for a StoredValue.
     */
    static size_t sizeOf(bool small) {
        // Subtract one because the length of the string is computed on demand.
        size_t base = sizeof(StoredValue) - sizeof(union stored_value_bodies) - 1;
        return base + (small ? sizeof(struct small_data) : sizeof(struct feature_data));
    }

    static void setMaxDataSize(size_t);
    static size_t getMaxDataSize();
    static size_t getCurrentSize();

private:

    StoredValue(const Item &itm, StoredValue *n, bool setDirty = true,
                bool small = false) :
        value(itm.getValue()), next(n), id(itm.getId()),
        dirtiness(0), _isSmall(small)
    {

        if (_isSmall) {
            extra.small.keylen = itm.getKey().length();
        } else {
            extra.feature.cas = itm.getCas();
            extra.feature.flags = itm.getFlags();
            extra.feature.exptime = itm.getExptime();
            extra.feature.locked = false;
            extra.feature.lock_expiry = 0;
            extra.feature.keylen = itm.getKey().length();
        }

        if (setDirty) {
            markDirty();
        } else {
            markClean(NULL);
        }

        increaseCurrentSize(size());
    }

    friend class HashTable;
    friend class StoredValueFactory;

    value_t      value;           // 16 bytes
    StoredValue *next;            // 8 bytes
    int64_t      id;              // 8 bytes
    uint32_t     dirtiness : 30;  // 30 bits -+
    bool         _isSmall  :  1;  // 1 bit    | 4 bytes
    bool         _isDirty  :  1;  // 1 bit  --+

    union stored_value_bodies extra;

    static void increaseCurrentSize(size_t by);
    static void reduceCurrentSize(size_t by);
    static bool hasAvailableSpace(const Item &item);

    static size_t maxDataSize;
    static Atomic<size_t> currentSize;

    DISALLOW_COPY_AND_ASSIGN(StoredValue);
};

typedef enum {
    INVALID_VBUCKET, NOT_FOUND, INVALID_CAS,
    WAS_CLEAN, WAS_DIRTY,
    IS_LOCKED, NOMEM,
    SUCCESS
} mutation_type_t;

/**
 * Base class for visiting a hash table.
 */
class HashTableVisitor {
public:
    virtual ~HashTableVisitor() {}

    /**
     * Visit an individual item within a hash table.
     *
     * @param v a pointer to a value in the hash table
     */
    virtual void visit(StoredValue *v) = 0;
    virtual bool shouldContinue() { return true; }
};

/**
 * Hash table visitor that reports the depth of each hashtable bucket.
 */
class HashTableDepthVisitor {
public:
    virtual ~HashTableDepthVisitor() {}

    /**
     * Called once for each hashtable bucket with its depth.
     *
     * @param bucket the index of the hashtable bucket
     * @param depth the number of entries in this hashtable bucket
     */
    virtual void visit(int bucket, int depth) = 0;
};

/**
 * Hash table visitor that finds the min and max bucket depths.
 */
class HashTableDepthStatVisitor : public HashTableDepthVisitor {
public:

    HashTableDepthStatVisitor() : min(INT_MAX), max(0), size(0) {}

    void visit(int bucket, int depth) {
        (void)bucket;
        min = std::min(min, depth);
        max = std::max(max, depth);
        size += depth;
    }

    int    min;
    int    max;
    size_t size;
};

/**
 * Track the current number of hashtable visitors.
 *
 * This class is a pretty generic counter holder that increments on
 * entry and decrements on return providing RAII guarantees around an
 * atomic counter.
 */
class VisitorTracker {
public:

    /**
     * Mark a visitor as visiting.
     *
     * @param c the counter that should be incremented (and later
     * decremented).
     */
    explicit VisitorTracker(Atomic<size_t> *c) : counter(c) {
        ++(*counter);
    }
    ~VisitorTracker() {
        --(*counter);
    }
private:
    Atomic<size_t> *counter;
};

/**
 * Hash table that stores all of the items in memory.
 */
enum stored_value_type {
    small, featured
};

class StoredValueFactory {
public:

    StoredValueFactory(enum stored_value_type t = featured) : type(t) {}

    StoredValue *operator ()(const Item &itm, StoredValue *n,
                             bool setDirty = true) {
        switch(type) {
        case small:
            return newStoredValue(itm, n, setDirty, 1);
            break;
        case featured:
            return newStoredValue(itm, n, setDirty, 0);
            break;
        default:
            abort();
        };
    }

private:

    StoredValue* newStoredValue(const Item &itm, StoredValue *n, bool setDirty,
                                bool small) {
        size_t base = StoredValue::sizeOf(small);

        std::string key = itm.getKey();
        assert(key.length() < 256);
        size_t len = key.length() + base;

        StoredValue *t = new (::operator new(len))
            StoredValue(itm, n, setDirty, small);
        if (small) {
            std::memcpy(t->extra.small.keybytes, key.data(), key.length());
        } else {
            std::memcpy(t->extra.feature.keybytes, key.data(), key.length());
        }

        return t;
    }

    enum stored_value_type type;

};

class HashTable {
public:

    // Construct with number of buckets and locks.
    HashTable(size_t s = 0, size_t l = 0, enum stored_value_type t = featured)
        : valFact(t) {
        size = HashTable::getNumBuckets(s);
        n_locks = HashTable::getNumLocks(l);
        valFact = StoredValueFactory(getDefaultStorageValueType());
        assert(size > 0);
        assert(n_locks > 0);
        assert(visitors == 0);
        values = static_cast<StoredValue**>(calloc(size, sizeof(StoredValue*)));
        mutexes = new Mutex[n_locks];
        activeState = true;
    }

    ~HashTable() {
        clear(true);
        // Wait for any outstanding visitors to finish.
        while (visitors > 0) {
            usleep(100);
        }
        delete []mutexes;
        free(values);
        values = NULL;
    }

    size_t getSize(void) { return size; }
    size_t getNumLocks(void) { return n_locks; }

    /**
     * Clear the hash table.
     *
     * @param deactivate true when this hash table is being destroyed completely
     *
     * @return the number of items removed
     */
    size_t clear(bool deactivate = false);

    StoredValue *find(std::string &key) {
        assert(active());
        int bucket_num = bucket(key);
        LockHolder lh(getMutex(bucket_num));
        return unlocked_find(key, bucket_num);
    }

    mutation_type_t set(const Item &val) {
        assert(active());
        mutation_type_t rv = NOT_FOUND;
        int bucket_num = bucket(val.getKey());
        LockHolder lh(getMutex(bucket_num));
        StoredValue *v = unlocked_find(val.getKey(), bucket_num);
        Item &itm = const_cast<Item&>(val);
        if (v) {

            if (v->isLocked(ep_current_time())) {
                /*
                 * item is locked, deny if there is cas value mismatch
                 * or no cas value is provided by the user
                 */
                if (val.getCas() != v->getCas()) {
                    return IS_LOCKED;
                }
                /* allow operation*/
                v->unlock();
            } else if (val.getCas() != 0 && val.getCas() != v->getCas()) {
                return INVALID_CAS;
            }
            itm.setCas();
            rv = v->isClean() ? WAS_CLEAN : WAS_DIRTY;
            v->setValue(itm.getValue(),
                        itm.getFlags(), itm.getExptime(),
                        itm.getCas());
        } else {
            if (itm.getCas() != 0) {
                return NOT_FOUND;
            }

            if (!StoredValue::hasAvailableSpace(itm)) {
                return NOMEM;
            }

            itm.setCas();
            v = valFact(itm, values[bucket_num]);
            values[bucket_num] = v;
        }
        return rv;
    }

    bool add(const Item &val, bool isDirty = true) {
        assert(active());
        int bucket_num = bucket(val.getKey());
        LockHolder lh(getMutex(bucket_num));
        StoredValue *v = unlocked_find(val.getKey(), bucket_num);
        if (v) {
            return false;
        } else {
            Item &itm = const_cast<Item&>(val);
            itm.setCas();
            if (!StoredValue::hasAvailableSpace(itm)) {
                return NOMEM;
            }
            v = valFact(itm, values[bucket_num], isDirty);
            values[bucket_num] = v;
        }

        return true;
    }

    StoredValue *unlocked_find(const std::string &key, int bucket_num) {
        StoredValue *v = values[bucket_num];
        while (v) {
            if (v->hasKey(key)) {
                // check the expiry time
                if (v->getExptime() != 0 && v->getExptime() < ep_current_time()) {
                    (void)unlocked_del(key, bucket_num);
                    return NULL;
                }
                return v;
            }
            v = v->next;
        }
        return NULL;
    }

    inline int bucket(const char *str, const size_t len) {
        assert(active());
        int h=5381;

        for(size_t i=0; i < len; i++) {
            h = ((h << 5) + h) ^ str[i];
        }

        return abs(h % (int)size);
    }

    inline int bucket(const std::string &s) {
        return bucket(s.data(), s.length());
    }

    inline Mutex &getMutexForLock(int lock_num) {
        assert(active());
        assert(lock_num < (int)n_locks);
        assert(lock_num >= 0);
        return mutexes[lock_num];
    }

    // Get the mutex for a bucket (for doing your own lock management)
    inline Mutex &getMutex(int bucket_num) {
        return getMutexForLock(mutexForBucket(bucket_num));
    }

    /**
     * Delete a key from the cache without trying to lock the cache first
     * (Please note that you <b>MUST</b> acquire the mutex before calling
     * this function!!!
     *
     * @param key the key to delete
     * @param bucket_num the bucket to look in (must already be locked)
     * @return true if an object was deleted, false otherwise
     */
    bool unlocked_del(const std::string &key, int bucket_num) {
        assert(active());
        StoredValue *v = values[bucket_num];

        // Special case empty bucket.
        if (!v) {
            return false;
        }

        // Special case the first one
        if (v->hasKey(key)) {
            if (v->isLocked(ep_current_time())) {
                return false;
            }
            values[bucket_num] = v->next;
            delete v;
            return true;
        }

        while (v->next) {
            if (v->next->hasKey(key)) {
                StoredValue *tmp = v->next;
                if (tmp->isLocked(ep_current_time())) {
                    return false;
                }
                v->next = v->next->next;
                delete tmp;
                return true;
            } else {
                v = v->next;
            }
        }

        return false;
    }

    // True if it existed
    bool del(const std::string &key) {
        assert(active());
        int bucket_num = bucket(key);
        LockHolder lh(getMutex(bucket_num));
        return unlocked_del(key, bucket_num);
    }

    void visit(HashTableVisitor &visitor);

    void visitDepth(HashTableDepthVisitor &visitor);

    static size_t getNumBuckets(size_t);
    static size_t getNumLocks(size_t);

    static void setDefaultNumBuckets(size_t);
    static void setDefaultNumLocks(size_t);

    /**
     * Set the stored value type by name.
     *
     * @param t either "small" or "featured"
     *
     * @rteurn true if this type is not handled.
     */
    static bool setDefaultStorageValueType(const char *t);
    static void setDefaultStorageValueType(enum stored_value_type);
    static enum stored_value_type getDefaultStorageValueType();
    static const char* getDefaultStorageValueTypeStr();

private:
    inline bool active() { return activeState = true; }
    inline void active(bool newv) { activeState = newv; }

    size_t               size;
    size_t               n_locks;
    StoredValue        **values;
    Mutex               *mutexes;
    StoredValueFactory   valFact;
    Atomic<size_t>       visitors;
    bool                 activeState;

    static size_t                 defaultNumBuckets;
    static size_t                 defaultNumLocks;
    static enum stored_value_type defaultStoredValueType;

    inline int mutexForBucket(int bucket_num) {
        assert(active());
        assert(bucket_num < (int)size);
        assert(bucket_num >= 0);
        int lock_num = bucket_num % (int)n_locks;
        assert(lock_num < (int)n_locks);
        assert(lock_num >= 0);
        return lock_num;
    }

    DISALLOW_COPY_AND_ASSIGN(HashTable);
};

#endif /* STORED_VALUE_H */
