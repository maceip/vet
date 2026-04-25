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
        "dpm_projector.cc",
        "engine_inference_runner.cc",
        "event.cc",
        "event_sourced_log.cc",
        "projection_prompt.cc",
        "stateless_decision_engine.cc",
    ],
    hdrs = [
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
