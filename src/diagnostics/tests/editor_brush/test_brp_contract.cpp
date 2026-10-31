// Host test of the BRP-04/06 bounded, generation-checked transaction contract.
// Run with: clang++ -std=c++17 -Wall -Wextra -Werror -Isrc this_file.cpp -o /tmp/brp02 && /tmp/brp02
#include "rendering/rd/core/terrain_3d_editor_brush_transaction.h"
#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>

static t3dEditorBrushTile_t controlTile( uint16_t minx, uint16_t miny, uint32_t offset ) {
    t3dEditorBrushTile_t tile{};
    tile.m_nWidth = 16;
    tile.m_nHeight = 16;
    tile.m_nMinX = minx;
    tile.m_nMinY = miny;
    tile.m_nRowStrideBytes = 64;
    tile.m_nEncoding = T3D_EDITOR_BRUSH_CONTROL_RF_BITS;
    tile.m_nByteOffset = offset;
    return tile;
}
static void tileChecks() {
    Terrain3DEditorBrushTransaction txn;
    assert(!txn.begin(0, 2, 3));
    assert(!txn.begin(1, 0, 3));
    assert(!txn.begin(1, 2, 0));
    assert(txn.begin(1, 2, 3));
    assert(!txn.begin(1, 2, 3));
    t3dEditorBrushTile_t t = controlTile(0,0,0);
    assert(txn.appendTile(t));
    assert(!txn.appendTile(t));
    t.m_nMinX=16;
    assert(!txn.appendTile(t)); // wrong offset
    t.m_nByteOffset=1024;
    assert(txn.appendTile(t));
    assert(txn.payloadBytes()==2048);
    assert(txn.tileCount()==2);
    for(uint32_t i=2; i<T3D_EDITOR_BRUSH_MAX_TILES;i++) {
        t = controlTile( uint16_t((i%8)*16), uint16_t(((i%64)/8)*16), txn.payloadBytes() );
        t.m_nRegionX = int32_t(i / 64);
        assert(txn.appendTile(t));
    }
    assert(txn.payloadBytes()==T3D_EDITOR_BRUSH_MAX_TRANSFER_BYTES);
    t.m_nByteOffset=txn.payloadBytes();
    t.m_nMinX=0; t.m_nMinY=96;
    assert(!txn.appendTile(t)); // maximum tile count
    assert(!txn.seal()); // no brush commands means no stroke
    t3dEditorBrushCommand_t command{};
    command.m_nStrokeId=1;
    command.m_nOwnerGeneration=2;
    command.m_nDeviceGeneration=3;
    command.m_nSequence=1;
    command.m_nMapType=1;
    command.m_fRadius=20.f;
    assert(txn.acceptCommand(command));
    assert(txn.commandCount()==1);
    assert(txn.command(0).m_nSequence==1);
    assert(txn.seal());
    assert(!txn.appendTile(t)); // sealed
}
static void callbackChecks() {
    Terrain3DEditorBrushTransaction txn;
    assert(txn.begin(11,22,33));
    assert(txn.appendTile(controlTile(0,0,0)));
    t3dEditorBrushCommand_t cmd{};
    cmd.m_nStrokeId=11;
    cmd.m_nOwnerGeneration=22;
    cmd.m_nDeviceGeneration=33;
    cmd.m_nSequence=1;
    cmd.m_nMapType=1;
    cmd.m_fRadius=20.0f;
    assert(txn.acceptCommand(cmd));
    assert(!txn.acceptCommand(cmd));
    cmd.m_nSequence=2;
    cmd.m_fRadius=-1.0f;
    assert(!txn.acceptCommand(cmd));
    cmd.m_fRadius=NAN;
    assert(!txn.acceptCommand(cmd));
    cmd.m_fRadius=20.0f;
    assert(txn.acceptCommand(cmd));
    assert(txn.seal());
    assert(!txn.markCpuCommitted());
    assert(txn.markReadbackPending());
    assert(!txn.acceptReadback(11,22,34,1024));
    assert(!txn.acceptReadback(11,23,33,1024));
    assert(!txn.acceptReadback(11,22,33,1000));
    assert(txn.acceptReadback(11,22,33,1024));
    assert(!txn.acceptReadback(11,22,33,1024)); // duplicate callback
    assert(!txn.markCpuCommitted()); // readback cannot be acknowledged before claiming
    assert(txn.claimReadbackForCommit());
    assert(!txn.claimReadbackForCommit()); // exactly one owner-thread publisher
    assert(txn.state()==T3D_EDITOR_BRUSH_CPU_COMMITTING);
    assert(txn.markCpuCommitted());
    assert(!txn.markCpuCommitted());
    txn.cancel();
    assert(txn.state()==T3D_EDITOR_BRUSH_CPU_COMMITTED);
    txn.reset();
    assert(txn.begin(12,22,33));
    assert(txn.appendTile(controlTile(0,0,0)));
    txn.cancel();
    assert(!txn.seal());
    txn.reset();
    assert(txn.begin(13,22,33));
    assert(txn.appendTile(controlTile(0,0,0)));
    cmd.m_nStrokeId=13;
    cmd.m_nSequence=1;
    assert(txn.acceptCommand(cmd));
    assert(txn.seal());
    assert(txn.markReadbackPending());
    txn.cancel();
    assert(!txn.acceptReadback(13,22,33,1024));
    assert(!txn.markCpuCommitted());
    txn.reset();
    assert(txn.begin(14,22,33));
    txn.fail();
    assert(!txn.seal());
}
int main() {
    tileChecks();
    callbackChecks();
    puts("BRP-04/06 transaction contract: PASS");
    return 0;
}
