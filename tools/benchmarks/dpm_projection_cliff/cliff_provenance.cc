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

#include "tools/benchmarks/dpm_projection_cliff/cliff_provenance.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "absl/strings/str_cat.h"  // from @com_google_absl
#include "absl/strings/strip.h"    // from @com_google_absl
#include "runtime/platform/hash/hasher.h"

#if defined(__unix__) || defined(__APPLE__)
#include <sys/utsname.h>
#include <unistd.h>
#endif

namespace litert::lm::bench {
namespace {

std::string Trim(absl::string_view s) {
  return std::string(absl::StripAsciiWhitespace(s));
}

std::string PopenFirstLine(const std::string& cmd) {
#if defined(__unix__) || defined(__APPLE__)
  std::FILE* f = ::popen(cmd.c_str(), "r");
  if (f == nullptr) return "";
  std::array<char, 1024> buf{};
  std::string line;
  if (std::fgets(buf.data(), static_cast<int>(buf.size()), f) != nullptr) {
    line = buf.data();
  }
  ::pclose(f);
  return Trim(line);
#else
  (void)cmd;
  return "";
#endif
}

}  // namespace

std::string DetectGitSha() {
  // git rev-parse --verify HEAD prints the 40-char SHA on success.
  // We tolerate failure (no .git, detached HEAD with no head, etc.).
  std::string sha = PopenFirstLine("git rev-parse --verify HEAD 2>/dev/null");
  // Validate: must be 40 lowercase hex.
  if (sha.size() != 40) return "";
  for (char c : sha) {
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return "";
  }
  return sha;
}

std::optional<bool> DetectDirtyTree() {
  // git status --porcelain prints nothing on a clean tree.
  std::string out = PopenFirstLine(
      "git status --porcelain --untracked-files=no 2>/dev/null");
  // popen returns empty on both clean and on error; we disambiguate by
  // checking whether the SHA is detectable too.
  if (DetectGitSha().empty()) return std::nullopt;
  return !out.empty();
}

std::string DetectHostname() {
#if defined(__unix__) || defined(__APPLE__)
  std::array<char, 256> buf{};
  if (::gethostname(buf.data(), buf.size() - 1) == 0) {
    buf[buf.size() - 1] = '\0';
    return std::string(buf.data());
  }
#endif
  return "";
}

std::string DetectOs() {
#if defined(__unix__) || defined(__APPLE__)
  struct utsname u;
  if (::uname(&u) == 0) {
    return absl::StrCat(u.sysname, " ", u.release, " ", u.machine);
  }
#endif
  return "";
}

std::string DetectCpuModel() {
#if defined(__linux__)
  std::ifstream in("/proc/cpuinfo");
  if (!in.is_open()) return "";
  std::string line;
  while (std::getline(in, line)) {
    constexpr absl::string_view kKey = "model name";
    if (line.size() > kKey.size() && line.substr(0, kKey.size()) == kKey) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) continue;
      return Trim(line.substr(colon + 1));
    }
  }
  return "";
#else
  return "";
#endif
}

std::string DetectAcceleratorId() {
  // Best-effort GPU detection. nvidia-smi -L produces lines like
  //   "GPU 0: NVIDIA A10G (UUID: GPU-...)".
  // We take the first such line. If nvidia-smi is not present or the
  // call fails, we return empty so the JSONL writer omits the field.
  std::string line = PopenFirstLine("nvidia-smi -L 2>/dev/null");
  if (!line.empty()) return line;
  // ROCm fallback (silent if rocm-smi is missing).
  line = PopenFirstLine(
      "rocm-smi --showproductname 2>/dev/null | sed -n '2p'");
  return line;
}

std::string DetectArchitectureTag() {
#if defined(__linux__)
  #if defined(__aarch64__)
    return "linux_arm64";
  #elif defined(__x86_64__)
    return "linux_x86_64";
  #else
    return "linux";
  #endif
#elif defined(__APPLE__)
  #if defined(__aarch64__)
    return "macos_arm64";
  #else
    return "macos_x86_64";
  #endif
#elif defined(_WIN32)
  return "windows_x86_64";
#else
  return "";
#endif
}

std::string Blake3HexOfFile(const std::string& path) {
  if (path.empty()) return "";
  std::ifstream in(path, std::ios::binary);
  if (!in.is_open()) return "";
  auto hasher = CreateHasher(HashAlgorithm::kBlake3);
  if (hasher == nullptr) return "";
  constexpr size_t kChunk = 1 << 16;
  std::vector<char> buf(kChunk);
  while (in) {
    in.read(buf.data(), buf.size());
    const std::streamsize n = in.gcount();
    if (n > 0) {
      hasher->Update(absl::string_view(buf.data(), static_cast<size_t>(n)));
    }
  }
  return hasher->Finalize().ToHex();
}

CliffProvenance CaptureProvenance(const std::string& model_path,
                                  const std::string& config_path) {
  CliffProvenance p;
  p.git_sha = DetectGitSha();
  p.dirty_tree = DetectDirtyTree();
  // Runtime version: we don't have a top-level VERSION file in the
  // repo today, so we report the bench's namespace + the build date
  // baked in by the compiler. Replace if we add a versioning scheme.
  p.runtime_version = absl::StrCat("litert-lm-bench@", __DATE__);
  p.hostname = DetectHostname();
  p.os = DetectOs();
  p.cpu_model = DetectCpuModel();
  p.accelerator_id = DetectAcceleratorId();
  p.architecture_tag = DetectArchitectureTag();
  if (!model_path.empty()) {
    p.model_artifact_hash = Blake3HexOfFile(model_path);
  }
  if (!config_path.empty()) {
    p.config_hash = Blake3HexOfFile(config_path);
  }
  return p;
}

}  // namespace litert::lm::bench
