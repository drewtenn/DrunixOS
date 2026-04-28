# Rexy App Porting Policy

Rexy is a first-class Drunix userland language, not a thin translation target.
When porting a Drunix app from C, C++, assembly, shell, or another language to
Rexy, missing functionality must be fixed in Rexy itself instead of worked
around in the app.

If a port needs functionality Rexy cannot express yet, implement the right
compiler, backend, runtime, or standard-library support first. Then write the
app in normal Rexy using that surface. Do not copy behavior into per-app
externs, hard-coded syscall shims, generated helper blobs, target-specific
shortcuts, or other local escape hatches just to get one program compiling.

Use the same shape other supported languages and compilers would provide:

- Language syntax and semantics belong in the Rexy parser, semantic analyzer,
  IR lowering, and code generators.
- Hosted OS behavior belongs in Rexy's `std` surface, implemented in portable
  Rexy where possible.
- Target-specific details belong only at the narrow runtime adapter boundary:
  ABI startup, syscalls, linking, object format, and unavoidable assembly.
- Drunix Make rules should invoke `rexc` as the compiler driver. They should
  not assemble, link, or patch Rexy output by hand.
- Tests should cover the new compiler or stdlib surface before the app depends
  on it.

This rule is deliberately strict. Each app port should make Rexy more complete
for the next port. If the honest implementation requires a missing feature such
as structs, slices, allocation, richer results, file APIs, process APIs,
environment access, or target codegen support, add that feature to Rexy first
and keep the app code boring.
