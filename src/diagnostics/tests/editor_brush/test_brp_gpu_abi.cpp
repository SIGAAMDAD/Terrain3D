#include "rendering/rd/core/terrain_3d_editor_brush_gpu.h"
#include "rendering/rd/core/terrain_3d_editor_brush_transaction.h"
#include <cassert>
#include <cstring>
#include <vector>
#include <iostream>

static uint32_t lookupIndex(const int32_t rx, const int32_t ry, const uint32_t x, const uint32_t y) {
    return ((uint32_t(ry + 16) * 8u + y / 16u) * 256u) + uint32_t(rx + 16) * 8u + x / 16u;
}
int main() {
    assert(sizeof(t3dEditorBrushGpuTile_t)==32u);
    assert(sizeof(t3dEditorBrushGpuPush_t)==128u);
    assert(lookupIndex(-16,-16,0,0)==0u);
    assert(lookupIndex(15,15,112,112)==65535u);
    assert(lookupIndex(-1,-1,112,112)==32639u);
    Terrain3DEditorBrushTransaction t;
    assert(t.begin(500u, 1u, 1u));
    t3dEditorBrushTile_t tile;
    tile.m_nRegionX=-1; tile.m_nRegionY=-1;
    tile.m_nMinX=112u; tile.m_nMinY=112u;
    tile.m_nWidth=16u; tile.m_nHeight=16u;
    tile.m_nRowStrideBytes=64u;
    tile.m_nEncoding=T3D_EDITOR_BRUSH_CONTROL_RF_BITS;
    tile.m_nSourceRevision=UINT64_MAX-2u;
    assert(t.appendTile(tile));
    t3dEditorBrushCommand_t cmd;
    cmd.m_nStrokeId=500u; cmd.m_nOwnerGeneration=1u; cmd.m_nDeviceGeneration=1u;
    cmd.m_nMapType=1u; cmd.m_nTool=7u; cmd.m_nOperation=0u;
    cmd.m_nRegionX=-1; cmd.m_nRegionY=-1; cmd.m_nSequence=1u;
    cmd.m_fRadius=16.f;
    assert(t.acceptCommand(cmd));
    assert(t.command(0).m_nTileCountAtEnqueue==1u);
    std::vector<uint8_t> before(128u*128u*4u), after=before;
    std::vector<uint8_t> payload(1024u);
    for(uint32_t y=0;y<16u;++y) for(uint32_t x=0;x<16u;++x) {
        uint32_t initial=1u, modified=(x+y)%2 ? 5u : 1u;
        std::memcpy(before.data()+((112u+y)*128u+112u+x)*4u,&initial,4u);
        std::memcpy(after.data()+((112u+y)*128u+112u+x)*4u,&initial,4u);
        std::memcpy(payload.data()+(y*16u+x)*4u,&modified,4u);
    }
    bool changed=false, hole=false;
    assert(Terrain3DEditorBrushTransaction::stageTileBytes(tile,payload.data(),uint32_t(payload.size()),before.data(),after.data(),uint32_t(before.size()),changed,hole));
    assert(changed&&hole);
    assert(!std::memcmp(after.data()+((112u)*128u+113u)*4u,payload.data()+4u,4u));
    assert(t.seal()); assert(t.markReadbackPending());
    assert(t.acceptReadback(500u,1u,1u,1024u));
    assert(t.claimReadbackForCommit()); assert(!t.claimReadbackForCommit());
    assert(t.markCpuCommitted());
    std::cout << "BRP Wave 2 GPU tile ABI: passed\n";
}
