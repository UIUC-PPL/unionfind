## UnionFindLib

This library provides the functionality of performing union-find operations
on graphs in a distributed asynchronous fashion. The implementation is in
Charm++ and can be used in any generic Charm++-based graph applications.
The initial code was developed by Karthik Senthil.

### Example

An example application(a simple graph program) is included in the `examples`
directory. A more detailed documentation of the library usage and functionality
will be added soon.

### Currently implemented features

* Fully distributed union-find algorithm
* Simple path compression
* Connected components identification & labelling
* Threshold-based component pruning

### Component numbering (read this before using `componentNumber`)

After `find_components`, every vertex's `componentNumber` is **the component
boss's own `vertexID`** — a sparse 64-bit id. It is **not** a dense serial in
`0..C-1`. Key it through a map (`std::map`/`unordered_map`); never use it as an
array index.

This changed in `bbe0856` (2026-08-21). The library used to mint dense serials
with a parallel prefix scan before labeling began; a boss's globally unique
`vertexID` names its component just as well, needs no global coordination, and
lets labeling start immediately instead of waiting on the slowest element's
boss count. The component COUNT is still reported (the overlapped sum
reduction that sets `totalNumBosses`, delivered to every element).

**Reconstructing dense numbering, if a client needs it.** The formula is

    dense_id(boss) = exclusive_prefix_sum(per-element boss counts)
                   + rank of that boss within its element

and the scan input, `myLocalNumBosses`, is still computed by the self-naming
pass in `find_components`. The prefix library is still built, linked and
instantiated (`prefixLib/`, `prefixLibArray`, wired through
`passLibGroupID`) — only the call site was removed — so there are two ways
back:

1. **In place (a ~25-line revert of `bbe0856`'s call site):** scan first, then
   label with dense ids. Labeling then waits on the scan. Measured cost of that
   wait at 2 billion particles on 16 Frontier nodes: **wall-neutral** (+40.4 ms
   against a 39.9 ms arm spread; paratreet2 `design/campaign-archive/relay74.txt`
   declined to call it a regression). The prefix stage was never a measured
   cost; it was removed as an enabler, not as an optimization.
2. **As an optional post-pass:** keep self-naming so labeling starts
   immediately, then after labeling run the same scan, assign dense ids to
   local bosses, and translate each vertex's boss-`vertexID` label to the dense
   id through the `parentCache` request/reply path labeling already uses. Costs
   one scan plus roughly one extra labeling round; clients that do not want
   dense ids pay nothing.

**Two caveats.**

* Dense numbering is well defined only in **dense mode**. In lazy mode the
  self-naming pass counts only *touched* vertices — untouched ids are
  implicitly their own singleton components and are never counted — so a scan
  over boss counts would number only the touched components.
* The prefix reintroduces the constraint that dense serials are not stable
  until quiescence. Self-naming is what makes labeling re-runnable mid-stream
  (the compression-wave path, `FOF_WAVE` — compile-gated under
  `CONCURRENT_COMPRESSION_WAVE` since the 2026-08 cleanup, see
  Makefile.common); a dense scheme cannot be.

### Todos

* TRAM integration
* Local edge optimizations
* Priority for some messages
* Testing with large graph datasets (probabilistic meshes)
* Integration with Changa

### Installation
```
$ git clone https://github.com/UIUC-PPL/unionfind.git
$ cd prefixLib
edit CHARMC path in Makefile
$ make
$ cd ..
edit CHARM_DIR and UNION_FIND_DIR in Makefile.common
$ make
```
