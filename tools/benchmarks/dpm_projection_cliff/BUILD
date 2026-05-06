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
    name = "cliff_trajectory",
    srcs = ["cliff_trajectory.cc"],
    hdrs = ["cliff_trajectory.h"],
    deps = [
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_library(
    name = "cliff_provenance",
    srcs = ["cliff_provenance.cc"],
    hdrs = ["cliff_provenance.h"],
    deps = [
        "//runtime/platform/hash:hasher",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_library(
    name = "cliff_corpus",
    srcs = ["cliff_corpus.cc"],
    hdrs = ["cliff_corpus.h"],
    deps = [
        ":cliff_handoff",
        ":cliff_trajectory",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_library(
    name = "cliff_handoff",
    srcs = ["cliff_handoff.cc"],
    hdrs = ["cliff_handoff.h"],
    deps = [
        "//runtime/dpm",
        "//runtime/proto:checkpoint_cc_proto",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
    ],
)

cc_library(
    name = "dpm_projection_cliff_lib",
    srcs = ["dpm_projection_cliff.cc"],
    hdrs = ["dpm_projection_cliff.h"],
    additional_linker_inputs = select({
        "@platforms//os:windows": [
            "@litert//litert/c:windows_exported_symbols.def",
        ],
        "@platforms//os:linux": ["//runtime/engine:litert_lm_main.exported_symbols"],
        "//conditions:default": [],
    }),
    linkopts = select({
        "@litert//litert:litert_link_capi_so": [],
        "@platforms//os:ios": ["-Wl,-exported_symbol,_LiteRt*"],
        "@platforms//os:macos": ["-Wl,-exported_symbol,_LiteRt*"],
        "@platforms//os:windows": [
            "/DEF:$(location @litert//litert/c:windows_exported_symbols.def)",
        ],
        "@platforms//os:linux": [
            "-Wl,--dynamic-list=$(location //runtime/engine:litert_lm_main.exported_symbols)",
        ],
        "//conditions:default": ["-Wl,--export-dynamic-symbol=LiteRt*"],
    }) + select({
        "@platforms//os:android": ["-lEGL", "-lGLESv3"],
        "//conditions:default": [],
    }),
    deps = [
        ":cliff_corpus",
        ":cliff_handoff",
        ":cliff_provenance",
        ":cliff_trajectory",
        "//runtime/components:tokenizer",
        "//runtime/conversation",
        "//runtime/conversation:io_types",
        "//runtime/dpm",
        "//runtime/engine:engine_factory",
        "//runtime/engine:engine_impl_selected",
        "//runtime/engine:engine_interface",
        "//runtime/engine:engine_settings",
        "//runtime/engine:io_types",
        "//runtime/executor:executor_settings_base",
        "//runtime/executor:llm_executor_settings",
        "//runtime/platform/checkpoint:checkpoint_store",
        "//runtime/platform/checkpoint:local_filesystem_checkpoint_store",
        "//runtime/platform/checkpoint:s3_express_checkpoint_store",
        "//runtime/platform/hash:hasher",
        "//runtime/proto:engine_cc_proto",
        "//runtime/util:litert_status_util",
        "@nlohmann_json//:json",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_absl//absl/time",
    ],
)

cc_binary(
    name = "dpm_projection_cliff",
    srcs = ["dpm_projection_cliff_main.cc"],
    deps = [
        ":cliff_corpus",
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

# ----- scenario / agent-session corpus loader and tests --------------

cc_library(
    name = "session_case_loader",
    srcs = ["scenario/session_case_loader.cc"],
    hdrs = ["scenario/session_case_loader.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@nlohmann_json//:json",
    ],
)

cc_test(
    name = "session_case_loader_test",
    srcs = ["scenario/session_case_loader_test.cc"],
    data = ["scenario/golden/seed_case.json"],
    deps = [
        ":session_case_loader",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "phase2_scenario_test",
    srcs = ["scenario/phase2_scenario_test.cc"],
    data = [
        "scenario/golden/seed_case.json",
        "agentic_qwen/golden/synthetic_seed_pair.json",
    ],
    deps = [
        ":session_case_loader",
        "//runtime/dpm:projection_prompt",
        "//runtime/platform/hash:hasher",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "@com_google_googletest//:gtest_main",
    ],
)
