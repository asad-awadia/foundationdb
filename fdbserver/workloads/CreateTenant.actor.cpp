/*
 * CreateTenant.actor.cpp
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

#include <cstdint>

#include "fdbclient/TenantManagement.actor.h"
#include "fdbserver/workloads/workloads.actor.h"

#include "flow/actorcompiler.h" // This must be the last #include.

struct CreateTenantWorkload : TestWorkload {
	TenantName tenant;

	CreateTenantWorkload(WorkloadContext const& wcx) : TestWorkload(wcx) {
		tenant = getOption(options, "name"_sr, "DefaultTenant"_sr);
	}

	std::string description() const override { return "CreateTenant"; }
	Future<Void> setup(Database const& cx) override {
		if (clientId == 0) {
			return _setup(this, cx);
		}
		return Void();
	}

	Future<Void> start(Database const& cx) override { return Void(); }
	Future<bool> check(Database const& cx) override { return true; }
	virtual void getMetrics(std::vector<PerfMetric>& m) override {}

	ACTOR static Future<Void> _setup(CreateTenantWorkload* self, Database db) {
		try {
			Optional<TenantMapEntry> entry = wait(TenantAPI::createTenant(db.getReference(), self->tenant));
			ASSERT(entry.present());
		} catch (Error& e) {
			TraceEvent(SevError, "TenantCreationFailed").error(e);
			if (e.code() == error_code_actor_cancelled) {
				throw;
			}
			ASSERT(false);
		}
		return Void();
	}
};

WorkloadFactory<CreateTenantWorkload> CreateTenantWorkload("CreateTenant");
