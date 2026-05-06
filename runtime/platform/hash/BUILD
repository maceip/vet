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
    name = "hasher",
    srcs = [
        "blake3_hasher.cc",
        "hasher.cc",
        "sha256_hasher.cc",
    ],
    hdrs = [
        "blake3_hasher.h",
        "hasher.h",
        "sha256_hasher.h",
    ],
    deps = [
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_test(
    name = "hasher_test",
    srcs = ["hasher_test.cc"],
    deps = [
        ":hasher",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/util:test_utils",
    ],
)
