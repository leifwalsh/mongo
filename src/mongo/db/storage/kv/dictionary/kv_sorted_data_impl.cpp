// kv_sorted_data_impl.cpp

/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
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


#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/storage/kv/dictionary/kv_dictionary.h"
#include "mongo/db/storage/kv/dictionary/kv_sorted_data_impl.h"
#include "mongo/db/storage/kv/slice.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"

namespace mongo {

    const int kTempKeyMaxSize = 1024; // Do the same as the heap implementation

    namespace {

        /**
         * Strips the field names from a BSON object
         */
        BSONObj stripFieldNames( const BSONObj& obj ) {
            BSONObjBuilder b;
            BSONObjIterator i( obj );
            while ( i.more() ) {
                BSONElement e = i.next();
                b.appendAs( e, "" );
            }
            return b.obj();
        }

        /**
         * Constructs a string containing the bytes of key followed by the bytes of loc.
         *
         * @param removeFieldNames true if the field names in key should be replaced with empty
         * strings, and false otherwise. Useful because field names are not necessary in an index
         * key, because the ordering of the fields is already known.
         */
        Slice makeString( const BSONObj& key, const RecordId loc, bool removeFieldNames = true ) {
            const BSONObj finalKey = removeFieldNames ? stripFieldNames( key ) : key;

            Slice s(finalKey.objsize() + sizeof loc);

            std::copy(finalKey.objdata(), finalKey.objdata() + finalKey.objsize(), s.mutableData());
            RecordId *lp = reinterpret_cast<RecordId *>(s.mutableData() + finalKey.objsize());
            *lp = loc;

            return s;
        }

        /**
         * Constructs an IndexKeyEntry from a slice containing the bytes of a BSONObject followed
         * by the bytes of a RecordId
         */
        IndexKeyEntry makeIndexKeyEntry( const Slice& slice ) {
            BSONObj key = BSONObj( slice.data() );
            RecordId loc = *reinterpret_cast<const RecordId*>( slice.data() + key.objsize() );
            return IndexKeyEntry( key, loc );
        }

        Slice makeKeyString(const BSONObj& key, bool removeFieldNames = true) {
            const BSONObj finalKey = removeFieldNames ? stripFieldNames(key) : key;
            Slice s(finalKey.objsize());
            std::copy(finalKey.objdata(), finalKey.objdata() + finalKey.objsize(), s.begin());
            return s;
        }

        Slice makeRecordIdString(const RecordId loc) {
            return Slice::of(loc);
        }

        Slice makeRecordIdString(const set<RecordId>& locs) {
            Slice s(locs.size() * sizeof(RecordId));
            std::copy(locs.begin(), locs.end(), reinterpret_cast<RecordId *>(s.begin()));
            return s;
        }

        BSONObj makeKey(const Slice &slice) {
            return BSONObj(slice.data());
        }

        RecordId makeRecordId(const Slice &slice) {
            return slice.as<RecordId>();
        }

        set<RecordId> makeRecordIdSet(const Slice &slice) {
            set<RecordId> locs;
            std::copy(reinterpret_cast<const RecordId *>(slice.begin()), reinterpret_cast<const RecordId *>(slice.end()), std::inserter(locs, locs.end()));
            return locs;
        }

        /**
         * Creates an error code message out of a key
         */
        string dupKeyError(const BSONObj& key) {
            StringBuilder sb;
            sb << "E11000 duplicate key error ";
            // TODO figure out how to include index name without dangerous casts
            sb << "dup key: " << key.toString();
            return sb.str();
        }

    }  // namespace

    KVSortedDataImpl::KVSortedDataImpl( KVDictionary* db,
                                        OperationContext* opCtx,
                                        const IndexDescriptor* desc)
        : _db(db),
          _unique(desc ? desc->infoObj()["unique"].trueValue() : false) {
        invariant( _db );
    }

    Status KVSortedDataBuilderImpl::addKey(const BSONObj& key, const RecordId& loc) {
        return _impl->insert(_txn, key, loc, _dupsAllowed);
    }

    SortedDataBuilderInterface* KVSortedDataImpl::getBulkBuilder(OperationContext* txn,
                                                                 bool dupsAllowed) {
      return new KVSortedDataBuilderImpl(this, txn, dupsAllowed);
    }

    Status KVSortedDataImpl::insert(OperationContext* txn,
                                    const BSONObj& key,
                                    const RecordId& loc,
                                    bool dupsAllowed) {
        if (key.objsize() >= kTempKeyMaxSize) {
            const string msg = mongoutils::str::stream()
                               << "KVSortedDataImpl::insert() key too large to index, failing "
                               << key.objsize() << ' ' << key;
            return Status(ErrorCodes::KeyTooLong, msg);
        }

        try {
            if (_unique) {
                if (dupsAllowed) {
                    log() << "unique but dups allowed";
                    Slice val;
                    Status s = _db->get(txn, makeKeyString(key), val);
                    if (s.isOK()) {
                        set<RecordId> locs = makeRecordIdSet(val);
                        locs.insert(loc);
                        log() << "found val, appending loc";
                        return _db->insert(txn, makeKeyString(key), makeRecordIdString(locs));
                    } else if (s == ErrorCodes::NoSuchKey) {
                        log() << "didn't find val, inserting";
                        return _db->insert(txn, makeKeyString(key), makeRecordIdString(loc));
                    } else {
                        log() << "error " << s.codeString();
                        return s;
                    }
                } else {
                    log() << "unique and dups not allowed";
                    Status s = _db->insert(txn, makeKeyString(key), makeRecordIdString(loc), false);
                    if (s == ErrorCodes::DuplicateKey) {
                        log() << "engine said unique insert got dup key";
                        return Status(ErrorCodes::DuplicateKey, dupKeyError(key));
                    }
                    return s;
                }
            } else {
                return _db->insert(txn, makeString(key, loc), Slice());
            }
        } catch (WriteConflictException) {
            if (!dupsAllowed) {
                // If we see a WriteConflictException on a unique index, according to
                // https://jira.mongodb.org/browse/SERVER-16337 we should consider it a duplicate
                // key even if this means reporting false positives.
                return Status(ErrorCodes::DuplicateKey, dupKeyError(key));
            }
            throw;
        }

        return Status::OK();
    }

    void KVSortedDataImpl::unindex(OperationContext* txn,
                                   const BSONObj& key,
                                   const RecordId& loc,
                                   bool dupsAllowed) {
        if (_unique) {
            _db->remove(txn, makeKeyString(key));
        } else {
            _db->remove(txn, makeString(key, loc));
        }
    }

    Status KVSortedDataImpl::dupKeyCheck(OperationContext* txn,
                                         const BSONObj& key,
                                         const RecordId& loc) {
        boost::scoped_ptr<SortedDataInterface::Cursor> cursor(newCursor(txn, 1));
        cursor->locate(key, RecordId());

        if (cursor->isEOF() || cursor->getKey() != key) {
            return Status::OK();
        } else if (cursor->getRecordId() == loc) {
            return Status::OK();
        } else {
            return Status(ErrorCodes::DuplicateKey, dupKeyError(key));
        }
    }

    void KVSortedDataImpl::fullValidate(OperationContext* txn, bool full, long long* numKeysOut,
                                        BSONObjBuilder* output) const {
        if (numKeysOut) {
            *numKeysOut = 0;
            for (boost::scoped_ptr<KVDictionary::Cursor> cursor(_db->getCursor(txn));
                 cursor->ok(); cursor->advance(txn)) {
                ++(*numKeysOut);
            }
        }
    }

    bool KVSortedDataImpl::isEmpty( OperationContext* txn ) {
        boost::scoped_ptr<KVDictionary::Cursor> cursor(_db->getCursor(txn));
        return !cursor->ok();
    }

    Status KVSortedDataImpl::touch(OperationContext* txn) const {
        // fullValidate iterates over every key, which brings things into memory
        long long numKeys;
        fullValidate(txn, true, &numKeys, NULL);
        return Status::OK();
    }

    long long KVSortedDataImpl::numEntries(OperationContext* txn) const {
        long long numKeys = 0;
        fullValidate(txn, true, &numKeys, NULL);
        return numKeys;
    }

    Status KVSortedDataImpl::initAsEmpty(OperationContext* txn) {
        // no-op
        return Status::OK();
    }

    long long KVSortedDataImpl::getSpaceUsedBytes( OperationContext* txn ) const {
        KVDictionary::Stats stats = _db->getStats();
        return stats.storageSize;
    }

    // ---------------------------------------------------------------------- //

    class KVSortedDataInterfaceCursor : public SortedDataInterface::Cursor {
        KVDictionary *_db;
        const int _dir;
        OperationContext *_txn;
        const bool _unique;

        mutable boost::scoped_ptr<KVDictionary::Cursor> _cursor;
        BSONObj _savedKey;
        RecordId _savedLoc;
        mutable bool _initialized;

        void _initialize() const {
            if (_initialized) {
                return;
            }
            _initialized = true;
            if (_cursor) {
                return;
            }
            _cursor.reset(_db->getCursor(_txn, _dir));
        }

        bool _locate(const BSONObj &key, const RecordId &loc) {
            if (_unique) {
                _cursor.reset(_db->getCursor(_txn, makeKeyString(key, false), _dir));
            } else {
                _cursor.reset(_db->getCursor(_txn, makeString(key, loc, false), _dir));
            }
            return !isEOF() && loc == getRecordId() && key == getKey();
        }

    public:
        KVSortedDataInterfaceCursor(KVDictionary *db, OperationContext *txn, int direction, bool unique)
            : _db(db),
              _dir(direction),
              _txn(txn),
              _unique(unique),
              _cursor(),
              _savedKey(),
              _savedLoc(),
              _initialized(false)
        {}

        virtual ~KVSortedDataInterfaceCursor() {}

        int getDirection() const {
            return _dir;
        }

        bool isEOF() const {
            _initialize();
            return !_cursor || !_cursor->ok();
        }

        bool pointsToSamePlaceAs(const Cursor& other) const {
            return getRecordId() == other.getRecordId() && getKey() == other.getKey();
        }

        void aboutToDeleteBucket(const RecordId& bucket) { }

        bool locate(const BSONObj& key, const RecordId& loc) {
            return _locate(stripFieldNames(key), loc);
        }

        void advanceTo(const BSONObj &keyBegin,
                       int keyBeginLen,
                       bool afterKey,
                       const vector<const BSONElement*>& keyEnd,
                       const vector<bool>& keyEndInclusive) {
            // make a key representing the location to which we want to advance.
            BSONObj key = IndexEntryComparison::makeQueryObject(
                                                                keyBegin,
                                                                keyBeginLen,
                                                                afterKey,
                                                                keyEnd,
                                                                keyEndInclusive,
                                                                getDirection() );
            _locate(key, _dir > 0 ? RecordId::min() : RecordId::max());
        }

        void customLocate(const BSONObj& keyBegin,
                          int keyBeginLen,
                          bool afterVersion,
                          const vector<const BSONElement*>& keyEnd,
                          const vector<bool>& keyEndInclusive) {
            // The rocks engine has this to say:
            // XXX I think these do the same thing????
            advanceTo( keyBegin, keyBeginLen, afterVersion, keyEnd, keyEndInclusive );
        }

        BSONObj getKey() const {
            _initialize();
            if (isEOF()) {
                return BSONObj();
            }
            if (_unique) {
                return makeKey(_cursor->currKey());
            } else {
                IndexKeyEntry entry = makeIndexKeyEntry(_cursor->currKey());
                return entry.key;
            }
        }

        RecordId getRecordId() const {
            _initialize();
            if (isEOF()) {
                return RecordId();
            }
            if (_unique) {
                return makeRecordId(_cursor->currVal());
            } else {
                IndexKeyEntry entry = makeIndexKeyEntry(_cursor->currKey());
                return entry.loc;
            }
        }

        void advance() {
            _initialize();
            if (!isEOF()) {
                _cursor->advance(_txn);
            }
        }

        void savePosition() {
            _initialize();
            _savedKey = getKey().getOwned();
            _savedLoc = getRecordId();
            _cursor.reset();
            _txn = NULL;
        }

        void restorePosition(OperationContext* txn) {
            invariant(!_txn && !_cursor);
            _txn = txn;
            _initialized = true;
            if (!_savedKey.isEmpty() && !_savedLoc.isNull()) {
                _locate(_savedKey, _savedLoc);
            } else {
                invariant(_savedKey.isEmpty() && _savedLoc.isNull());
                invariant(isEOF()); // this is the whole point!
            }
        }
    };

    SortedDataInterface::Cursor* KVSortedDataImpl::newCursor(OperationContext* txn,
                                                             int direction) const {
        return new KVSortedDataInterfaceCursor(_db.get(), txn, direction, _unique);
    }

} // namespace mongo
