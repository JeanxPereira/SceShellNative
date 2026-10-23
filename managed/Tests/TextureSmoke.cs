// Renderer smoke test: drives the firmware's own Sce.PlayStation.Core (FW 4.03) the way
// the PUI does — a texture from the real Sce.PlayStation.PUI_UI3.rco, decoded by the
// Image API, drawn with libScePsm's embedded Texture shader program — to check the
// host's graphics, resources and imaging layers end to end without the shell around it.
//
//   SceShellNative.exe --firmware <dump> --overlay <dump> --exec TextureSmoke.exe [texture id [rco]]
//
// With an rco argument starting with '/' the file is loaded as an application resource
// (systemCommonPath false), e.g. "/Application/resource/Sce.Vsh.ShellUI.Ime.rco"
// tex_osk_defaultAtlas; any other name is a vsh_asset file (systemCommonPath true).
// Block-compressed images (DDS: Dxt1..Bc7U) are not decoded, as UIImage.FilterIfNeeded
// does; Texture2D(Image) then takes FromImageInstance's compressed branch.
//
// Built against the prepared firmware assemblies (after one launcher run):
//   mono csc.exe -target:exe -r:build/bin/cache/assemblies/Sce.PlayStation.Core.dll
//        -out:build/bin/cache/assemblies/TextureSmoke.exe managed/Tests/TextureSmoke.cs
using System;
using Sce.PlayStation.Core;
using Sce.PlayStation.Core.Graphics;
using Sce.PlayStation.Core.Imaging;
using Sce.PlayStation.Core.Renderer;
using Sce.PlayStation.Core.Resources;

static class TextureSmoke
{
    static int Main(string[] args)
    {
        string id = args.Length > 0 ? args[0] : "iconid_challenges_bronze";
        string rco = args.Length > 1 ? args[1] : null;
        var gc = new GraphicsContext();
        var res = rco != null && rco.StartsWith("/") ? CxmlResources.Load(rco, false, false)
                                                   : CxmlResources.Load(rco ?? "Sce.PlayStation.PUI_UI3.rco", false, true);
        var tr = default(TextureResource);
        res.GetTexture(id, ref tr);
        var img = new Image(tr.Buffer);
        var fmt = img.Format;
        bool compressed = fmt == ImageFormat.Dxt1 || fmt == ImageFormat.Dxt3 || fmt == ImageFormat.Dxt5 || fmt == ImageFormat.Bc4U ||
                          fmt == ImageFormat.Bc5U || fmt == ImageFormat.Bc6U || fmt == ImageFormat.Bc7U;
        if (!compressed)
            img.Decode();
        var tex = new Texture2D(img, false);
        Console.WriteLine("texture {0}: image {1} {2} -> {3}x{4} {5}", id, img.FileFormat, fmt, tex.Width, tex.Height, tex.Format);
        if (compressed)
        {
            var pd = img.ToBuffer();  // GetPixelDataSize + GetPixelData
            Console.WriteLine("GetPixelData: {0} bytes, magic {1}{2}{3}{4}, fourcc {5}{6}{7}{8}", pd.Length, (char)pd[0], (char)pd[1],
                              (char)pd[2], (char)pd[3], (char)pd[84], (char)pd[85], (char)pd[86], (char)pd[87]);
        }

        var sp = ShaderManager.LoadEmbeddedShader(EmbeddedShader.Texture);
        int uWorld = sp.FindUniform("u_WorldMatrix"), uColor = sp.FindUniform("u_Color");
        var world = Matrix4.Ortho(0, gc.Screen.Width, gc.Screen.Height, 0, -1, 1);
        var color = new Vector4(1, 1, 1, 1);
        sp.SetUniformValue(uWorld, ref world);
        sp.SetUniformValue(uColor, ref color);

        // Two copies: native size at (100,100) and 3x at (600,100).
        var vb = new VertexBuffer(8, VertexFormat.Float4, VertexFormat.Float4, VertexFormat.Float2);
        float w = tex.Width, h = tex.Height;
        vb.SetVertices(0, Quad(100, 100, w, h, 600, 100, 3 * w, 3 * h));
        vb.SetVertices(1, new float[] { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 });
        vb.SetVertices(2, new float[] { 0,0, 1,0, 0,1, 1,1, 0,0, 1,0, 0,1, 1,1 });

        for (int frame = 0; frame < 600; frame++)
        {
            gc.SetClearColor(0.10f, 0.12f, 0.18f, 1.0f);
            gc.Clear();
            gc.SetShaderProgram(sp);
            gc.SetVertexBuffer(0, vb);
            gc.SetTexture(0, tex);
            gc.Enable(EnableMode.Blend);
            gc.SetBlendFunc(BlendFuncMode.Add, BlendFuncFactor.SrcAlpha, BlendFuncFactor.OneMinusSrcAlpha);
            gc.DrawArrays(DrawMode.TriangleStrip, 0, 4);
            gc.DrawArrays(DrawMode.TriangleStrip, 4, 4);
            gc.SwapBuffers();
        }
        return 0;
    }

    static float[] Quad(float x0, float y0, float w0, float h0, float x1, float y1, float w1, float h1)
    {
        return new float[] {
            x0, y0, 0, 1,  x0 + w0, y0, 0, 1,  x0, y0 + h0, 0, 1,  x0 + w0, y0 + h0, 0, 1,
            x1, y1, 0, 1,  x1 + w1, y1, 0, 1,  x1, y1 + h1, 0, 1,  x1 + w1, y1 + h1, 0, 1 };
    }
}
