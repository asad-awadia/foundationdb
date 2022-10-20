/*
 * MockGlobalState.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2022 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FOUNDATIONDB_MOCKGLOBALSTATE_H
#define FOUNDATIONDB_MOCKGLOBALSTATE_H

#include "StorageMetrics.h"
#include "fdbclient/KeyRangeMap.h"
#include "fdbclient/StorageServerInterface.h"
#include "fdbclient/DatabaseConfiguration.h"
#include "SimulatedCluster.h"
#include "ShardsAffectedByTeamFailure.h"

struct MockGlobalStateTester;

enum class MockShardStatus {
	EMPTY = 0, // data loss
	COMPLETED,
	INFLIGHT,
	UNSET
};

inline bool isStatusTransitionValid(MockShardStatus from, MockShardStatus to) {
	switch (from) {
	case MockShardStatus::UNSET:
	case MockShardStatus::EMPTY:
	case MockShardStatus::INFLIGHT:
		return to == MockShardStatus::COMPLETED || to == MockShardStatus::INFLIGHT || to == MockShardStatus::EMPTY;
	case MockShardStatus::COMPLETED:
		return to == MockShardStatus::EMPTY;
	default:
		ASSERT(false);
	}
	return false;
}

class MockStorageServer {
	friend struct MockGlobalStateTester;

public:
	struct ShardInfo {
		MockShardStatus status;
		uint64_t shardSize;

		bool operator==(const ShardInfo& a) const { return shardSize == a.shardSize && status == a.status; }
		bool operator!=(const ShardInfo& a) const { return !(a == *this); }
	};

	static constexpr uint64_t DEFAULT_DISK_SPACE = 1000LL * 1024 * 1024 * 1024;

	// control plane statistics associated with a real storage server
	uint64_t usedDiskSpace = 0, availableDiskSpace = DEFAULT_DISK_SPACE;

	// In-memory counterpart of the `serverKeys` in system keyspace
	// the value ShardStatus is [InFlight, Completed, Empty] and metrics uint64_t is the shard size, the caveat is the
	// size() and nthRange() would use the metrics as index instead
	KeyRangeMap<ShardInfo> serverKeys;

	// sampled metrics
	StorageServerMetrics metrics;
	CoalescedKeyRangeMap<bool, int64_t, KeyBytesMetric<int64_t>> byteSampleClears;

	StorageServerInterface ssi; // serve RPC requests
	UID id;
	bool primary = true; // Only support single region MGS for now

	MockStorageServer() = default;

	MockStorageServer(StorageServerInterface ssi, uint64_t availableDiskSpace, uint64_t usedDiskSpace = 0)
	  : usedDiskSpace(usedDiskSpace), availableDiskSpace(availableDiskSpace), ssi(ssi), id(ssi.id()) {}

	MockStorageServer(const UID& id, uint64_t availableDiskSpace, uint64_t usedDiskSpace = 0)
	  : MockStorageServer(StorageServerInterface(id), availableDiskSpace, usedDiskSpace) {}

	decltype(serverKeys)::Ranges getAllRanges() { return serverKeys.ranges(); }

	bool allShardStatusEqual(KeyRangeRef range, MockShardStatus status);

	// change the status of range. This function may result in split to make the shard boundary align with range.begin
	// and range.end. In this case, if restrictSize==true, the sum of the split shard size is strictly equal to the old
	// large shard. Otherwise, the size are randomly generated between (min_shard_size, max_shard_size)
	void setShardStatus(KeyRangeRef range, MockShardStatus status, bool restrictSize);

	// this function removed an aligned range from server
	void removeShard(KeyRangeRef range);

	uint64_t sumRangeSize(KeyRangeRef range) const;

protected:
	void threeWayShardSplitting(KeyRangeRef outerRange,
	                            KeyRangeRef innerRange,
	                            uint64_t outerRangeSize,
	                            bool restrictSize);

	void twoWayShardSplitting(KeyRangeRef range, KeyRef splitPoint, uint64_t rangeSize, bool restrictSize);
};

class MockGlobalState {
	friend struct MockGlobalStateTester;

public:
	typedef ShardsAffectedByTeamFailure::Team Team;
	// In-memory counterpart of the `keyServers` in system keyspace
	Reference<ShardsAffectedByTeamFailure> shardMapping;
	// In-memory counterpart of the `serverListKeys` in system keyspace
	std::map<UID, MockStorageServer> allServers;
	DatabaseConfiguration configuration;

	// user defined parameters for mock workload purpose
	double emptyProb; // probability of doing an empty read
	uint32_t minByteSize, maxByteSize; // the size band of a point data operation
	bool restrictSize = true;

	MockGlobalState() : shardMapping(new ShardsAffectedByTeamFailure) {}

	static UID indexToUID(uint64_t a) { return UID(a, a); }
	void initializeAsEmptyDatabaseMGS(const DatabaseConfiguration& conf,
	                                  uint64_t defaultDiskSpace = MockStorageServer::DEFAULT_DISK_SPACE);

	void addStorageServer(StorageServerInterface server, uint64_t diskSpace = MockStorageServer::DEFAULT_DISK_SPACE);

	// check methods
	/* Shard status contract:
	 * Shard is static.
	 * * In mgs.shardMapping, the destination teams is empty for the given shard;
	 * * For each MSS belonging to the source teams, mss.serverKeys[shard] = Completed
	 * Shard is in-flight.
	 * * In mgs.shardMapping,the destination teams is non-empty for a given shard;
	 * * For each MSS belonging to the source teams, mss.serverKeys[shard] = Completed
	 * * For each MSS belonging to the destination teams, mss.serverKeys[shard] = InFlight|Completed
	 * Shard is lost.
	 * * In mgs.shardMapping,  the destination teams is empty for the given shard;
	 * * For each MSS belonging to the source teams, mss.serverKeys[shard] = Empty
	 */
	bool serverIsSourceForShard(const UID& serverId, KeyRangeRef shard, bool inFlightShard = false);
	bool serverIsDestForShard(const UID& serverId, KeyRangeRef shard);

	/* Server status contract:
	 * Server X  is removed
	 * * mgs.shardMapping doesn’t have any information about X
	 * * mgs.allServer doesn’t contain X
	 * Server X is healthy
	 * * mgs.allServer[X] is existed
	 * Server X is failed but haven’t been removed (a temporary status between healthy and removed)
	 * * mgs.shardMapping doesn’t have any information about X
	 * * mgs.allServer[X] is existed
	 */
	bool allShardRemovedFromServer(const UID& serverId);
};

#endif // FOUNDATIONDB_MOCKGLOBALSTATE_H
