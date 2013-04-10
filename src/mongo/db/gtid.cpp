/**
*    Copyright (C) 2012 Tokutek Inc.
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
*/

#include "gtid.h"

#include "mongo/pch.h"

namespace mongo {

    // TODO: Is there a faster comparison function for this?
    int GTID::cmp(GTID a, GTID b) {
        if (a._primarySeqNo != b._primarySeqNo) {
            return (a._primarySeqNo < b._primarySeqNo) ? -1 : 1;
        }
        if (a._GTSeqNo == b._GTSeqNo) return 0;
        return (a._GTSeqNo < b._GTSeqNo) ? -1 : 1;
    }

    uint32_t GTID::GTIDBinarySize() {
        // for _primarySeqNo and _GTSeqNo
        return 2*sizeof(uint64_t);
    }

#define SWAP64(x) \
        ((uint64_t)((((uint64_t)(x) & 0xff00000000000000ULL) >> 56) | \
                    (((uint64_t)(x) & 0x00ff000000000000ULL) >> 40) | \
                    (((uint64_t)(x) & 0x0000ff0000000000ULL) >> 24) | \
                    (((uint64_t)(x) & 0x000000ff00000000ULL) >>  8) | \
                    (((uint64_t)(x) & 0x00000000ff000000ULL) <<  8) | \
                    (((uint64_t)(x) & 0x0000000000ff0000ULL) << 24) | \
                    (((uint64_t)(x) & 0x000000000000ff00ULL) << 40) | \
                    (((uint64_t)(x) & 0x00000000000000ffULL) << 56)))

    GTID::GTID() {
        _primarySeqNo = 0;
        _GTSeqNo = 0;
    }

    GTID::GTID(uint64_t primarySeqNo, uint64_t GTSeqNo) {
        _primarySeqNo = primarySeqNo;
        _GTSeqNo = GTSeqNo;
    }

    GTID::GTID(const char* binData) {
        const char* pos = binData;
        uint64_t swappedPrim = *(uint64_t *)pos;
        pos += sizeof(uint64_t);
        uint64_t swappedSec = *(uint64_t *)pos;
        _primarySeqNo = SWAP64(swappedPrim);
        _GTSeqNo = SWAP64(swappedSec);
    }

    void GTID::serializeBinaryData(char* binData) {
        char* pos = binData;
        uint64_t prim =  SWAP64(_primarySeqNo);
        memcpy(pos, &prim, sizeof(uint64_t));
        pos += sizeof(uint64_t);
        uint64_t sec =  SWAP64(_GTSeqNo);
        memcpy(pos, &sec, sizeof(uint64_t));
    }

    void GTID::inc() {
        _GTSeqNo++;
    }

    void GTID::inc_primary() {
        _primarySeqNo++;
        _GTSeqNo = 0;
    }




    
    GTIDManager::GTIDManager( GTID lastGTID ) {
        _nextLiveGTID = lastGTID;
        _nextLiveGTID.inc();
        _minLiveGTID = _nextLiveGTID;
        // note that _minUnappliedGTID is not set
    }

    GTIDManager::~GTIDManager() {
    }

    // This function is meant to only be called on a primary,
    // it assumes that we are fully up to date and are the ones
    // getting GTIDs for transactions that will be applying
    // new data to the replica set. 
    //
    // returns a GTID that is an increment of _lastGTID
    // also notes that GTID has been handed out
    GTID GTIDManager::getGTIDForPrimary() {
        GTID ret;
        _lock.lock();
        ret = _nextLiveGTID;
        _liveGTIDs.insert(ret);
        _nextLiveGTID.inc();
        _lock.unlock();
        return ret;
    }
    
    // notification that user of GTID has completed work
    // and either committed or aborted transaction associated with
    // GTID
    //
    // THIS MUST BE DONE ON A PRIMARY
    //
    void GTIDManager::noteLiveGTIDDone(GTID gtid) {
        _lock.lock();
        dassert(GTID::cmp(gtid, _minLiveGTID) >= 0);
        dassert(_liveGTIDs.size() > 0);
        // remove from list of GTIDs
        _liveGTIDs.erase(gtid);
        // if what we are removing is currently the minumum live GTID
        // we need to update the minimum live GTID
        if (GTID::cmp(_minLiveGTID, gtid) == 0) {
            if (_liveGTIDs.size() == 0) {
                _minLiveGTID = _nextLiveGTID;
            }
            else {
                // get the minumum from _liveGTIDs and set it to _minLiveGTIDs
                _minLiveGTID = *(_liveGTIDs.begin());
            }
            // note that on a primary, which we must be, these are equivalent
            _minUnappliedGTID = _minLiveGTID;
        }
        _lock.unlock();
    }


    // This function is called on a secondary when a GTID 
    // from the primary is added and committed to the opLog
    void GTIDManager::noteGTIDAdded(GTID gtid) {
        _lock.lock();
        // if we are adding a GTID on a secondary, then 
        // these values must be equal
        dassert(GTID::cmp(_nextLiveGTID, _minLiveGTID) == 0);
        dassert(GTID::cmp(_nextLiveGTID, gtid) <= 0);
        _nextLiveGTID = gtid;
        _minLiveGTID = gtid;
        _lock.unlock();
    }

    // called when a secondary takes an unapplied GTID it has read in the oplog
    // and starts to apply it
    void GTIDManager::noteApplyingGTID(GTID gtid) {
        _lock.lock();
        dassert(GTID::cmp(gtid, _minUnappliedGTID) > 0);
        dassert(GTID::cmp(gtid, _nextUnappliedGTID) >= 0);
        if (_unappliedGTIDs.size() == 0) {
            _minUnappliedGTID = gtid;
        }

        _unappliedGTIDs.insert(gtid);        
        _nextUnappliedGTID = gtid;
        _nextUnappliedGTID.inc();
        _lock.unlock();
    }

    // called when a GTID has finished being applied, which means
    // we can remove it from the unappliedGTIDs set
    void GTIDManager::noteGTIDApplied(GTID gtid) {
        _lock.lock();
        dassert(GTID::cmp(gtid, _minUnappliedGTID) >= 0);
        dassert(_unappliedGTIDs.size() > 0);
        // remove from list of GTIDs
        _unappliedGTIDs.erase(gtid);
        // if what we are removing is currently the minumum live GTID
        // we need to update the minimum live GTID
        if (GTID::cmp(_minUnappliedGTID, gtid) == 0) {
            if (_unappliedGTIDs.size() == 0) {
                _minUnappliedGTID= _nextUnappliedGTID;
            }
            else {
                // get the minumum from _liveGTIDs and set it to _minLiveGTIDs
                _minUnappliedGTID = *(_unappliedGTIDs.begin());
            }
        }
        _lock.unlock();
    }


    void GTIDManager::getMins(GTID* minLiveGTID, GTID* minUnappliedGTID) {
        _lock.lock();
        *minLiveGTID = _minLiveGTID;
        *minUnappliedGTID = _minUnappliedGTID;
        _lock.unlock();
    }

    void GTIDManager::resetManager(GTID lastGTID) {
        // TODO: figure out what to do with unapplied GTID info here
        _lock.lock();
        dassert(_liveGTIDs.size() == 0);
        _nextLiveGTID = lastGTID;
        _nextLiveGTID.inc_primary();
        _minLiveGTID = _nextLiveGTID;
        _lock.unlock();
    }

} // namespace mongo