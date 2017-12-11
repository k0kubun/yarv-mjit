# YARV-MJIT

Stack-based method JIT compiler of MRI, a fork of [MJIT](https://github.com/vnmakarov/ruby) project.

## What's YARV-MJIT?

YARV-MJIT is method JIT compiler implementation based on MJIT infrastructure.

The biggest difference between Vladimir's RTL-MJIT and YARV-MJIT is that YARV-MJIT is compiled from not RTL instructions
but original stack-based YARV instructions like [LLRB](https://github.com/k0kubun/llrb).

### Details
Interface and architecture are the same as original MJIT's ones.
So it generates C source code and lets C compiler to generate native code.
In implementation, mjit.c is almost a subset of MJIT's one,
and mjit\_compile.c part is different but it's separated in object level for future easy replacement with RTL version.

## Why YARV-MJIT?

For Ruby 3x3, Ruby 3 will be likely to have JIT compiler. And currently the most promising approach seems to be MJIT.

But the problem is that MJIT is based on whole new RTL instructions.
After replacing all VM instructions, unlike JIT compiler which is optionally turned on by `-j` flag,
we can't turn off VM changes and changing all VM instructions is likely to have some bugs.
Mandatory change is risker than optional change.

So for serious production systems, it will be harder to try new version including RTL instructions, even in the case JIT is turned off.
Then there will be few oppotunities to test Ruby changes which are NOT related to RTL or MJIT,
and thus complex MJIT infrastructure won't be tested on serious systems too.

However, if we compile native code from current YARV instructions,
we can safely introduce JIT compiler and its infrastructure, keeping chance to turn off all experimental JIT features.
And in that case, many people will use new Ruby on production.
The more new Ruby will be used, the more experimental features will be tested too.

**The aim of YARV-MJIT probject is introducing optional JIT infrastructure first in Ruby 2.x,
and then adding mandatory VM changes of RTL instructions with already-tested JIT infrastructure in Ruby 3.0.**
It's designed to be a safe migration step from YARV to RTL w/ MJIT.

## Current status

- Much slower than MJIT, but faster than latest MRI
- As it doesn't modify VM at all, **`make test-all` and `make test-spec` pass** without `-j` (JIT enable flag)
  - With `-j`, it's still broken and there are bugs that should be fixed
- Minimizing header is still not ported to YARV-MJIT
- Some big functions like `vm_search_method` are not inlined yet
  - So there are still many oppotunities to reproduce original MJIT's performance
  - But it would increase compilation time, so minimizing header should be introduced first
- And thus Ruby method can't be inlined

### Optcarrot benchmark

Note: Base revision of YARV-MJIT is newer than 2.5.0-preview1.

- Measured with Intel 4.0GHz i7-4790K with 16GB memory under x86-64 Ubuntu 8 Cores
- I used following implementations:
  - v2 - Ruby MRI version 2.0.0
  - v2.5 - Ruby MRI 2.5.0-preview1, which is very similar to YARV-MJIT's base (newer than MJIT's base)
  - rtl - Vladimir's latest RTL MJIT ([21bbbd3](https://github.com/vnmakarov/ruby/commit/21bbbd37b5d9f86910f7679a584bbbfb9dc9c9b1)) without `-j` option
  - rtl-mjit - MJIT (`-j`) with GCC 5.4.0 with -O2
  - rtl-mjit-cl - MJIT (`-j:l`) using LLVM Clang 3.8.0 with -O2
  - yarv - k0kubun's YARV-MJIT without `-j` option, mostly same as trunk
  - yarv-mjit - k0kubun's YARV-MJIT (`-j`) with GCC 5.4.0 with -O2
  - yarv-mjit-cl - YARV-MJIT (`-j`) with LLVM Clang 3.8.0 with -O2

|   | v2 | v2.5 | rtl | rtl-mjit | rtl-mjit-cl | yarv | yarv-mjit | yarv-mjit-cl |
|:--|:---|:-----|:----|:---------|:------------|:-----|:----------|:-------------|
|FPS|35.41|43.36|38.00|75.57     | 81.25       | 45.76| 69.44     | 59.60        |
|Speedup|1.0|1.22|1.07|2.13      | 2.29        | 1.29 | 1.96      | 1.68         |


### Other benchmarks
[rails\_ruby\_bench](https://github.com/noahgibbs/rails_ruby_bench) and micro benchmarks are to be done.

## Future works

- Support Windows
- Fix bugs in JIT-ed code, and pass tests with `-j`
- Inline Ruby method and one written in C
