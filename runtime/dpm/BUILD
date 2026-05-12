# Copyright 2026 Google LLC.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# [Google-internal load of `cc_library`]
# [Google-internal load of `cc_test`]

package(
    default_hdrs_check = "strict",
    default_visibility = [
        "//visibility:public",
    ],
)

licenses(["notice"])

cc_library(
    name = "dpm",
    srcs = [
        "active_evidence_view.cc",
        "dpm_projector.cc",
        "engine_inference_runner.cc",
        "event.cc",
        "event_sourced_log.cc",
        "projection_prompt.cc",
        "stateless_decision_engine.cc",
    ],
    hdrs = [
        "active_evidence_view.h",
        "dpm_projector.h",
        "engine_inference_runner.h",
        "event.h",
        "event_sourced_log.h",
        "projection_prompt.h",
        "stateless_decision_engine.h",
    ],
    deps = [
        "@com_google_absl//absl/hash",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_absl//absl/time",
        "@nlohmann_json//:json",
        "//runtime/engine:engine_interface",
        "//runtime/engine:engine_settings",
        "//runtime/engine:io_types",
        "//runtime/platform/eventlog:event_sink",
        "//runtime/platform/eventlog:posix_event_sink",
        "//runtime/proto:sampler_params_cc_proto",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "projection_comparator",
    srcs = ["projection_comparator.cc"],
    hdrs = ["projection_comparator.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@nlohmann_json//:json",
    ],
)

cc_library(
    name = "audited_checkpoint_loader",
    srcs = ["audited_checkpoint_loader.cc"],
    hdrs = ["audited_checkpoint_loader.h"],
    deps = [
        ":checkpoint_decision_gate",
        ":checkpointed_projection",
        ":correction_protocol",
        ":dpm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "//runtime/platform/audit:audit_certificate_signer",
        "//runtime/platform/audit:audit_ledger",
        "//runtime/platform/checkpoint:checkpoint_store",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "checkpoint_policy",
    srcs = ["checkpoint_policy.cc"],
    hdrs = ["checkpoint_policy.h"],
    deps = [
        ":dpm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/checkpoint:kv_quantization",
        "//runtime/platform/hash:hasher",
        "//runtime/proto:checkpoint_cc_proto",
    ],
)

cc_library(
    name = "checkpointed_projection",
    srcs = ["checkpointed_projection.cc"],
    hdrs = ["checkpointed_projection.h"],
    deps = [
        ":dpm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "//runtime/platform/checkpoint:canonical_manifest",
        "//runtime/platform/checkpoint:checkpoint_store",
        "//runtime/platform/checkpoint:rollup_manifest",
        "//runtime/platform/hash:hasher",
        "//runtime/platform/provenance:merkle_dag_store",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "correction_protocol",
    srcs = ["correction_protocol.cc"],
    hdrs = ["correction_protocol.h"],
    deps = [
        ":dpm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@nlohmann_json//:json",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "checkpoint_decision_gate",
    srcs = ["checkpoint_decision_gate.cc"],
    hdrs = ["checkpoint_decision_gate.h"],
    deps = [
        ":correction_protocol",
        ":dpm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "//runtime/platform/audit:audit_certificate",
        "//runtime/platform/audit:audit_certificate_signer",
        "//runtime/platform/audit:audit_ledger",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "projection_replay_auditor",
    srcs = ["projection_replay_auditor.cc"],
    hdrs = ["projection_replay_auditor.h"],
    deps = [
        ":checkpointed_projection",
        ":correction_protocol",
        ":dpm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "//runtime/platform/audit:audit_certificate",
        "//runtime/platform/audit:audit_ledger",
        "//runtime/platform/checkpoint:canonical_manifest",
        "//runtime/platform/checkpoint:checkpoint_store",
        "//runtime/platform/hash:hasher",
        "//runtime/platform/provenance:merkle_dag_store",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "projection_prefix_cache",
    srcs = ["projection_prefix_cache.cc"],
    hdrs = ["projection_prefix_cache.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_test(
    name = "projection_comparator_test",
    srcs = ["projection_comparator_test.cc"],
    deps = [
        ":projection_comparator",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "audited_checkpoint_loader_test",
    srcs = ["audited_checkpoint_loader_test.cc"],
    deps = [
        ":audited_checkpoint_loader",
        ":checkpointed_projection",
        ":correction_protocol",
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/audit:audit_certificate",
        "//runtime/platform/audit:audit_certificate_signer",
        "//runtime/platform/audit:local_filesystem_audit_ledger",
        "//runtime/platform/checkpoint:local_filesystem_checkpoint_store",
        "//runtime/platform/hash:hasher",
        "//runtime/platform/provenance:local_merkle_dag_store",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "projection_prefix_cache_test",
    srcs = ["projection_prefix_cache_test.cc"],
    deps = [
        ":dpm",
        ":projection_prefix_cache",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "checkpoint_policy_test",
    srcs = ["checkpoint_policy_test.cc"],
    deps = [
        ":checkpoint_policy",
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "//runtime/platform/checkpoint:kv_quantization",
        "//runtime/platform/hash:hasher",
        "//runtime/proto:checkpoint_cc_proto",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "checkpointed_projection_test",
    srcs = ["checkpointed_projection_test.cc"],
    deps = [
        ":checkpointed_projection",
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/checkpoint:checkpoint_store",
        "//runtime/platform/checkpoint:local_filesystem_checkpoint_store",
        "//runtime/platform/hash:hasher",
        "//runtime/platform/provenance:local_merkle_dag_store",
        "//runtime/platform/provenance:provenance_query",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "phase3_audit_flow_test",
    srcs = ["phase3_audit_flow_test.cc"],
    deps = [
        ":checkpoint_decision_gate",
        ":checkpointed_projection",
        ":correction_protocol",
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/audit:audit_certificate",
        "//runtime/platform/audit:audit_certificate_signer",
        "//runtime/platform/audit:local_filesystem_audit_ledger",
        "//runtime/platform/checkpoint:local_filesystem_checkpoint_store",
        "//runtime/platform/hash:hasher",
        "//runtime/platform/provenance:local_merkle_dag_store",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "projection_replay_auditor_test",
    srcs = ["projection_replay_auditor_test.cc"],
    deps = [
        ":checkpoint_decision_gate",
        ":checkpointed_projection",
        ":correction_protocol",
        ":dpm",
        ":projection_replay_auditor",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/audit:audit_certificate",
        "//runtime/platform/audit:local_filesystem_audit_ledger",
        "//runtime/platform/checkpoint:local_filesystem_checkpoint_store",
        "//runtime/platform/hash:hasher",
        "//runtime/platform/provenance:local_merkle_dag_store",
        "//runtime/platform/provenance:provenance_query",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "event_sourced_log_test",
    srcs = ["event_sourced_log_test.cc"],
    deps = [
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/functional:function_ref",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/eventlog:event_sink",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "active_evidence_view_test",
    srcs = ["active_evidence_view_test.cc"],
    deps = [
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "dpm_projector_test",
    srcs = ["dpm_projector_test.cc"],
    deps = [
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "projection_prompt_test",
    srcs = ["projection_prompt_test.cc"],
    deps = [
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "engine_inference_runner_test",
    srcs = ["engine_inference_runner_test.cc"],
    deps = [
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/log:absl_log",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_absl//absl/time",
        "//runtime/components:tokenizer",
        "//runtime/engine:engine_interface",
        "//runtime/engine:engine_settings",
        "//runtime/engine:io_types",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "dpm_determinism_e2e_test",
    srcs = ["dpm_determinism_e2e_test.cc"],
    tags = [
        "manual",
        "requires-model-artifact",
    ],
    deps = [
        ":dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/engine:engine_factory",
        "//runtime/engine:engine_impl_selected",
        "//runtime/engine:engine_interface",
        "//runtime/engine:engine_settings",
        "//runtime/executor:executor_settings_base",
        "//runtime/util:litert_status_util",
        "//runtime/util:test_utils",
    ],
)
