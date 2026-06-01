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

# [Google-internal load of `cc_binary`]
# [Google-internal load of `cc_library`]
# [Google-internal load of `cc_test`]

package(default_visibility = ["//visibility:public"])

licenses(["notice"])

cc_library(
    name = "vet_aid",
    srcs = ["vet_aid.cc"],
    hdrs = ["vet_aid.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@nlohmann_json//:json",
        "//runtime/platform/hash:hasher",
    ],
)

cc_library(
    name = "vet_trace",
    srcs = ["vet_trace.cc"],
    hdrs = ["vet_trace.h"],
    deps = [
        ":vet_aid",
        "//runtime/dpm",
        "//runtime/platform/hash:hasher",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@nlohmann_json//:json",
    ],
)

cc_binary(
    name = "vet",
    srcs = ["vet_main.cc"],
    deps = [
        ":vet_aid",
        ":vet_trace",
        "//runtime/dpm",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@nlohmann_json//:json",
    ],
)

cc_test(
    name = "vet_stage2_test",
    # Name kept for build-label stability; tests cover handoff bundles and verify.
    srcs = ["vet_stage2_test.cc"],
    deps = [
        ":vet_aid",
        ":vet_trace",
        "//runtime/dpm",
        "//runtime/util:test_utils",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
    ],
)
