// Copyright 2026 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/dpm/checkpoint_policy.h"

#include <cstdint>
#include <string>
#include <vector>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "runtime/dpm/event.h"
#include "runtime/platform/checkpoint/kv_quantization.h"
#include "runtime/platform/hash/hasher.h"
#include "runtime/proto/checkpoint.pb.h"
#include "runtime/util/test_utils.h"

namespace litert::lm {
namespace {

using ::testing::HasSubstr;

void StampHash(proto::Hash256* dst, const Hash256& src) {
  dst->set_bytes(src.bytes.data(), src.bytes.size());
  dst->set_algorithm(proto::HASH_BLAKE3);
}

proto::CheckpointAbi ManifestForArm64Gqa(const Hash256& artifact_hash) {
  proto::CheckpointAbi abi;
  abi.mutable_identity()->set_tenant_id("tenant-a");
  abi.mutable_identity()->set_session_id("session-1");
  abi.mutable_identity()->set_branch_id("main");
  abi.mutable_level()->set_level(1);
  abi.mutable_producer()->set_architecture_tag("arm64-hexagon-int8");
  abi.mutable_model()->set_model_id("gemma-3-test");
  StampHash(abi.mutable_model()->mutable_artifact_hash(), artifact_hash);
  abi.mutable_model()->set_model_class(proto::MODEL_CLASS_GQA);
  abi.mutable_model()->set_num_layers(24);
  abi.mutable_model()->set_num_kv_heads(8);
  abi.mutable_model()->set_head_dim(128);
  abi.set_kv_dtype(proto::KV_DTYPE_FP16);  // replay-safe
  abi.set_body_size_bytes(64 * 1024);
  return abi;
}

ThawRequest LocalArm64Request(const Hash256& artifact_hash) {
  ThawRequest r;
  r.tenant_id = "tenant-a";
  r.session_id = "session-1";
  r.branch_id = "main";
  r.model_id = "gemma-3-test";
  r.model_artifact_hash = artifact_hash;
  r.model_class = proto::MODEL_CLASS_GQA;
  r.architecture_tag = "arm64-hexagon-int8";
  // Default kv_policy = require_replay_safe.
  return r;
}

TEST(CompatibilityTest, MatchingFieldsAllowThaw) {
  Hash256 model = HashBytes(HashAlgorithm::kBlake3, "model-bytes");
  proto::CheckpointAbi abi = ManifestForArm64Gqa(model);
  ThawRequest req = LocalArm64Request(model);
  ThawDecision d = EvaluateCheckpointCompatibility(abi, req);
  EXPECT_TRUE(d.can_thaw);
  EXPECT_FALSE(d.must_refill_from_log);
}

TEST(CompatibilityTest, ArchitectureMismatchSoftFalls) {
  Hash256 model = HashBytes(HashAlgorithm::kBlake3, "m");
  proto::CheckpointAbi abi = ManifestForArm64Gqa(model);
  ThawRequest req = LocalArm64Request(model);
  req.architecture_tag = "x86_64-cuda-fp16";
  ThawDecision d = EvaluateCheckpointCompatibility(abi, req);
  EXPECT_FALSE(d.can_thaw);
  EXPECT_TRUE(d.must_refill_from_log);
  EXPECT_THAT(d.reason, HasSubstr("architecture"));
}

TEST(CompatibilityTest, ModelArtifactMismatchSoftFalls) {
  Hash256 model = HashBytes(HashAlgorithm::kBlake3, "m");
  proto::CheckpointAbi abi = ManifestForArm64Gqa(model);
  ThawRequest req = LocalArm64Request(
      HashBytes(HashAlgorithm::kBlake3, "different-model"));
  ThawDecision d = EvaluateCheckpointCompatibility(abi, req);
  EXPECT_FALSE(d.can_thaw);
  EXPECT_THAT(d.reason, HasSubstr("artifact hash"));
}

TEST(CompatibilityTest, ReplaySafePolicyRejectsInt8Manifest) {
  Hash256 model = HashBytes(HashAlgorithm::kBlake3, "m");
  proto::CheckpointAbi abi = ManifestForArm64Gqa(model);
  abi.set_kv_dtype(proto::KV_DTYPE_INT8_PER_TOKEN);
  ThawRequest req = LocalArm64Request(model);
  // Default kv_policy.require_replay_safe = true.
  ThawDecision d = EvaluateCheckpointCompatibility(abi, req);
  EXPECT_FALSE(d.can_thaw);
  EXPECT_TRUE(d.must_refill_from_log);
  EXPECT_THAT(d.reason, HasSubstr("replay-safe"));
}

TEST(CompatibilityTest, OptInPolicyAcceptsApprovedInt8) {
  Hash256 model = HashBytes(HashAlgorithm::kBlake3, "m");
  proto::CheckpointAbi abi = ManifestForArm64Gqa(model);
  abi.set_kv_dtype(proto::KV_DTYPE_INT8_PER_TOKEN);
  ThawRequest req = LocalArm64Request(model);
  req.kv_policy.require_replay_safe = false;
  req.kv_policy.approved_dtype = KvDtype::kInt8PerToken;
  ThawDecision d = EvaluateCheckpointCompatibility(abi, req);
  EXPECT_TRUE(d.can_thaw);
}

TEST(ThawVerificationTest, MatchingDigestApproves) {
  Hash256 h = HashBytes(HashAlgorithm::kBlake3, "x");
  ThawDecision d = EvaluateCheckpointThawVerification(h, h);
  EXPECT_TRUE(d.can_thaw);
}

TEST(ThawVerificationTest, MismatchSoftFalls) {
  Hash256 a = HashBytes(HashAlgorithm::kBlake3, "x");
  Hash256 b = HashBytes(HashAlgorithm::kBlake3, "y");
  ThawDecision d = EvaluateCheckpointThawVerification(a, b);
  EXPECT_FALSE(d.can_thaw);
  EXPECT_TRUE(d.must_refill_from_log);
  EXPECT_THAT(d.reason, HasSubstr("digest mismatch"));
}

TEST(ThawVerificationTest, ZeroHashFails) {
  Hash256 zero;
  Hash256 nonzero = HashBytes(HashAlgorithm::kBlake3, "x");
  EXPECT_FALSE(EvaluateCheckpointThawVerification(zero, nonzero).can_thaw);
  EXPECT_FALSE(EvaluateCheckpointThawVerification(nonzero, zero).can_thaw);
}

TEST(TriggerTest, HandoffWins) {
  CheckpointTriggerPolicy policy;
  CheckpointTriggerState state;
  state.explicit_handoff = true;
  CheckpointTriggerDecision d = ShouldCreateCheckpoint(policy, state);
  EXPECT_TRUE(d.should_checkpoint);
  EXPECT_FALSE(d.speculative);
  EXPECT_EQ(d.trigger, proto::TRIGGER_HANDOFF);
}

TEST(TriggerTest, MaxTokenDeltaWins) {
  CheckpointTriggerPolicy policy;
  CheckpointTriggerState state;
  state.tokens_since_checkpoint = 5000;
  CheckpointTriggerDecision d = ShouldCreateCheckpoint(policy, state);
  EXPECT_TRUE(d.should_checkpoint);
  EXPECT_EQ(d.trigger, proto::TRIGGER_TOKEN_THRESHOLD);
}

TEST(TriggerTest, ContextPressureWinsOverTool) {
  CheckpointTriggerPolicy policy;
  CheckpointTriggerState state;
  state.current_context_tokens = 7600;
  state.max_context_tokens = 8192;  // 92.7% > 75%
  state.last_event_was_milestone_tool = true;
  state.tokens_since_checkpoint = 3000;
  CheckpointTriggerDecision d = ShouldCreateCheckpoint(policy, state);
  EXPECT_TRUE(d.should_checkpoint);
  EXPECT_EQ(d.trigger, proto::TRIGGER_TOKEN_THRESHOLD);
}

TEST(TriggerTest, IdleIsSpeculative) {
  CheckpointTriggerPolicy policy;
  CheckpointTriggerState state;
  state.user_idle = true;
  state.tokens_since_checkpoint = 100;
  CheckpointTriggerDecision d = ShouldCreateCheckpoint(policy, state);
  EXPECT_TRUE(d.should_checkpoint);
  EXPECT_TRUE(d.speculative);
  EXPECT_EQ(d.trigger, proto::TRIGGER_IDLE_SPECULATIVE);
}

TEST(TriggerTest, NothingTriggersBelowThresholds) {
  CheckpointTriggerPolicy policy;
  CheckpointTriggerState state;
  state.tokens_since_checkpoint = 10;
  EXPECT_FALSE(ShouldCreateCheckpoint(policy, state).should_checkpoint);
}

TEST(TransportTierTest, RackLocalRdmaPicksRoCe) {
  CheckpointTransportRequest r;
  r.rack_local = true;
  r.rdma_available = true;
  EXPECT_EQ(SelectCheckpointTransportTier(r),
            proto::TRANSPORT_TIER_RDMA_ROCE);
}

TEST(TransportTierTest, CrossAzFallsToGrpc) {
  CheckpointTransportRequest r;
  r.rack_local = true;
  r.rdma_available = true;
  r.cross_az_or_region = true;
  EXPECT_EQ(SelectCheckpointTransportTier(r),
            proto::TRANSPORT_TIER_GRPC_FLATBUFFERS);
}

TEST(CompactionTest, LevelCapTriggers) {
  std::vector<proto::CheckpointAbi> chain(9);
  for (int i = 0; i < 9; ++i) {
    chain[i].mutable_level()->set_level(i + 1);  // all deltas
    chain[i].set_body_size_bytes(1024);
  }
  CheckpointCompactionPolicy policy;
  policy.max_delta_levels = 8;
  EXPECT_TRUE(ShouldCompactCheckpointDeltas(chain, policy));
}

TEST(CompactionTest, ByteCapTriggers) {
  std::vector<proto::CheckpointAbi> chain(2);
  for (auto& a : chain) {
    a.mutable_level()->set_level(1);
    a.set_body_size_bytes(80 * 1024 * 1024);
  }
  CheckpointCompactionPolicy policy;
  policy.max_delta_levels = 100;
  policy.max_delta_bytes = 128 * 1024 * 1024;
  EXPECT_TRUE(ShouldCompactCheckpointDeltas(chain, policy));
}

TEST(CompactionTest, Level0EntriesIgnored) {
  std::vector<proto::CheckpointAbi> chain(2);
  chain[0].mutable_level()->set_level(0);
  chain[0].set_body_size_bytes(1024);
  chain[1].mutable_level()->set_level(0);
  chain[1].set_body_size_bytes(1024);
  CheckpointCompactionPolicy policy;
  policy.max_delta_levels = 1;
  EXPECT_FALSE(ShouldCompactCheckpointDeltas(chain, policy));
}

TEST(StorageTierTest, ValidatesMetadataMustBeMemoryDb) {
  proto::CheckpointStorageBinding binding;
  binding.set_metadata_tier(proto::STORAGE_TIER_MEMORYDB_METADATA);
  binding.set_blob_tier(proto::STORAGE_TIER_S3_EXPRESS_ONE_ZONE);
  binding.set_metadata_uri("memorydb://...");
  binding.set_blob_uri("s3express://...");
  EXPECT_OK(ValidateCheckpointStorageTiers(binding));

  binding.set_metadata_tier(proto::STORAGE_TIER_LOCAL_FILE);
  EXPECT_FALSE(ValidateCheckpointStorageTiers(binding).ok());
}

TEST(StorageTierTest, RejectsBlobInMemoryDb) {
  proto::CheckpointStorageBinding binding;
  binding.set_metadata_tier(proto::STORAGE_TIER_MEMORYDB_METADATA);
  binding.set_blob_tier(proto::STORAGE_TIER_MEMORYDB_METADATA);
  binding.set_metadata_uri("memorydb://...");
  binding.set_blob_uri("memorydb://...");
  EXPECT_FALSE(ValidateCheckpointStorageTiers(binding).ok());
}

TEST(StorageTierTest, RejectsEmptyUris) {
  proto::CheckpointStorageBinding binding;
  binding.set_metadata_tier(proto::STORAGE_TIER_MEMORYDB_METADATA);
  binding.set_blob_tier(proto::STORAGE_TIER_S3_EXPRESS_ONE_ZONE);
  binding.set_blob_uri("s3express://...");
  EXPECT_FALSE(ValidateCheckpointStorageTiers(binding).ok());
}

}  // namespace
}  // namespace litert::lm
