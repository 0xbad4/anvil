#pragma once

// TODO optimize redundant instructions (e.g. `tax` followed by `txa` is a no-op, `ld [x+0]` is the same as `ld [x]`, etc.)
// TODO build it in a modular way so that we can add more optimizations later (e.g. peephole optimizations, constant folding, etc.)