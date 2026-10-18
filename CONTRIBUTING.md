# Contributing Guidelines

Contributions to `nvidia-overlay-shared-mapping` must adhere to modern C++17 systems programming standards.

## Engineering Standards

1. **Language & Dialect**: ISO C++17 (MSVC v143).
2. **RAII & Resource Management**:
   - File mapping handles (`HANDLE`), view pointers (`UnmapViewOfFile`), and Direct3D COM interfaces (`ID3D11Device`, `ID3D11Texture2D`, `IDXGIResource`) must be strictly managed using RAII wrappers (`Microsoft::WRL::ComPtr` or custom smart handles).
3. **Concurrency & Thread Safety**:
   - Shared memory updates must atomically increment sequence numbers to avoid rendering torn or partially copied texture frames.
4. **Clean Aesthetics**:
   - Zero emojis across all commits, PRs, comments, and documentation.
   - Authoritative, formal technical English.
