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
