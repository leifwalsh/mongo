// kv_dictionary.h

/**
*    Copyright (C) 2014 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#pragma once

#include "mongo/base/status.h"
#include "mongo/bson/ordering.h"
#include "mongo/db/storage/index_entry_comparison.h"
#include "mongo/db/storage/kv/slice.h"

namespace mongo {

    class BSONElement;
    class BSONObjBuilder;
    class KVUpdateMessage;
    class OperationContext;

    /**
     * A sort dictionary interface for mapping binary keys to binary
     * values.
     *
     * Used as the primary storage abstraction for KVRecordStore and
     * KVSortedDataInterface.
     */
    class KVDictionary {
    public:
        /**
         * Compares two binary keys in a KVDictionary. Only two possible
         * implementations exist: one for using memcmp, another for using
         * IndexEntryComparison (the key language for collections'
         * indexes)
         */
        class Comparator {
            Comparator(const IndexEntryComparison &cmp, bool unique, bool useMemcmp)
                : _cmp(cmp),
                  _unique(unique),
                  _useMemcmp(useMemcmp)
            {}

        public:
            /**
             * Return a Comparator object that compares keys using memcmp
             * and sorts by length when keys contain a common prefix.
             */
            static Comparator useMemcmp();

            /**
             * Return a Comparator object that compares keys using an
             * IndexEntryComparison.
             */
            static Comparator useIndexEntryComparison(const IndexEntryComparison &cmp, bool unique);

            /**
             * Create a comparator from a serialized byte slice.
             */
            explicit Comparator(const Slice &serialized);

            /**
             * Serialize this comparator into a byte slice that can later
             * be interpreted by the Slice constructor above. Useful for
             * dictionary implementations that need to serialize to disk
             * how keys can be sorted (and not always have that info
             * available at all times in memory with a virtual class, for
             * example).
             */
            Slice serialize() const;

            /**
             * Compare two keys with a memcmp style return value:
             *
             * < 0 iff a < b
             * == 0 iff a == b
             * > 0 iff a > b
             */
            int operator()(const Slice &a, const Slice &b) const;

        private:
            IndexEntryComparison _cmp;
            bool _unique;
            bool _useMemcmp;
        };

        virtual ~KVDictionary() { }

        /**
         * Get the associated value for `key' from the dictionary, storing
         * an owned slice into `value'
         *
         * Return:
         *   Status::OK() success, value contains and owned slice.
         *   Status::code() == ErrorCodes::NoSuchKey when no key was found, value is not populated.
         */
        virtual Status get(OperationContext *opCtx, const Slice &key, Slice &value) const = 0;

        /**
         * Insert `key' into the dictionary and associate it with `value',
         * overwriting any existing value.
         *
         * Return: Status::OK() success.
         */
        virtual Status insert(OperationContext *opCtx, const Slice &key, const Slice &value, bool overwrite=true) = 0;

        /**
         * Remove `key' and its associated value from the dictionary, if
         * any such key exists.
         *
         * Return: Status::OK() success.
         */
        virtual Status remove(OperationContext *opCtx, const Slice &key) = 0;

        /**
         * Update the value for `key' whose old value is `oldValue' and
         * whose new image should be the result of applying `message'.
         * 
         * Requires: `oldValue' is in fact the value
         *           `get(opCtx, key, ...)' would return
         * Note: violation of the requirement is undefined behavior, but
         *       usually leads to corrupt data / lost updates.
         * Return: Status:OK() success.
         */
        virtual Status update(OperationContext *opCtx, const Slice &key, const Slice &oldValue,
                              const KVUpdateMessage &message);

        /**
         * Update the value for `key' whose new image should be the result of applying `message' to
         * whatever value is present.
         *
         * Note: Should have behavior equivalent to calling `get(opCtx, key, ...)' first, then
         *       calling the other `update' overload with the result (or `Slice()' if missing).
         * Return: Status:OK() success.
         */
        virtual Status update(OperationContext *opCtx, const Slice &key, const KVUpdateMessage &message);

        /**
         * Name of the dictionary.
         */
        virtual const char *name() const = 0;

        /**
         * Basic dictionary stats.
         */
        class Stats {
        public:
            Stats() : dataSize(0), storageSize(0), numKeys(0) { }
            // Size of current 'user data' in the dictionary (sum of key/value lengths)
            int64_t dataSize;

            // Space used on the storage device
            int64_t storageSize;

            // Total number of keys.
            int64_t numKeys;
        };

        /**
         * Get stats for the dictionary.
         *
         * Note: stats may be exact or estimated. Caller should not depend
         *       on exactness.
         * Return: initialized Stats object
         */
        virtual Stats getStats() const = 0;

        /**
         * Append specific stats about this dictionary to the given bson
         * builder.
         *
         * Return: Status::OK(), success
         */
        virtual void appendCustomStats(OperationContext *opCtx, BSONObjBuilder* result, double scale) const = 0;

        /**
         * Set a custom `option' for this dictionary
         *
         * Return: Status::OK(), success
         *         Status::code() == ErrorCodes::BadValue, option not recognized / supported
         */
        virtual Status setCustomOption(OperationContext *opCtx, const BSONElement& option, BSONObjBuilder* info) = 0;

        /**
         * Run compaction if the unerlying data structure supports it.
         *
         * Return: Status::OK(), success
         */
        virtual Status compact(OperationContext *opCtx) = 0;

        /**
         * Sorted cursor interface over a KVDictionary.
         */
        class Cursor {
        public:
            virtual ~Cursor() { }

            /**
             * Checks if the cursor is safe to use.
             *
             * Return: true, the cursor is valid
             *         false, the cursor is dead and cannot be used any longer
             */
            virtual bool ok() const = 0;

            /**
             * Seek the cursor to a given key. If the key does not exist:
             * - The cursor is positioned over the first key > the given
             *   key, if getCursor() below was called with direction == 1
             * - Or the first key < the given key if direction was == -1
             *
             * TODO: Is ok() required here? I _think_ not.
             */
            virtual void seek(OperationContext *opCtx, const Slice &key) = 0;

            /**
             * Advance the cursor to the next key/value pair.
             *
             * Requires: ok() is true
             */
            virtual void advance(OperationContext *opCtx) = 0;

            /**
             * Get the current key from the cursor.
             *
             * Return: owned Slice representing the key data
             * Requires: ok() is true
             */
            virtual Slice currKey() const = 0;

            /**
             * Get the current value from the cursor.
             *
             * Return: owned Slice representing the value data
             * Requires: ok() is true
             */
            virtual Slice currVal() const = 0;
        };

        /**
         * Get a cursor over this dictionary that will iterate forward if
         * direction > 1 and backward if direction < 1. Direction also
         * affects how cursor seek lands on a key when an inexact match is
         * found. See Cursor::seek().
         *
         * Return: Cursor interface implementation (ownership passes to caller)
         */
        virtual Cursor *getCursor(OperationContext *opCtx, const Slice &key, const int direction = 1) const = 0;

        virtual Cursor *getCursor(OperationContext *opCtx, const int direction = 1) const = 0;
    };

} // namespace mongo
