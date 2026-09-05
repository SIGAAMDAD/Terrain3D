// Copyright © 2023-2026 Cory Petkovsek, Roope Palmroos, and Contributors.

#ifndef TERRAIN3D_RENDER_WORLD_VT_SHADERS_H
#define TERRAIN3D_RENDER_WORLD_VT_SHADERS_H

static const char *T3D_RW_M06_VT_SAMPLE_GLSL = R"GLSL(
struct VTPageTableEntry {
    uint physical_slot;
    uint parent_page;
    uint generation;
    uint mip_flags;
};
bool t3d_vt_resolve_page(
    uint wanted_page, uint page_table_count, readonly buffer VTPageTable { VTPageTableEntry page_table[]; }, out uint physical_slot, out uint resolved_mip) {
    uint page = wanted_page;
    for (uint i = 0u; i < 18u; ++i) {
        if (page == 0xffffffffu || page >= page_table_count) {
            break;
        }
        VTPageTableEntry e = page_table[page];
        uint flags = e.mip_flags >> 16u;
        if ((flags & 2u) != 0u && e.physical_slot != 0xffffffffu) {
            physical_slot = e.physical_slot;
            resolved_mip = e.mip_flags & 0xffffu;
            return true;
        }
        page = e.parent_page;
    }
    physical_slot = 0xffffffffu;
    resolved_mip = 0u;
    return false;
}
)GLSL";

#endif // TERRAIN3D_RENDER_WORLD_VT_SHADERS_H
