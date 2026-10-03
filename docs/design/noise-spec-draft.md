# Noise functions: spec draft v2

Drafted by TheMadMaid (DeepSeek Flash, level 0, text only) over the slow hop, tasks 001 to 004, 2026-10-03; reviewed by Claude. Unverified where marked. Item 5 of her decision list (harness vocabulary) was outside the task and is left out here. Status: draft, nothing built.

## API

Single header, namespace `maid::noise`, one consistent shape (no `value2`):

- `float perlin2(float x, float y, uint32_t seed)` / `perlin3(...)`
- `float simplex2(float x, float y, uint64_t seed)` / `simplex3(...)`
- `float fbm(int octaves, float lacunarity, float gain, F gen)` : wraps any of the above
- `uint32_t seed_from(uint64_t source)` : a mixer, not a generator

**Range:** declare `[-1, 1]` as the contract, note that the observed range is narrower, and clamp nothing silently. A caller needing the full range rescales at the call site. Assert the declared range in tests; record the observed maximum.

**Stacking:** `fbm` takes a callable rather than a fixed lattice pointer, so it composes over any source.

## Sources: strengths and selection

**stb_perlin** : a vendored single file, cheapest to integrate and safest to ship; fbm, turbulence and ridged variants included. Its 2D behaviour is less carefully specified than its 3D. *Pick for jitter, and wherever the effect must exist with near-zero integration cost.*

**Perlin improved noise (2002)** : a specification written fresh, so you control it completely and it is the easiest to test. Classic axis-aligned artefacts, worst in 3D. *Pick as the reference implementation the others are checked against, and where auditability beats quality.*

**OpenSimplex2** : visibly better isotropy, no obvious axis artefacts, cleaner 3D; CC0. More code to write fresh, no single-file reference. *Pick for themes, TUI effects and the review page : anything the eye looks at. This is the default for visual output.*

## Seeds

Gradient-noise lattices are fixed; reproducibility across seeds comes either from a seed argument inside the implementation or from offsetting the input coordinates by a seed-derived value : these are not equivalent, and which applies differs per source.

- **Perlin (2002):** no seed in the algorithm; reproducibility is a coordinate offset. With no offset, identical coordinates give identical output regardless of seed : say so.
- **OpenSimplex2:** the gradient table is fixed; seed affects output only via offset or permutation choice. Choose one mechanism and document it.
- **stb_perlin:** **verify against the vendored `stb_perlin.h` (does `stb_perlin_noise3_seed` exist?) before choosing between a seed argument and a coordinate offset.**

I am **unsure** whether the three should share one offset scheme. Sharing makes them mutually comparable; differing schemes suit each. I recommend sharing.

## Test list

**Pin values from the chosen implementation first** : generate and commit them, then test against those. Never test against numbers from a paper or a website.

- **Range:** wide grid sample; assert within the declared contract; record observed maximum.
- **Determinism:** same input and seed twice, same output; and across a process restart.
- **Seed separation:** different seeds, same coordinate, different output : assert *not equal*, never a specific value.
- **Continuity:** fine steps along a line; no jump beyond a bound derived from step size.
- **Zero and negatives:** at and around the origin, and each negative quadrant.
- **Lattice points:** for **Perlin-style implementations only**, gradient noise is exactly zero at integer coordinates : assert it. **OpenSimplex2 at integer points is to be measured and recorded, not asserted**; I am unsure whether its skewed lattice coincides with the gradient origins that force zero.
- **Symmetry:** do not assert exact symmetry under sign flips; the classic implementations are not exactly symmetric.

## Decisions for Micaiah

Each is a recommendation, not a settled choice:

1. **One header or one per source** : recommend one header; the API is deliberately uniform.
2. **Which sources at launch** : recommend stb_perlin first (cheapest, covers jitter), Perlin fresh second as the reference, OpenSimplex2 when the visual work starts.
3. **Cross-platform reproducibility** : recommend guaranteeing it; if required, it constrains the seed hash, not the noise.
4. **Smooth noise or hash-to-float for jitter** : recommend hash-to-float if jitter wants a statistical guarantee (uniform, no clumping); smooth noise may be the wrong tool there.
