/*
 * BenchStream.actor.cpp
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

#include "benchmark/benchmark.h"

#include "flow/flow.h"
#include "flow/ThreadHelper.actor.h"
#include "flow/PriorityMultiLock.actor.h"
#include <deque>
#include "flow/actorcompiler.h" // This must be the last #include.

ACTOR static Future<Void> benchPriorityMultiLock(benchmark::State* benchState) {
	state std::vector<int> priorities;

	// Set up priority list with limits 10, 20, 30, ...
	while (priorities.size() < benchState->range(0)) {
		priorities.push_back(10 * (priorities.size() + 1));
	}

	state int concurrency = priorities.size() * 10;
	state PriorityMultiLock* pml = new PriorityMultiLock(concurrency, priorities);
	state std::vector<int> counts;
	counts.resize(priorities.size(), 0);

	// Clog the lock buy taking concurrency locks
	state std::deque<Future<PriorityMultiLock::Lock>> lockFutures;
	for (int j = 0; j < concurrency; ++j) {
		lockFutures.push_back(pml->lock(j % priorities.size()));
	}

	// Wait for all of the initial locks to be taken
	// This will work regardless of their priorities as there are only n = concurrency of them
	wait(waitForAll(std::vector<Future<PriorityMultiLock::Lock>>(lockFutures.begin(), lockFutures.end())));

	// For each iteration of the loop, one new lock user is created, for a total of
	// concurrency + 1 users.  The new user replaces an old one, which is then waited
	// on.  This will succeed regardless of the lock priorities used because prior to
	// new user there were only n = concurrency users so they will all be served before
	// the new user.
	state int p = 0;
	state int i = 0;
	while (benchState->KeepRunning()) {
		// Get and replace the i'th lock future with a new lock waiter
		Future<PriorityMultiLock::Lock> f = lockFutures[i];
		lockFutures[i] = pml->lock(p);

		PriorityMultiLock::Lock lock = wait(f);

		// Rotate to another priority
		if (++p == priorities.size()) {
			p = 0;
		}

		// Rotate to next lock index
		if (++i == lockFutures.size()) {
			i = 0;
		}
	}

	benchState->SetItemsProcessed(static_cast<long>(benchState->iterations()));

	delete pml;
	return Void();
}

static void bench_priorityMultiLock(benchmark::State& benchState) {
	onMainThread([&benchState]() { return benchPriorityMultiLock(&benchState); }).blockUntilReady();
}

BENCHMARK(bench_priorityMultiLock)->DenseRange(1, 8)->ReportAggregatesOnly(true);
