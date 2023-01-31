/*
 * BulkSetup.actor.cpp
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

#include "fdbclient/TenantEntryCache.actor.h"
#include "fdbclient/TenantManagement.actor.h"
#include "fdbrpc/ContinuousSample.h"
#include "fdbserver/Knobs.h"
#include "fdbserver/TesterInterface.actor.h"
#include "fdbserver/workloads/workloads.actor.h"
#include "fdbserver/workloads/BulkSetup.actor.h"
#include "flow/genericactors.actor.h"
#include "flow/actorcompiler.h" // This must be the last #include.

struct BulkSetupWorkload : TestWorkload {
	static constexpr auto NAME = "BulkLoadWithTenants";

	int nodeCount;
	double transactionsPerSecond;
	Key keyPrefix;
	double maxNumTenants;
	double minNumTenants;
	std::vector<Reference<Tenant>> tenants;
	bool deleteTenants;
	double testDuration;

	BulkSetupWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		transactionsPerSecond = getOption(options, "transactionsPerSecond"_sr, 5000.0) / clientCount;
		nodeCount = getOption(options, "nodeCount"_sr, transactionsPerSecond * clientCount);
		keyPrefix = unprintable(getOption(options, "keyPrefix"_sr, ""_sr).toString());
		// maximum and minimum number of tenants per client
		maxNumTenants = getOption(options, "maxNumTenants"_sr, 0);
		minNumTenants = getOption(options, "minNumTenants"_sr, 0);
		deleteTenants = getOption(options, "deleteTenants"_sr, false);
		ASSERT(minNumTenants <= maxNumTenants);
		testDuration = getOption(options, "testDuration"_sr, -1);
	}

	void getMetrics(std::vector<PerfMetric>& m) override {}

	Key keyForIndex(int n) { return key(n); }
	Key key(int n) { return doubleToTestKey((double)n / nodeCount, keyPrefix); }
	Value value(int n) { return doubleToTestKey(n, keyPrefix); }

	Standalone<KeyValueRef> operator()(int n) { return KeyValueRef(key(n), value((n + 1) % nodeCount)); }

	ACTOR static Future<Void> _setup(BulkSetupWorkload* workload, Database cx) {
		// create a bunch of tenants (between min and max tenants)
		state int numTenantsToCreate =
		    deterministicRandom()->randomInt(workload->minNumTenants, workload->maxNumTenants + 1);
		TraceEvent("BulkSetupTenantCreation").detail("NumTenants", numTenantsToCreate);

		if (numTenantsToCreate > 0) {
			state std::vector<Future<Optional<TenantMapEntry>>> tenantFutures;
			for (int i = 0; i < numTenantsToCreate; i++) {
				TenantName tenantName = TenantNameRef(format("BulkSetupTenant_%04d", i));
				TraceEvent("CreatingTenant").detail("Tenant", tenantName);
				tenantFutures.push_back(TenantAPI::createTenant(cx.getReference(), tenantName));
			}
			wait(waitForAll(tenantFutures));
			for (auto& f : tenantFutures) {
				ASSERT(f.get().present());
				workload->tenants.push_back(makeReference<Tenant>(f.get().get().id, f.get().get().tenantName));
			}
		}
		wait(bulkSetup(cx,
		               workload,
		               workload->nodeCount,
		               Promise<double>(),
		               false,
		               0.0,
		               1e12,
		               std::vector<uint64_t>(),
		               Promise<std::vector<std::pair<uint64_t, double>>>(),
		               0,
		               0.1,
		               0,
		               0,
		               workload->tenants));

		// We want to ensure that tenant deletion happens before the restore phase starts
		if (workload->deleteTenants) {
			state int numTenantsToDelete = deterministicRandom()->randomInt(0, workload->tenants.size() + 1);
			if (numTenantsToDelete > 0) {
				state int i;
				for (i = 0; i < numTenantsToDelete; i++) {
					state int tenantIndex = deterministicRandom()->randomInt(0, workload->tenants.size());
					state Reference<Tenant> tenant = workload->tenants[tenantIndex];
					TraceEvent("BulkSetupTenantDeletionClearing")
					    .detail("Tenant", tenant)
					    .detail("TotalNumTenants", workload->tenants.size());
					// clear the tenant
					state ReadYourWritesTransaction tr = ReadYourWritesTransaction(cx, tenant);
					loop {
						try {
							tr.clear(normalKeys);
							wait(tr.commit());
							break;
						} catch (Error& e) {
							wait(tr.onError(e));
						}
					}
					// delete the tenant
					wait(success(TenantAPI::deleteTenant(cx.getReference(), tenant->name.get(), tenant->id())));
					workload->tenants.erase(workload->tenants.begin() + tenantIndex);

					TraceEvent("BulkSetupTenantDeletionDone")
					    .detail("Tenant", tenant)
					    .detail("TotalNumTenants", workload->tenants.size());
				}
			}
		}
		return Void();
	}

	Future<Void> setup(Database const& cx) override { return Void(); }

	Future<Void> start(Database const& cx) override {
		if (clientId == 0) {
			if (testDuration > 0) {
				return timeout(_setup(this, cx), testDuration, Void());
			} else {
				return _setup(this, cx);
			}
		}
		return Void();
	}

	Future<bool> check(Database const& cx) override { return true; }
};

WorkloadFactory<BulkSetupWorkload> BulkSetupWorkloadFactory;
