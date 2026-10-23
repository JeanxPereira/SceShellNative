// The BGLayer's sound player ("SceBGLayerBgmMgr") of the ShellUI eboot (NPXS40087, FW 4.03): the
// natives behind Sce.Vsh.ShellUI.BGLayer.SoundPlayer (InitializeBgmNative .. DumpBgmDataNative,
// eboot 0x8E840 .. 0x8E9E0) and the eboot's "NGS2 Music System" (0x8FE40 .. 0x915A0) over
// libSceNgs2 (psmhost/libSceNgs2). The boot sounds (sfx_coldboot / sfx_warmboot / sfx_initialboot /
// sfx_transition .at9) play through it.
//
// The manager is root +40 of the BGLayer's native root (InitializeNative 0x8DB30 creates it with
// sub_8F1B0); bglayer.cpp calls the three hooks below at the points the eboot does.
#pragma once

namespace ssn::eboot::bglayer::bgm {

// End of InitializeNative (0x8DB30): `v8 = new(64); sub_8F1B0(v8); root[5] = v8`.
void create_manager();
// FinalizeNative (0x8DBF0): the root is deleted and qword_EC0080 cleared. The root's destructor
// (0x8F0E0) does not touch +40: the manager is not destroyed, it is unreachable from then on.
void root_finalized();
// UpdateNative (0x8DCC0): `if (root[5]) sub_8F3E0(root[5], state.FrameMilliseconds)` after the
// background layer's Update / Render.
void update(float frame_milliseconds);

}  // namespace ssn::eboot::bglayer::bgm
