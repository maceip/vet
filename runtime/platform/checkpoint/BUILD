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
    name = "kv_quantization",
    srcs = ["kv_quantization.cc"],
    hdrs = ["kv_quantization.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/types:span",
    ],
)

cc_library(
    name = "durable_writer",
    srcs = ["durable_writer.cc"],
    hdrs = ["durable_writer.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_test(
    name = "durable_writer_test",
    srcs = ["durable_writer_test.cc"],
    deps = [
        ":durable_writer",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/util:test_utils",
    ],
)

cc_library(
    name = "canonical_manifest",
    srcs = ["canonical_manifest.cc"],
    hdrs = ["canonical_manifest.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
    ],
)

cc_test(
    name = "canonical_manifest_test",
    srcs = ["canonical_manifest_test.cc"],
    deps = [
        ":canonical_manifest",
        "@com_google_googletest//:gtest_main",
        "//runtime/platform/hash:hasher",
        "//runtime/util:test_utils",
    ],
)

cc_library(
    name = "checkpoint_store",
    hdrs = ["checkpoint_store.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
    ],
)

cc_library(
    name = "local_filesystem_checkpoint_store",
    srcs = ["local_filesystem_checkpoint_store.cc"],
    hdrs = ["local_filesystem_checkpoint_store.h"],
    deps = [
        ":checkpoint_store",
        ":durable_writer",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_test(
    name = "local_filesystem_checkpoint_store_test",
    srcs = ["local_filesystem_checkpoint_store_test.cc"],
    deps = [
        ":local_filesystem_checkpoint_store",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:test_utils",
    ],
)

cc_library(
    name = "checkpoint_codec",
    srcs = ["checkpoint_codec.cc"],
    hdrs = ["checkpoint_codec.h"],
    deps = [
        ":kv_quantization",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_test(
    name = "checkpoint_codec_test",
    srcs = ["checkpoint_codec_test.cc"],
    deps = [
        ":checkpoint_codec",
        ":kv_quantization",
        "@com_google_googletest//:gtest_main",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "kv_quantization_test",
    srcs = ["kv_quantization_test.cc"],
    deps = [
        ":kv_quantization",
        "@com_google_googletest//:gtest_main",
        "//runtime/util:test_utils",
    ],
)
