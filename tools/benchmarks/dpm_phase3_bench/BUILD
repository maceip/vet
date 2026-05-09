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

# [Google-internal load of `cc_test`]

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

cc_test(
    name = "phase3_substrate_smoke",
    srcs = ["phase3_substrate_smoke.cc"],
    deps = [
        "//runtime/dpm:audited_checkpoint_loader",
        "//runtime/dpm:checkpointed_projection",
        "//runtime/dpm:correction_protocol",
        "//runtime/dpm:dpm",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/audit:audit_certificate",
        "//runtime/platform/audit:local_filesystem_audit_ledger",
        "//runtime/platform/checkpoint:local_filesystem_checkpoint_store",
        "//runtime/platform/hash:hasher",
        "//runtime/platform/provenance:local_merkle_dag_store",
        "//runtime/util:test_utils",
    ],
)

