// kv_record_store_capped.h

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

#include "mongo/db/storage/kv/dictionary/kv_record_store.h"

namespace mongo {

    class KVSizeStorer;

    // Like a KVRecordStore, but size is capped and inserts
    // may truncate off old records from the beginning.
    class KVRecordStoreCapped : public KVRecordStore {
    public:
        // KVRecordStore takes ownership of db
        KVRecordStoreCapped( KVDictionary *db,
                             OperationContext* opCtx,
                             const StringData& ns,
                             const StringData& ident,
                             const CollectionOptions& options,
                             KVSizeStorer *sizeStorer);

        virtual ~KVRecordStoreCapped() { }

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const char* data,
                                                  int len,
                                                  bool enforceQuota );

        virtual StatusWith<DiskLoc> insertRecord( OperationContext* txn,
                                                  const DocWriter* doc,
                                                  bool enforceQuota );

        virtual void deleteRecord( OperationContext* txn, const DiskLoc& dl );

        virtual void appendCustomStats( OperationContext* txn,
                                        BSONObjBuilder* result,
                                        double scale ) const;

        // KVRecordStore is not capped, KVRecordStoreCapped is capped
        virtual bool isCapped() const { return true; }

        virtual void temp_cappedTruncateAfter(OperationContext* txn,
                                              DiskLoc end,
                                              bool inclusive);

        virtual void setCappedDeleteCallback(CappedDocumentDeleteCallback* cb) {
            _cappedDeleteCallback = cb;
        }

        virtual bool cappedMaxDocs() const { return _cappedMaxDocs; }

        virtual bool cappedMaxSize() const { return _cappedMaxSize; }

    private:
        bool needsDelete(OperationContext *txn) const;

        void deleteAsNeeded(OperationContext *txn);

        const int64_t _cappedMaxSize;
        const int64_t _cappedMaxDocs;
        CappedDocumentDeleteCallback* _cappedDeleteCallback;
        boost::mutex _cappedDeleteMutex;
    };

} // namespace mongo
