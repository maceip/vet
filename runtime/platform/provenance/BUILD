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
    name = "merkle_dag_store",
    hdrs = ["merkle_dag_store.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
    ],
)

cc_library(
    name = "local_merkle_dag_store",
    srcs = ["local_merkle_dag_store.cc"],
    hdrs = ["local_merkle_dag_store.h"],
    deps = [
        ":merkle_dag_store",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "provenance_query",
    srcs = ["provenance_query.cc"],
    hdrs = ["provenance_query.h"],
    deps = [
        ":merkle_dag_store",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_test(
    name = "provenance_test",
    srcs = ["provenance_test.cc"],
    deps = [
        ":local_merkle_dag_store",
        ":merkle_dag_store",
        ":provenance_query",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:test_utils",
    ],
)
