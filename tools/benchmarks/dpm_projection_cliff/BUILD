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
    name = "dpm_projection_cliff_lib",
    srcs = ["dpm_projection_cliff.cc"],
    hdrs = ["dpm_projection_cliff.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_binary(
    name = "dpm_projection_cliff",
    srcs = ["dpm_projection_cliff_main.cc"],
    deps = [
        ":dpm_projection_cliff_lib",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "dpm_projection_cliff_test",
    srcs = ["dpm_projection_cliff_test.cc"],
    deps = [
        ":dpm_projection_cliff_lib",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
    ],
)
