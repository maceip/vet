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
    default_visibility = ["//visibility:public"],
)

licenses(["notice"])

cc_library(
    name = "audit_certificate",
    srcs = ["audit_certificate.cc"],
    hdrs = ["audit_certificate.h"],
    deps = [
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "audit_ledger",
    hdrs = ["audit_ledger.h"],
    deps = [
        ":audit_certificate",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
    ],
)

cc_library(
    name = "audit_certificate_signer",
    srcs = ["audit_certificate_signer.cc"],
    hdrs = ["audit_certificate_signer.h"],
    deps = [
        ":audit_certificate",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "liboqs_audit_certificate_signer",
    srcs = ["liboqs_audit_certificate_signer.cc"],
    hdrs = ["liboqs_audit_certificate_signer.h"],
    linkopts = select({
        "@platforms//os:windows": [],
        "//conditions:default": ["-ldl"],
    }),
    deps = [
        ":audit_certificate",
        ":audit_certificate_signer",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "audit_query",
    srcs = ["audit_query.cc"],
    hdrs = ["audit_query.h"],
    deps = [
        ":audit_certificate",
        ":audit_ledger",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_library(
    name = "local_filesystem_audit_ledger",
    srcs = ["local_filesystem_audit_ledger.cc"],
    hdrs = ["local_filesystem_audit_ledger.h"],
    deps = [
        ":audit_certificate",
        ":audit_ledger",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/checkpoint:durable_writer",
        "//runtime/platform/hash:hasher",
        "//runtime/util:litert_status_util",
    ],
)

cc_test(
    name = "audit_certificate_signer_test",
    srcs = ["audit_certificate_signer_test.cc"],
    deps = [
        ":audit_certificate",
        ":audit_certificate_signer",
        ":local_filesystem_audit_ledger",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "liboqs_audit_certificate_signer_test",
    srcs = ["liboqs_audit_certificate_signer_test.cc"],
    deps = [
        ":liboqs_audit_certificate_signer",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "audit_query_test",
    srcs = ["audit_query_test.cc"],
    deps = [
        ":audit_certificate",
        ":audit_query",
        ":local_filesystem_audit_ledger",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:test_utils",
    ],
)

cc_test(
    name = "local_filesystem_audit_ledger_test",
    srcs = ["local_filesystem_audit_ledger_test.cc"],
    deps = [
        ":audit_certificate",
        ":local_filesystem_audit_ledger",
        "@com_google_googletest//:gtest_main",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:string_view",
        "//runtime/platform/hash:hasher",
        "//runtime/util:test_utils",
    ],
)
