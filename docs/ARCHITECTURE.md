# Deep IPC Architecture & Token Cryptanalysis

## Named File Mapping Naming Scheme

The NVIDIA in-game overlay identifies shared-memory IPC sections through a deterministic object namespace residing under the Windows Session Object Directory (`\Sessions\X\BaseNamedObjects\` or `\BaseNamedObjects\`):

```text
Section Name Format:
NV_SHARED_OVERLAY_SLOT_{TargetPID}_{SlotIndex}
```

The permissions assigned to these file mapping section objects historically granted full access (`GENERIC_ALL` / `FILE_MAP_ALL_ACCESS`) to members of the local Interactive Logon Session. Consequently, any user-mode process executing under the same user account can discover and map these sections without elevation.

## Shared Memory Structure Layout

Each section maps a contiguous physical page backing the following POD structures:

```text
+------------------------------------+
| SECTION HEADER (32 Bytes)          |
| - uint32_t Version (0x000B0280)    |
| - uint32_t TargetPID               |
| - uint32_t TotalSlots              |
| - uint32_t GlobalSequenceCounter   |
| - uint8_t  Reserved[16]            |
+------------------------------------+
| SLOT 0 DESCRIPTOR (64 Bytes)       |
| - uint64_t EncodedToken            |
| - uint32_t Width                   |
| - uint32_t Height                  |
| - uint32_t Format (DXGI_FORMAT)    |
| - float    Bounds[4] (Left,Top,R,B)|
| - uint32_t SlotRevisionCounter     |
| - uint8_t  Reserved[28]            |
+------------------------------------+
| SLOT 1 .. N DESCRIPTORS            |
+------------------------------------+
```

## Mathematical Breakdown of the Token Permutation

To prevent untrusted callers from placing arbitrary integer handles into the slot, the handle undergoes an encoding permutation before assignment:

```cpp
std::uint64_t resource_token_mask(std::uint64_t key, std::uint32_t seed) {
    constexpr std::uint64_t c1 = 0xFF51AFD7ED558CCDull;
    constexpr std::uint64_t cx = 0xED77E7F1C90AA277ull;
    constexpr std::uint64_t c2 = 0xC4CEB9FE1A85EC53ull;

    const std::uint64_t repeated = static_cast<std::uint64_t>(seed) |
        (static_cast<std::uint64_t>(seed) << 32);

    std::uint64_t value = key ^ repeated;
    value = (value ^ (value >> 33)) * c1;
    value = (value ^ (value >> 33)) * cx;
    value = (value ^ (value >> 33)) * c2;
    return value ^ (value >> 33);
}
```

### Invertibility of the Permutation

Each stage in this pipeline is a bijection over $\mathbb{Z}_{2^{64}}$:

1. **Multiplication by Odd Constants**:
   Because $c_1, c_x, c_2$ are odd integers, their greatest common divisor with $2^{64}$ is 1:
   $$\gcd(c_i, 2^{64}) = 1$$
   Thus, multiplicative modular inverses $c_i^{-1} \pmod{2^{64}}$ exist and can be computed via the Extended Euclidean Algorithm:
   - $c_1^{-1} = 0x4F74465B47B855B5\text{ull}$
   - $c_x^{-1} = 0x6E95A8D19F9F4247\text{ull}$
   - $c_2^{-1} = 0x9320B58FF199859B\text{ull}$

2. **Shift-XOR Step Inversion**:
   The transformation $y = x \oplus (x \gg 33)$ is invertible because the upper 33 bits of $x$ are unchanged in $y$:
   - $x_{63..31} = y_{63..31}$
   - $x_{30..0} = y_{30..0} \oplus x_{63..33}$
   Hence, $x = y \oplus (y \gg 33)$.

3. **Known Plaintext Key Recovery**:
   When `CaptureCore.log` records the decoded shared handle $H$ corresponding to an observed encoded token $T$, the mask $M$ is obtained as:
   $$M = T \oplus H$$
   Applying the inverted pipeline to $M$ and XORing with the duplicated seed recovers the secret session key $K_{session}$ in $\mathcal{O}(1)$ time.

## D3D11 Shared Texture Pipeline

```text
[ Companion Process ]                                   [ NVIDIA Compositor ]
         │                                                        │
         ├─► D3D11CreateDevice(...)                               │
         │                                                        │
         ├─► CreateTexture2D(D3D11_RESOURCE_MISC_SHARED)          │
         │                                                        │
         ├─► QueryInterface(IID_PPV_ARGS(&dxgiResource))          │
         ├─► dxgiResource->GetSharedHandle(&hShared)              │
         │                                                        │
         ├─► Encrypt Token & Update Slot MMF ─────────────────────┤
         │                                                        │
         ├─► Render UI Loop & ID3D11DeviceContext::Flush()        │
         │                                                        ├─► ID3D11Device::OpenSharedResource(hShared)
         │                                                        ├─► Paint Texture onto Desktop Overlay
```
