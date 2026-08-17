# EABase dependency

Terrain3D's EASTL integration is pinned to the official Electronic Arts EABase
repository at commit:

`0699a15efdfd20b6cecf02153bfa5663decb653c`

The execution environment used to build this patch had no outbound DNS, so a
full `git clone` could not be materialized here. This directory contains the
small compatibility header subset required by EASTL's configured build.

Run `tools/fetch_eabase.sh` from a networked checkout to replace this subset
atomically with the complete pinned official repository. The include path is
identical, so no source or SCons changes are required.
