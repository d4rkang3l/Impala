// Copyright (c) 2012 Cloudera, Inc. All rights reserved.

#include <stdlib.h>
#include <stdio.h>
#include <iostream>
#include <gtest/gtest.h>
#include <boost/bind.hpp>
#include "common/object-pool.h"
#include "util/runtime-profile.h"
#include "util/cpu-info.h"

using namespace std;
using namespace boost;

namespace impala {

TEST(CountersTest, Basic) { 
  ObjectPool pool;
  RuntimeProfile profile_a(&pool, "ProfileA");
  RuntimeProfile profile_a1(&pool, "ProfileA1");
  RuntimeProfile profile_a2(&pool, "ProfileAb");

  TRuntimeProfileTree thrift_profile;

  profile_a.AddChild(&profile_a1);
  profile_a.AddChild(&profile_a2);

  // Test Empty
  profile_a.ToThrift(&thrift_profile.nodes);
  EXPECT_EQ(thrift_profile.nodes.size(), 3);
  thrift_profile.nodes.clear();

  RuntimeProfile::Counter* counter_a;
  RuntimeProfile::Counter* counter_b;
  RuntimeProfile::Counter* counter_merged;
  
  // Updating/setting counter
  counter_a = profile_a.AddCounter("A", TCounterType::UNIT);
  EXPECT_TRUE(counter_a != NULL);
  counter_a->Update(10);
  counter_a->Update(-5);
  EXPECT_EQ(counter_a->value(), 5);
  counter_a->Set(1);
  EXPECT_EQ(counter_a->value(), 1);
  
  counter_b = profile_a2.AddCounter("B", TCounterType::BYTES);
  EXPECT_TRUE(counter_b != NULL);

  // Serialize/deserialize
  profile_a.ToThrift(&thrift_profile.nodes);
  RuntimeProfile* from_thrift = RuntimeProfile::CreateFromThrift(&pool, thrift_profile);
  counter_merged = from_thrift->GetCounter("A");
  EXPECT_EQ(counter_merged->value(), 1);
  EXPECT_TRUE(from_thrift->GetCounter("Not there") ==  NULL);

  // Merge
  RuntimeProfile merged_profile(&pool, "Merged");
  merged_profile.Merge(from_thrift);
  counter_merged = merged_profile.GetCounter("A");
  EXPECT_EQ(counter_merged->value(), 1);

  // Merge 2 more times, counters should get aggregated
  merged_profile.Merge(from_thrift);
  merged_profile.Merge(from_thrift);
  EXPECT_EQ(counter_merged->value(), 3);

  // Update
  RuntimeProfile updated_profile(&pool, "Updated");
  updated_profile.Update(thrift_profile);
  RuntimeProfile::Counter* counter_updated = updated_profile.GetCounter("A");
  EXPECT_EQ(counter_updated->value(), 1);

  // Update 2 more times, counters should stay the same
  updated_profile.Update(thrift_profile);
  updated_profile.Update(thrift_profile);
  EXPECT_EQ(counter_updated->value(), 1);
}

void ValidateCounter(RuntimeProfile* profile, const string& name, int64_t value) {
  RuntimeProfile::Counter* counter = profile->GetCounter(name);
  EXPECT_TRUE(counter != NULL);
  EXPECT_EQ(counter->value(), value);
}

TEST(CountersTest, MergeAndUpdate) {
  // Create two trees.  Each tree has two children, one of which has the
  // same name in both trees.  Merging the two trees should result in 3
  // children, with the counters from the shared child aggregated.

  ObjectPool pool;
  RuntimeProfile profile1(&pool, "Parent1");
  RuntimeProfile p1_child1(&pool, "Child1");
  RuntimeProfile p1_child2(&pool, "Child2");
  profile1.AddChild(&p1_child1);
  profile1.AddChild(&p1_child2);
  
  RuntimeProfile profile2(&pool, "Parent2");
  RuntimeProfile p2_child1(&pool, "Child1");
  RuntimeProfile p2_child3(&pool, "Child3");
  profile2.AddChild(&p2_child1);
  profile2.AddChild(&p2_child3);

  // Create parent level counters
  RuntimeProfile::Counter* parent1_shared = 
      profile1.AddCounter("Parent Shared", TCounterType::UNIT);
  RuntimeProfile::Counter* parent2_shared = 
      profile2.AddCounter("Parent Shared", TCounterType::UNIT);
  RuntimeProfile::Counter* parent1_only = 
      profile1.AddCounter("Parent 1 Only", TCounterType::UNIT);
  RuntimeProfile::Counter* parent2_only = 
      profile2.AddCounter("Parent 2 Only", TCounterType::UNIT);
  parent1_shared->Update(1);
  parent2_shared->Update(3);
  parent1_only->Update(2);
  parent2_only->Update(5);

  // Create child level counters
  RuntimeProfile::Counter* p1_c1_shared =
    p1_child1.AddCounter("Child1 Shared", TCounterType::UNIT);
  RuntimeProfile::Counter* p1_c1_only =
    p1_child1.AddCounter("Child1 Parent 1 Only", TCounterType::UNIT);
  RuntimeProfile::Counter* p1_c2 =
    p1_child2.AddCounter("Child2", TCounterType::UNIT);
  RuntimeProfile::Counter* p2_c1_shared =
    p2_child1.AddCounter("Child1 Shared", TCounterType::UNIT);
  RuntimeProfile::Counter* p2_c1_only =
    p1_child1.AddCounter("Child1 Parent 2 Only", TCounterType::UNIT);
  RuntimeProfile::Counter* p2_c3 =
    p2_child3.AddCounter("Child3", TCounterType::UNIT);
  p1_c1_shared->Update(10);
  p1_c1_only->Update(50);
  p2_c1_shared->Update(20);
  p2_c1_only->Update(100);
  p2_c3->Update(30);
  p1_c2->Update(40);

  // Merge the two and validate
  TRuntimeProfileTree tprofile1;
  profile1.ToThrift(&tprofile1);
  RuntimeProfile* merged_profile = RuntimeProfile::CreateFromThrift(&pool, tprofile1);
  merged_profile->Merge(&profile2);
  EXPECT_EQ(merged_profile->num_counters(), 4);
  ValidateCounter(merged_profile, "Parent Shared", 4);
  ValidateCounter(merged_profile, "Parent 1 Only", 2);
  ValidateCounter(merged_profile, "Parent 2 Only", 5);

  vector<RuntimeProfile*> children;
  merged_profile->GetChildren(&children);
  EXPECT_EQ(children.size(), 3);

  for (int i = 0; i < 3; ++i) {
    RuntimeProfile* profile = children[i];
    if (profile->name().compare("Child1") == 0) {
      EXPECT_EQ(profile->num_counters(), 4);
      ValidateCounter(profile, "Child1 Shared", 30);
      ValidateCounter(profile, "Child1 Parent 1 Only", 50);
      ValidateCounter(profile, "Child1 Parent 2 Only", 100);
    } else if (profile->name().compare("Child2") == 0) {
      EXPECT_EQ(profile->num_counters(), 2);
      ValidateCounter(profile, "Child2", 40);
    } else if (profile->name().compare("Child3") == 0) {
      EXPECT_EQ(profile->num_counters(), 2);
      ValidateCounter(profile, "Child3", 30);
    } else {
      EXPECT_TRUE(false);
    }
  }

  // make sure we can print
  stringstream dummy;
  merged_profile->PrettyPrint(&dummy);

  // Update profile2 w/ profile1 and validate
  profile2.Update(tprofile1);
  EXPECT_EQ(profile2.num_counters(), 4);
  ValidateCounter(&profile2, "Parent Shared", 1);
  ValidateCounter(&profile2, "Parent 1 Only", 2);
  ValidateCounter(&profile2, "Parent 2 Only", 5);

  profile2.GetChildren(&children);
  EXPECT_EQ(children.size(), 3);

  for (int i = 0; i < 3; ++i) {
    RuntimeProfile* profile = children[i];
    if (profile->name().compare("Child1") == 0) {
      EXPECT_EQ(profile->num_counters(), 4);
      ValidateCounter(profile, "Child1 Shared", 10);
      ValidateCounter(profile, "Child1 Parent 1 Only", 50);
      ValidateCounter(profile, "Child1 Parent 2 Only", 100);
    } else if (profile->name().compare("Child2") == 0) {
      EXPECT_EQ(profile->num_counters(), 2);
      ValidateCounter(profile, "Child2", 40);
    } else if (profile->name().compare("Child3") == 0) {
      EXPECT_EQ(profile->num_counters(), 2);
      ValidateCounter(profile, "Child3", 30);
    } else {
      EXPECT_TRUE(false);
    }
  }

  // make sure we can print
  profile2.PrettyPrint(&dummy);
}

TEST(CountersTest, DerivedCounters) {
  ObjectPool pool;
  RuntimeProfile profile(&pool, "Profile");
  RuntimeProfile::Counter* bytes_counter =
      profile.AddCounter("bytes", TCounterType::BYTES);
  RuntimeProfile::Counter* ticks_counter =
      profile.AddCounter("ticks", TCounterType::CPU_TICKS);
  // set to 1 sec
  ticks_counter->Set(CpuInfo::cycles_per_ms() * 1000);

  RuntimeProfile::DerivedCounter* throughput_counter =
      profile.AddDerivedCounter("throughput", TCounterType::BYTES,
      bind<int64_t>(&RuntimeProfile::UnitsPerSecond, bytes_counter, ticks_counter));

  bytes_counter->Set(10);
  EXPECT_EQ(throughput_counter->value(), 10);
  bytes_counter->Set(20);
  EXPECT_EQ(throughput_counter->value(), 20);
  ticks_counter->Set(ticks_counter->value() / 2);
  EXPECT_EQ(throughput_counter->value(), 40);
}

TEST(CountersTest, InfoStringTest) {
  ObjectPool pool;
  RuntimeProfile profile(&pool, "Profile");
  EXPECT_TRUE(profile.GetInfoString("Key") == NULL);

  profile.AddInfoString("Key", "Value");
  const string* value = profile.GetInfoString("Key");
  EXPECT_TRUE(value != NULL);
  EXPECT_EQ(*value, "Value");
  
  // Convert it to thrift
  TRuntimeProfileTree tprofile;
  profile.ToThrift(&tprofile);

  // Convert it back
  RuntimeProfile* from_thrift = RuntimeProfile::CreateFromThrift(
      &pool, tprofile);
  value = from_thrift->GetInfoString("Key");
  EXPECT_TRUE(value != NULL);
  EXPECT_EQ(*value, "Value");

  // Test update.
  RuntimeProfile update_dst_profile(&pool, "Profile2");
  update_dst_profile.Update(tprofile);
  value = update_dst_profile.GetInfoString("Key");
  EXPECT_TRUE(value != NULL);
  EXPECT_EQ(*value, "Value");

  // Update the original profile, convert it to thrift and update from the dst
  // profile
  profile.AddInfoString("Key", "NewValue");
  profile.AddInfoString("Foo", "Bar");
  EXPECT_EQ(*profile.GetInfoString("Key"), "NewValue");
  EXPECT_EQ(*profile.GetInfoString("Foo"), "Bar");
  profile.ToThrift(&tprofile);

  update_dst_profile.Update(tprofile);
  EXPECT_EQ(*update_dst_profile.GetInfoString("Key"), "NewValue");
  EXPECT_EQ(*update_dst_profile.GetInfoString("Foo"), "Bar");
}

TEST(CountersTest, RateCounters) {
  ObjectPool pool;
  RuntimeProfile profile(&pool, "Profile");
  
  RuntimeProfile::Counter* bytes_counter =
      profile.AddCounter("bytes", TCounterType::BYTES);

  RuntimeProfile::Counter* rate_counter =
      profile.AddRateCounter("RateCounter", bytes_counter);
  EXPECT_TRUE(rate_counter->type() == TCounterType::BYTES_PER_SECOND);

  EXPECT_EQ(rate_counter->value(), 0);
  // set to 100MB.  Use bigger units to avoid truncating to 0 after divides.
  bytes_counter->Set(100 * 1024 * 1024);  

  // Wait one second.  
  sleep(1);

  int64_t rate = rate_counter->value();

  // Remove the counter so it no longer gets updates
  profile.StopRateCounterUpdates(rate_counter);

  // The rate counter is not perfectly accurate.  Currently updated at 500ms intervals,
  // we should have seen somewhere between 1 and 3 updates (33 - 200 MB/s)
  EXPECT_GT(rate, 66 * 1024 * 1024);
  EXPECT_LE(rate, 200 * 1024 * 1024);

  // Wait another second.  The counter has been removed. So the value should not be
  // changed (much).
  sleep(2);
  
  rate = rate_counter->value();
  EXPECT_GT(rate, 66 * 1024 * 1024);
  EXPECT_LE(rate, 200 * 1024 * 1024);
}

}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  impala::CpuInfo::Init();
  return RUN_ALL_TESTS();
}

