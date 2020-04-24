/*
 * TagThrottle.actor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2020 Apple Inc. and the FoundationDB project authors
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

#include "fdbclient/TagThrottle.h"
#include "fdbclient/MasterProxyInterface.h"
#include "fdbclient/DatabaseContext.h"

#include "flow/actorcompiler.h" // has to be last include

void TagSet::addTag(TransactionTagRef tag) {
	ASSERT(CLIENT_KNOBS->MAX_TRANSACTION_TAG_LENGTH < 256); // Tag length is encoded with a single byte

	if(tag.size() > CLIENT_KNOBS->MAX_TRANSACTION_TAG_LENGTH) {
		throw tag_too_long();
	}
	if(tags.size() >= CLIENT_KNOBS->MAX_TAGS_PER_TRANSACTION) {
		throw too_many_tags();
	}

	tags.insert(TransactionTagRef(arena, tag));
	bytes += tag.size();
}

size_t TagSet::size() const {
	return tags.size();
}

// Format for throttle key:
//
// tagThrottleKeysPrefix + [auto-throttled (1-byte 0/1)] + [priority (1-byte)] + [tag list]
// tag list consists of 1 or more consecutive tags, each encoded as:
// tag.size() (1 byte) + tag's bytes. For example, tag 'foo' is: \x03foo
// The tags are listed in sorted order
//
// Currently, the throttle API supports only 1 tag per throttle
Key TagThrottleKey::toKey() const {
	ASSERT(CLIENT_KNOBS->MAX_TRANSACTION_TAG_LENGTH < 256);
	ASSERT(tags.size() > 0);

	ASSERT(tags.size() == 1); // SOMEDAY: support multiple tags per throttle

	int size = tagThrottleKeysPrefix.size() + tags.size() + 2;
	for(auto tag : tags) {
		ASSERT(tag.size() <= CLIENT_KNOBS->MAX_TRANSACTION_TAG_LENGTH);
		size += tag.size();
	}

	Key result;

	uint8_t* str = new (result.arena()) uint8_t[size];
	result.contents() = StringRef(str, size);

	memcpy(str, tagThrottleKeysPrefix.begin(), tagThrottleKeysPrefix.size());
	str += tagThrottleKeysPrefix.size();

	*(str++) = autoThrottled ? 1 : 0;
	*(str++) = (uint8_t)priority;

	for(auto tag : tags) {
		*(str++) = (uint8_t)tag.size();
		if(tag.size() > 0) {
			memcpy(str, tag.begin(), tag.size());
			str += tag.size();
		}
	}

	return result;
}

TagThrottleKey TagThrottleKey::fromKey(const KeyRef& key) {
	const uint8_t *str = key.substr(tagThrottleKeysPrefix.size()).begin();
	bool autoThrottled = *(str++) != 0;
	ThrottleApi::Priority priority = ThrottleApi::Priority(*(str++));
	TagSet tags;

	while(str < key.end()) {
		uint8_t size = *(str++);
		tags.addTag(TransactionTagRef(str, size));
		str += size;
	}

	return TagThrottleKey(tags, autoThrottled, priority);
}

TagThrottleValue TagThrottleValue::fromValue(const ValueRef& value) {
	TagThrottleValue throttleValue;
	BinaryReader reader(value, IncludeVersion());
	reader >> throttleValue;
	return throttleValue;
}

namespace ThrottleApi {
	Priority priorityFromReadVersionFlags(int flags) {
		if((flags & GetReadVersionRequest::PRIORITY_SYSTEM_IMMEDIATE) == GetReadVersionRequest::PRIORITY_SYSTEM_IMMEDIATE) {
			return Priority::IMMEDIATE;
		}
		else if((flags & GetReadVersionRequest::PRIORITY_DEFAULT) == GetReadVersionRequest::PRIORITY_DEFAULT) {
			return Priority::DEFAULT;
		}
		else if((flags & GetReadVersionRequest::PRIORITY_BATCH) == GetReadVersionRequest::PRIORITY_BATCH) {
			return Priority::BATCH;
		}

		ASSERT(false);
		throw internal_error();
	}

	const char* priorityToString(Priority priority, bool capitalize) {
		switch(priority) {
			case Priority::BATCH:
				return capitalize ? "Batch" : "batch";
			case Priority::DEFAULT:
				return capitalize ? "Default" : "default";
			case Priority::IMMEDIATE:
				return capitalize ? "Immediate" : "immediate";
		}

		ASSERT(false);
		throw internal_error();
	}

	void signalThrottleChange(Transaction &tr) {
		tr.atomicOp(tagThrottleSignalKey, LiteralStringRef("XXXXXXXXXX\x00\x00\x00\x00"), MutationRef::SetVersionstampedValue);
	}

	ACTOR Future<Void> updateThrottleCount(Transaction *tr, int64_t delta) {
		state Future<Optional<Value>> countVal = tr->get(tagThrottleCountKey);
		state Future<Optional<Value>> limitVal = tr->get(tagThrottleLimitKey);

		wait(success(countVal) && success(limitVal));

		int64_t count = 0;
		int64_t limit = 0;

		if(countVal.get().present()) {
			BinaryReader reader(countVal.get().get(), Unversioned());
			reader >> count;
		}

		if(limitVal.get().present()) {
			BinaryReader reader(limitVal.get().get(), Unversioned());
			reader >> limit;
		}

		count += delta;

		if(count > limit) {
			throw too_many_tag_throttles();
		}

		BinaryWriter writer(Unversioned());
		writer << count;

		tr->set(tagThrottleCountKey, writer.toValue());
		return Void();
	}

	ACTOR Future<std::vector<TagThrottleInfo>> getThrottledTags(Database db, int limit) {
		state Transaction tr(db);

		loop {
			try {
				Standalone<RangeResultRef> throttles = wait(tr.getRange(tagThrottleKeys, limit));
				std::vector<TagThrottleInfo> results;
				for(auto throttle : throttles) {
					results.push_back(TagThrottleInfo(TagThrottleKey::fromKey(throttle.key), TagThrottleValue::fromValue(throttle.value)));
				}
				return results;
			}
			catch(Error& e) {
				wait(tr.onError(e));
			}
		}
	}

	ACTOR Future<Void> throttleTags(Database db, TagSet tags, double tpsRate, double initialDuration, bool autoThrottled, ThrottleApi::Priority priority, Optional<double> expirationTime) {
		state Transaction tr(db);
		state Key key = TagThrottleKey(tags, autoThrottled, priority).toKey();

		ASSERT(initialDuration > 0);

		TagThrottleValue throttle(tpsRate, expirationTime.present() ? expirationTime.get() : 0, initialDuration);
		BinaryWriter wr(IncludeVersion());
		wr << throttle;
		state Value value = wr.toValue();

		loop {
			try {
				if(!autoThrottled) {
					Optional<Value> oldThrottle = wait(tr.get(key));
					if(!oldThrottle.present()) {
						wait(updateThrottleCount(&tr, 1));
					}
				}

				tr.set(key, value);

				if(!autoThrottled) {
					signalThrottleChange(tr);
				}

				wait(tr.commit());
				return Void();
			}
			catch(Error& e) {
				wait(tr.onError(e));
			}
		}
	}

	ACTOR Future<bool> unthrottleTags(Database db, TagSet tags, bool autoThrottled, ThrottleApi::Priority priority) {
		state Transaction tr(db);
		state Key key = TagThrottleKey(tags, autoThrottled, priority).toKey();

		loop {
			try {
				state Optional<Value> value = wait(tr.get(key));
				if(value.present()) {
					if(!autoThrottled) {
						wait(updateThrottleCount(&tr, -1));
					}

					tr.clear(key);
					signalThrottleChange(tr);

					wait(tr.commit());
				}

				return value.present();
			}
			catch(Error& e) {
				wait(tr.onError(e));
			}
		}
	}

	ACTOR Future<uint64_t> unthrottleTags(Database db, KeyRef beginKey, KeyRef endKey) {
		state Transaction tr(db);

		state KeySelector begin = firstGreaterOrEqual(beginKey);
		state KeySelectorRef end = firstGreaterOrEqual(endKey);

		state uint64_t unthrottledTags = 0;

		loop {
			try {
				state Standalone<RangeResultRef> tags = wait(tr.getRange(begin, end, 1000));
				state uint64_t localUnthrottledTags = 0;
				uint64_t manualUnthrottledTags = 0;
				for(auto tag : tags) {
					bool autoThrottled = TagThrottleKey::fromKey(tag.key).autoThrottled;
					if(autoThrottled) {
						++localUnthrottledTags;
					}
					else {
						++localUnthrottledTags;
						++manualUnthrottledTags;
					}

					tr.clear(tag.key);
				}

				if(manualUnthrottledTags > 0) {
					wait(updateThrottleCount(&tr, -manualUnthrottledTags));
				}

				if(localUnthrottledTags > 0) {
					signalThrottleChange(tr);
				}

				wait(tr.commit());

				unthrottledTags += localUnthrottledTags;

				if(!tags.more) {
					return unthrottledTags;
				}

				ASSERT(tags.size() > 0);
				begin = KeySelector(firstGreaterThan(tags[tags.size()-1].key), tags.arena());
			}
			catch(Error& e) {
				wait(tr.onError(e));
			}
		}
	}	

	Future<uint64_t> unthrottleManual(Database db) {
		return unthrottleTags(db, tagThrottleKeysPrefix, tagThrottleAutoKeysPrefix);	
	}

	Future<uint64_t> unthrottleAuto(Database db) {
		return unthrottleTags(db, tagThrottleAutoKeysPrefix, tagThrottleKeys.end);	
	}

	Future<uint64_t> unthrottleAll(Database db) {
		return unthrottleTags(db, tagThrottleKeys.begin, tagThrottleKeys.end);	
	}

	ACTOR Future<Void> enableAuto(Database db, bool enabled) {
		state Transaction tr(db);

		loop {
			try {
				Optional<Value> value = wait(tr.get(tagThrottleAutoEnabledKey));
				if(!value.present() || (enabled && value.get() != LiteralStringRef("1") || (!enabled && value.get() != LiteralStringRef("0")))) {
					tr.set(tagThrottleAutoEnabledKey, LiteralStringRef(enabled ? "1" : "0"));
					signalThrottleChange(tr);

					wait(tr.commit());
				}
				return Void();
			}
			catch(Error& e) {
				wait(tr.onError(e));
			}
		}
	}
}