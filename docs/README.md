# LiteRT-LM Documentation

Welcome to the documentation for LiteRT-LM. Here you will find detailed
information about how to use the library, as well as API references and guides.

## Getting Started

If you are new to LiteRT-LM, this is the place to start. You will find
information on how to build and run the library, as well as a quick start guide.
Note that `bazel` is the recommended build system. The `CMake` build system is
added recently and still under active development.

*   [Build and Run using Bazel](./getting-started/build-and-run.md)
*   [(preliminary) CMake](./getting-started/cmake.md)

## Research Benchmarks

The DPM research artifacts include a GitHub Pages-ready benchmark explainer that
frames the prefill benchmark as a red-team assessment recreation and documents
the chart/data improvements needed for credible public claims.

*   [DPM red-team benchmark - selected Wipeout dossier](./benchmarks/dpm-dossier-y2k-wipeout.html)
    *   [Base dossier and art-direction comparison](./benchmarks/dpm-red-team-benchmark.html)
    *   [90s core dossier alternate](./benchmarks/dpm-dossier-90s-core.html)
    *   [Gilded age dossier alternate](./benchmarks/dpm-dossier-gilded-age.html)

## API Reference

Here you will find detailed information about the LiteRT-LM APIs.

*   **C++ API**
    *   [Conversation API](./api/cpp/conversation.md)
    *   [Constrained Decoding](./api/cpp/constrained-decoding.md)
    *   [Tool Use](./api/cpp/tool-use.md)
    *   [Advanced: ANTLR for Tool Use](./api/cpp/tool-use-antlr.md)
*   **Kotlin API**
    *   [Kotlin API](./api/kotlin/getting_started.md)
*   **Python API**
    *   [Python API](https://ai.google.dev/edge/litert-lm/python)

## Reporting Issues

If you encounter a bug or have a feature request, we encourage you to use the
[GitHub Issues](https://github.com/google-ai-edge/LiteRT-LM/issues/new) page to
report it.
