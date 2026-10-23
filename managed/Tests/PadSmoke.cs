// Input smoke test: reads the emulated DualSense through the firmware's own
// Sce.PlayStation.Core / Sce.PlayStation.Orbis (FW 4.03) the way the shell does —
// InputManager.SetDeviceIndexBehavior(MergeAllConnectedDevice), then per frame
// GamePad.GetData(0) (the PUI's UIGamePad) and InputManager.GetSystemButtonsState(0)
// (the AppMain PadManager) — printing every change.
//
//   SceShellNative.exe --firmware <dump> --overlay <dump> --exec PadSmoke.exe [frames]
//
// Built against the prepared firmware assemblies (after one launcher run):
//   mono csc.exe -target:exe -r:build/bin/cache/assemblies/Sce.PlayStation.Core.dll
//        -r:build/bin/cache/assemblies/Sce.PlayStation.Orbis.dll
//        -out:build/bin/cache/assemblies/PadSmoke.exe managed/Tests/PadSmoke.cs
using System;
using Sce.PlayStation.Core.Graphics;
using Sce.PlayStation.Core.Input;
using Sce.PlayStation.Orbis.Input;

static class PadSmoke
{
    static int Main(string[] args)
    {
        int frames = args.Length > 0 ? int.Parse(args[0]) : 1200;
        var gc = new GraphicsContext();
        InputManager.SetDeviceIndexBehavior(DeviceIndexBehavior.MergeAllConnectedDevice, null);
        var info0 = default(ControllerInfo);
        int handle = -1;
        InputManager.GetControllerHandle(0x10001L, ref handle);
        var byId = InputManager.GetControllerInfo(0x10001L);
        Console.WriteLine("PadSmoke: device 0x{0:x} type={1} connected={2} user={3} cap={4} handle={5}",
            byId.DeviceId, byId.Type, byId.Connected, byId.UserId, byId.Capability, handle);
        Console.WriteLine("PadSmoke: focus={0}", InputManager.GetControllerFocusStatus());

        GamePadButtons last = 0;
        float lastLx = 0;
        bool lastPs = false;
        for (int f = 0; f < frames; f++)
        {
            var d = GamePad.GetData(0);
            if (d.Buttons != last || d.AnalogLeftX != lastLx)
            {
                InputManager.GetControllerInfo(0, ref info0);
                Console.WriteLine("PadSmoke: frame {0} Buttons=[{1}] Down=[{2}] Up=[{3}] LX={4} active type={5}",
                    f, d.Buttons, d.ButtonsDown, d.ButtonsUp, d.AnalogLeftX, info0.Type);
                last = d.Buttons;
                lastLx = d.AnalogLeftX;
            }
            var info = default(ControllerInfo);
            var sys = default(SystemButtons);
            var normal = (GamePadButtons)0;
            InputManager.GetSystemButtonsState(0, ref info, ref sys, ref normal);
            if (sys.Ps != lastPs)
            {
                Console.WriteLine("PadSmoke: frame {0} system Ps={1} normal=[{2}] connected={3}", f, sys.Ps, normal, info.Connected);
                lastPs = sys.Ps;
            }
            gc.SetClearColor(0.1f, 0.1f, 0.1f, 1);
            gc.Clear();
            gc.SwapBuffers();
        }
        return 0;
    }
}
