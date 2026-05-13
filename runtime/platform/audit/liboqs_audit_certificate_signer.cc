// Copyright 2026 Google LLC.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "runtime/platform/audit/liboqs_audit_certificate_signer.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/status/status.h"  // from @com_google_absl
#include "absl/status/statusor.h"  // from @com_google_absl
#include "absl/strings/match.h"  // from @com_google_absl
#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/string_view.h"  // from @com_google_absl
#include "runtime/platform/audit/audit_certificate.h"
#include "runtime/platform/audit/audit_certificate_signer.h"
#include "runtime/util/status_macros.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace litert::lm {
namespace {

constexpr int kOqsSuccess = 0;

// The dynamic loader uses liboqs' public C ABI. The OQS_SIG layout is public in
// <oqs/sig.h>; only the length fields and opaque pointer identity are used here.
struct OQS_SIG {
  const char* method_name;
  const char* alg_version;
  uint8_t claimed_nist_level;
  bool euf_cma;
  bool suf_cma;
  bool sig_with_ctx_support;
  size_t length_public_key;
  size_t length_secret_key;
  size_t length_signature;
};

using OqsSigNew = OQS_SIG* (*)(const char*);
using OqsSigFree = void (*)(OQS_SIG*);
using OqsSigKeypair = int (*)(const OQS_SIG*, uint8_t*, uint8_t*);
using OqsSigSign = int (*)(const OQS_SIG*, uint8_t*, size_t*, const uint8_t*,
                           size_t, const uint8_t*);
using OqsSigVerify = int (*)(const OQS_SIG*, const uint8_t*, size_t,
                             const uint8_t*, size_t, const uint8_t*);
using OqsSigAlgIsEnabled = int (*)(const char*);
using OqsVersion = const char* (*)();

bool IsMlDsaAlgorithm(absl::string_view algorithm) {
  return algorithm == kAuditSignatureAlgorithmMlDsa44 ||
         algorithm == kAuditSignatureAlgorithmMlDsa65 ||
         algorithm == kAuditSignatureAlgorithmMlDsa87;
}

std::string DefaultLibOqsPath() {
#if defined(_WIN32)
  return "oqs.dll";
#elif defined(__APPLE__)
  return "liboqs.dylib";
#else
  return "liboqs.so";
#endif
}

class DynamicLibrary {
 public:
  static absl::StatusOr<std::unique_ptr<DynamicLibrary>> Open(
      absl::string_view library_path) {
    std::string path =
        library_path.empty() ? DefaultLibOqsPath() : std::string(library_path);
#if defined(_WIN32)
    HMODULE handle = LoadLibraryA(path.c_str());
    if (handle == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("failed to load liboqs shared library: ", path));
    }
#else
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
      return absl::FailedPreconditionError(absl::StrCat(
          "failed to load liboqs shared library: ", path, ": ", dlerror()));
    }
#endif
    return std::unique_ptr<DynamicLibrary>(new DynamicLibrary(handle));
  }

  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary& operator=(const DynamicLibrary&) = delete;

  ~DynamicLibrary() {
#if defined(_WIN32)
    if (handle_ != nullptr) FreeLibrary(handle_);
#else
    if (handle_ != nullptr) dlclose(handle_);
#endif
  }

  template <typename Fn>
  absl::StatusOr<Fn> Symbol(const char* name) const {
#if defined(_WIN32)
    FARPROC symbol = GetProcAddress(handle_, name);
#else
    void* symbol = dlsym(handle_, name);
#endif
    if (symbol == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("liboqs missing symbol: ", name));
    }
    return reinterpret_cast<Fn>(symbol);
  }

 private:
#if defined(_WIN32)
  explicit DynamicLibrary(HMODULE handle) : handle_(handle) {}
  HMODULE handle_;
#else
  explicit DynamicLibrary(void* handle) : handle_(handle) {}
  void* handle_;
#endif
};

class LibOqsApi {
 public:
  static absl::StatusOr<LibOqsApi> Load(absl::string_view library_path) {
    ASSIGN_OR_RETURN(std::unique_ptr<DynamicLibrary> library,
                     DynamicLibrary::Open(library_path));
    LibOqsApi api;
    api.library_ = std::move(library);
    ASSIGN_OR_RETURN(api.sig_new_, api.library_->Symbol<OqsSigNew>(
                                       "OQS_SIG_new"));
    ASSIGN_OR_RETURN(api.sig_free_, api.library_->Symbol<OqsSigFree>(
                                         "OQS_SIG_free"));
    ASSIGN_OR_RETURN(api.keypair_, api.library_->Symbol<OqsSigKeypair>(
                                         "OQS_SIG_keypair"));
    ASSIGN_OR_RETURN(api.sign_, api.library_->Symbol<OqsSigSign>(
                                      "OQS_SIG_sign"));
    ASSIGN_OR_RETURN(api.verify_, api.library_->Symbol<OqsSigVerify>(
                                        "OQS_SIG_verify"));
    ASSIGN_OR_RETURN(api.alg_is_enabled_,
                     api.library_->Symbol<OqsSigAlgIsEnabled>(
                         "OQS_SIG_alg_is_enabled"));
    ASSIGN_OR_RETURN(api.version_,
                     api.library_->Symbol<OqsVersion>("OQS_version"));
    const char* version = api.version_();
    if (version == nullptr || !IsSupportedLibOqsVersionForAudit(version)) {
      return absl::FailedPreconditionError(absl::StrCat(
          "unsupported liboqs version for audit certificate signatures: ",
          version == nullptr ? "<null>" : version));
    }
    return api;
  }

  absl::StatusOr<OQS_SIG*> NewSig(absl::string_view algorithm) const {
    if (!IsMlDsaAlgorithm(algorithm)) {
      return absl::InvalidArgumentError(
          absl::StrCat("unsupported audit signature algorithm: ", algorithm));
    }
    std::string method(algorithm);
    if (alg_is_enabled_(method.c_str()) != 1) {
      return absl::FailedPreconditionError(
          absl::StrCat("liboqs signature algorithm is not enabled: ",
                       algorithm));
    }
    OQS_SIG* sig = sig_new_(method.c_str());
    if (sig == nullptr) {
      return absl::FailedPreconditionError(
          absl::StrCat("liboqs failed to allocate signature algorithm: ",
                       algorithm));
    }
    return sig;
  }

  void FreeSig(OQS_SIG* sig) const { sig_free_(sig); }

  OqsSigKeypair keypair() const { return keypair_; }
  OqsSigSign sign() const { return sign_; }
  OqsSigVerify verify() const { return verify_; }

 private:
  std::unique_ptr<DynamicLibrary> library_;
  OqsSigNew sig_new_ = nullptr;
  OqsSigFree sig_free_ = nullptr;
  OqsSigKeypair keypair_ = nullptr;
  OqsSigSign sign_ = nullptr;
  OqsSigVerify verify_ = nullptr;
  OqsSigAlgIsEnabled alg_is_enabled_ = nullptr;
  OqsVersion version_ = nullptr;
};

struct OqsSigDeleter {
  const LibOqsApi* api = nullptr;
  void operator()(OQS_SIG* sig) const {
    if (api != nullptr && sig != nullptr) api->FreeSig(sig);
  }
};

absl::Status ValidateKeyPair(const LibOqsSignatureKeyPair& key_pair) {
  if (!IsMlDsaAlgorithm(key_pair.algorithm)) {
    return absl::InvalidArgumentError(
        absl::StrCat("unsupported audit signature algorithm: ",
                     key_pair.algorithm));
  }
  if (key_pair.key_id.empty() || key_pair.public_key.empty() ||
      key_pair.secret_key.empty()) {
    return absl::InvalidArgumentError(
        "liboqs signer requires key_id, public_key, and secret_key.");
  }
  return absl::OkStatus();
}

}  // namespace

bool IsSupportedLibOqsVersionForAudit(absl::string_view version) {
  return version == "0.15.0" || absl::StartsWith(version, "0.15.");
}

absl::StatusOr<LibOqsSignatureKeyPair> GenerateLibOqsMlDsaKeyPair(
    absl::string_view algorithm, absl::string_view key_id,
    absl::string_view library_path) {
  if (key_id.empty()) {
    return absl::InvalidArgumentError("liboqs key_id is required.");
  }
  ASSIGN_OR_RETURN(LibOqsApi api, LibOqsApi::Load(library_path));
  ASSIGN_OR_RETURN(OQS_SIG * raw_sig, api.NewSig(algorithm));
  std::unique_ptr<OQS_SIG, OqsSigDeleter> sig(raw_sig, OqsSigDeleter{&api});

  std::string public_key(sig->length_public_key, '\0');
  std::string secret_key(sig->length_secret_key, '\0');
  const int rc = api.keypair()(
      sig.get(), reinterpret_cast<uint8_t*>(public_key.data()),
      reinterpret_cast<uint8_t*>(secret_key.data()));
  if (rc != kOqsSuccess) {
    return absl::InternalError(
        absl::StrCat("liboqs keypair failed for ", algorithm));
  }
  return LibOqsSignatureKeyPair{
      .algorithm = std::string(algorithm),
      .key_id = std::string(key_id),
      .public_key = std::move(public_key),
      .secret_key = std::move(secret_key),
  };
}

LibOqsAuditCertificateSigner::LibOqsAuditCertificateSigner(
    LibOqsSignatureKeyPair key_pair, absl::string_view library_path)
    : key_pair_(std::move(key_pair)), library_path_(library_path) {}

absl::string_view LibOqsAuditCertificateSigner::Algorithm() const {
  return key_pair_.algorithm;
}

absl::string_view LibOqsAuditCertificateSigner::KeyId() const {
  return key_pair_.key_id;
}

absl::StatusOr<std::string> LibOqsAuditCertificateSigner::Sign(
    absl::string_view canonical_certificate) const {
  RETURN_IF_ERROR(ValidateKeyPair(key_pair_));
  ASSIGN_OR_RETURN(LibOqsApi api, LibOqsApi::Load(library_path_));
  ASSIGN_OR_RETURN(OQS_SIG * raw_sig, api.NewSig(key_pair_.algorithm));
  std::unique_ptr<OQS_SIG, OqsSigDeleter> sig(raw_sig, OqsSigDeleter{&api});
  if (key_pair_.secret_key.size() != sig->length_secret_key) {
    return absl::InvalidArgumentError(
        "liboqs signer secret key length does not match algorithm.");
  }

  std::string signature(sig->length_signature, '\0');
  size_t signature_len = signature.size();
  const int rc = api.sign()(
      sig.get(), reinterpret_cast<uint8_t*>(signature.data()), &signature_len,
      reinterpret_cast<const uint8_t*>(canonical_certificate.data()),
      canonical_certificate.size(),
      reinterpret_cast<const uint8_t*>(key_pair_.secret_key.data()));
  if (rc != kOqsSuccess) {
    return absl::InternalError(
        absl::StrCat("liboqs sign failed for ", key_pair_.algorithm));
  }
  signature.resize(signature_len);
  return signature;
}

LibOqsAuditCertificateVerifier::LibOqsAuditCertificateVerifier(
    absl::string_view library_path)
    : library_path_(library_path) {}

absl::Status LibOqsAuditCertificateVerifier::AddPublicKey(
    absl::string_view algorithm, absl::string_view key_id,
    absl::string_view public_key) {
  if (!IsMlDsaAlgorithm(algorithm)) {
    return absl::InvalidArgumentError(
        absl::StrCat("unsupported audit signature algorithm: ", algorithm));
  }
  if (key_id.empty() || public_key.empty()) {
    return absl::InvalidArgumentError(
        "liboqs verifier requires key_id and public_key.");
  }
  public_keys_.push_back(PublicKey{
      .algorithm = std::string(algorithm),
      .key_id = std::string(key_id),
      .bytes = std::string(public_key),
  });
  return absl::OkStatus();
}

absl::Status LibOqsAuditCertificateVerifier::Verify(
    const AuditCertificateSignature& signature,
    absl::string_view canonical_certificate) const {
  ASSIGN_OR_RETURN(LibOqsApi api, LibOqsApi::Load(library_path_));
  ASSIGN_OR_RETURN(OQS_SIG * raw_sig, api.NewSig(signature.algorithm));
  std::unique_ptr<OQS_SIG, OqsSigDeleter> sig(raw_sig, OqsSigDeleter{&api});

  for (const PublicKey& key : public_keys_) {
    if (key.algorithm != signature.algorithm ||
        key.key_id != signature.key_id) {
      continue;
    }
    if (key.bytes.size() != sig->length_public_key) {
      return absl::InvalidArgumentError(
          "liboqs verifier public key length does not match algorithm.");
    }
    const int rc = api.verify()(
        sig.get(), reinterpret_cast<const uint8_t*>(canonical_certificate.data()),
        canonical_certificate.size(),
        reinterpret_cast<const uint8_t*>(signature.signature.data()),
        signature.signature.size(),
        reinterpret_cast<const uint8_t*>(key.bytes.data()));
    if (rc == kOqsSuccess) return absl::OkStatus();
    return absl::UnauthenticatedError(
        "audit certificate signature verification failed.");
  }
  return absl::NotFoundError(
      "audit certificate signature key was not registered.");
}

}  // namespace litert::lm
