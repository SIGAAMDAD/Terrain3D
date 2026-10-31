// BRP-07: host-native packed tile publication contract; no Godot runtime.
#include "rendering/rd/core/terrain_3d_editor_brush_transaction.h"
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

static t3dEditorBrushTile_t tile( uint8_t encoding, uint16_t x, uint16_t y,
                                   uint16_t w, uint16_t h, uint32_t offset ) {
    t3dEditorBrushTile_t t{};
    t.m_nEncoding = encoding;
    t.m_nMinX = x; t.m_nMinY = y;
    t.m_nWidth = w; t.m_nHeight = h;
    t.m_nRowStrideBytes = w * Terrain3DEditorBrushTransaction::bytesPerTexel(encoding);
    t.m_nByteOffset = offset;
    return t;
}

static void packedHeight() {
    uint8_t source[128u*128u*2u]{};
    uint8_t dest[sizeof(source)]{};
    const uint8_t bytes[] = {0x12,0x34, 0xab,0xcd, 0x01,0x02, 0xfe,0xff};
    bool changed=false, hole=false;
    t3dEditorBrushTile_t t = tile(T3D_EDITOR_BRUSH_HEIGHT_R16, 127, 126, 1, 2, 0);
    assert(Terrain3DEditorBrushTransaction::stageTileBytes(t, bytes, 4,
        source, dest, sizeof(source), changed, hole));
    assert(changed && !hole);
    assert(dest[(126u*128u+127u)*2u] == 0x12);
    assert(dest[(126u*128u+127u)*2u+1] == 0x34);
    assert(dest[(127u*128u+127u)*2u] == 0xab);
    assert(dest[(127u*128u+127u)*2u+1] == 0xcd);
    // Reject invalid second row without touching the detached destination.
    changed=false;
    t.m_nByteOffset=3;
    assert(!Terrain3DEditorBrushTransaction::stageTileBytes(t, bytes, 4,
        source, dest, sizeof(source), changed, hole));
    assert(!changed);
}

static void packedControl() {
    uint32_t original[128u*128u]{};
    uint32_t after[128u*128u]{};
    const uint32_t texture_only[] = {0xf000001u, 0x00fa001u};
    bool changed=false, hole=false;
    t3dEditorBrushTile_t t = tile(T3D_EDITOR_BRUSH_CONTROL_RF_BITS, 15, 31, 2, 1, 0);
    assert(Terrain3DEditorBrushTransaction::stageTileBytes(t,
        reinterpret_cast<const uint8_t *>(texture_only), sizeof(texture_only),
        reinterpret_cast<const uint8_t *>(original), reinterpret_cast<uint8_t *>(after),
        sizeof(original), changed, hole));
    assert(changed && !hole);
    assert(after[31u*128u+15u] == texture_only[0]);
    assert(after[31u*128u+16u] == texture_only[1]);
    const uint32_t holes[] = {0xf000005u, 0x00fa005u};
    changed=false;
    assert(Terrain3DEditorBrushTransaction::stageTileBytes(t,
        reinterpret_cast<const uint8_t *>(holes), sizeof(holes),
        reinterpret_cast<const uint8_t *>(original), reinterpret_cast<uint8_t *>(after),
        sizeof(original), changed, hole));
    assert(changed && hole);
    assert(after[31u*128u+15u] == holes[0]);
}

static void packedColor() {
    uint32_t original[128u*128u]{};
    uint32_t after[128u*128u]{};
    const uint32_t paint[] = {0x01020304u,0xff000000u};
    bool changed=false, hole=false;
    t3dEditorBrushTile_t t = tile(T3D_EDITOR_BRUSH_COLOR_RGBA8, 126, 127, 2, 1, 0);
    assert(Terrain3DEditorBrushTransaction::stageTileBytes(t,
        reinterpret_cast<const uint8_t *>(paint), sizeof(paint),
        reinterpret_cast<const uint8_t *>(original), reinterpret_cast<uint8_t *>(after),
        sizeof(original), changed, hole));
    assert(changed && !hole);
    assert(after[127u*128u+126u] == paint[0] && after[127u*128u+127u] == paint[1]);
    changed=false;
    assert(Terrain3DEditorBrushTransaction::stageTileBytes(t,
        reinterpret_cast<const uint8_t *>(paint), sizeof(paint),
        reinterpret_cast<const uint8_t *>(after), reinterpret_cast<uint8_t *>(after),
        sizeof(after), changed, hole));
    assert(!changed && !hole);
}

static void revisionsAndCoverage() {
    Terrain3DEditorBrushTransaction txn;
    assert(txn.begin(7u, 19u, 3u));
    t3dEditorBrushTile_t t = tile(T3D_EDITOR_BRUSH_CONTROL_RF_BITS, 0, 0, 16, 16, 0);
    t.m_nSourceRevision = 0x123400000021ULL;
    assert(txn.appendTile(t));
    t.m_nSourceRevision++;
    t.m_nMinX = 16; t.m_nByteOffset=1024;
    assert(!txn.appendTile(t));
    t.m_nSourceRevision--;
    assert(txn.appendTile(t));
    t3dEditorBrushCommand_t cmd{};
    cmd.m_nStrokeId = 7u; cmd.m_nOwnerGeneration=19u;
    cmd.m_nDeviceGeneration=3u; cmd.m_nSequence=1u;
    cmd.m_nMapType=T3D_EDITOR_BRUSH_CONTROL_RF_BITS;
    cmd.m_fRadius=10.f;
    cmd.m_nRegionX=42;
    assert(!txn.acceptCommand(cmd));
    cmd.m_nRegionX=0;
    assert(txn.acceptCommand(cmd));
    assert(txn.seal());
    assert(txn.markReadbackPending());
    assert(txn.acceptReadback(7u, 19u, 3u, txn.payloadBytes()));
    txn.cancel();
    assert(!txn.claimReadbackForCommit());
    assert(!txn.markCpuCommitted());
}
int main() {
    packedHeight(); packedControl(); packedColor(); revisionsAndCoverage();
    puts("BRP-07 packed tile and owner revision contract: PASS");
    return 0;
}
