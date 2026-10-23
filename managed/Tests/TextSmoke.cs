// Text smoke test: drives the firmware's own Sce.PlayStation.Core (FW 4.03) Font / Image
// text API the way the PUI does (Font(alias, size, style, weight, attribute),
// GetTextMetrics, GetTextWidth, Image.DrawText into an Rgba8888 image), uploads the image
// as a texture and draws it with libScePsm's embedded Texture program, to check the host's
// text layer (psmhost/libScePsm/text) end to end without the shell around it.
//
//   SceShellNative.exe --firmware <dump> --overlay <dump>... --exec TextSmoke.exe
//        [titleSize itemSize labelSize]
//
// Built against the prepared firmware assemblies (after one launcher run):
//   mono csc.exe -target:exe -r:build/bin/cache/assemblies/Sce.PlayStation.Core.dll
//        -out:build/bin/cache/assemblies/TextSmoke.exe managed/Tests/TextSmoke.cs
using System;
using Sce.PlayStation.Core;
using Sce.PlayStation.Core.Graphics;
using Sce.PlayStation.Core.Imaging;
using Sce.PlayStation.Core.Renderer;

static class TextSmoke
{
    static void Report(Font font, string text)
    {
        CharMetrics[] m = font.GetTextMetrics(text);
        int width = font.GetTextWidth(text);
        Console.WriteLine("[text] {0} ascent={1} descent={2} lineSpacing={3} center={4} width={5} \"{6}\"",
            font, font.Ascent, font.Descent, font.LineSpacing, font.Center, width, text);
        var sb = new System.Text.StringBuilder();
        for (int i = 0; i < m.Length; i++)
            sb.AppendFormat(" {0}:x{1:F2}/w{2:F2}/h{3:F2}/bx{4:F2}/by{5:F2}/adv{6:F2}", text[i], m[i].X, m[i].Width,
                m[i].Height, m[i].HorizontalBearingX, m[i].HorizontalBearingY, m[i].HorizontalAdvance);
        Console.WriteLine("[text]  metrics" + sb);
    }

    static int Main(string[] args)
    {
        int titleSize = args.Length > 0 ? int.Parse(args[0]) : 54;
        int itemSize = args.Length > 1 ? int.Parse(args[1]) : 30;
        int labelSize = args.Length > 2 ? int.Parse(args[2]) : 23;
        var gc = new GraphicsContext();
        int sw = gc.Screen.Width, sh = gc.Screen.Height;

        // Width of the oracle's title string over a range of sizes (to pick the size).
        foreach (FontWeight w in new[] { FontWeight.Light, FontWeight.Medium })
            for (int s = 36; s <= 60; s += 2)
            {
                var f = new Font(FontAlias.System, s, FontStyle.Normal, w, FontAttribute.None);
                Console.WriteLine("[text] scan {0} {1} width={2} ascent={3} descent={4}", w, s,
                    f.GetTextWidth("Select Your Language"), f.Ascent, f.Descent);
                f.Dispose();
            }

        var title = new Font(FontAlias.System, titleSize, FontStyle.Normal, FontWeight.Light, FontAttribute.EnableFontScaleAdjustment | FontAttribute.EmojiMetricsStyleUI3);
        var item = new Font(FontAlias.System, itemSize, FontStyle.Normal, FontWeight.Light, FontAttribute.EmojiMetricsStyleUI3);
        var label = new Font(FontAlias.System, labelSize, FontStyle.Normal, FontWeight.Medium, FontAttribute.EmojiMetricsStyleUI3);
        var italic = new Font(FontAlias.System, itemSize, FontStyle.Italic, FontWeight.Medium, FontAttribute.EmojiMetricsStyleUI3);
        Report(title, "Select Your Language");
        Report(item, "English (United Kingdom)");
        Report(label, "Select");

        // Other entry points the PUI uses: shaped widths (GetTextWidthArray -> direction 2),
        // registered emoji metrics, locale notification, and the unimplemented MeasureTextMetrics.
        int[] adv = item.GetTextWidthArray("English (United Kingdom)");
        Console.WriteLine("[text] GetTextWidthArray: " + string.Join(",", adv));
        Console.WriteLine("[text] GetTextWidth Thai: " + item.GetTextWidth("ภาษาไทย"));
        var reg = new CharMetrics { Width = 40, Height = 32 };
        Font.RegisterCharMetrics(FontAlias.System, 0xEC10, reg, FontAttribute.EmojiMetricsStyleUI3);
        CharMetrics[] em = item.GetTextMetrics("AB");
        Console.WriteLine("[text] registered U+EC10: w={0:F3} h={1:F3} by={2:F3} adv={3:F3}; B.x={4:F3}", em[1].Width, em[1].Height,
            em[1].HorizontalBearingY, em[1].HorizontalAdvance, em[2].X);
        Font.UnregisterCharMetrics(FontAlias.System, 0xEC10, FontAttribute.EmojiMetricsStyleUI3);
        Font.NotifySystemConfigurationUpdate();
        Font.NotifySystemConfigurationUpdate("en-US");
        try { item.MeasureTextMetrics(new GlyphMetrics[4], 0, "abcd"); }
        catch (Exception e) { Console.WriteLine("[text] MeasureTextMetrics: " + e.GetType().Name + " " + e.Message); }

        // One full-screen Rgba8888 image, transparent; strings placed at the oracle's
        // (1280x720) ink positions scaled to 1920x1080 (x * 1.5), top-left = layout origin.
        var img = new Image(ImageMode.Rgba, new ImageSize(sw, sh), new ImageColor(0, 0, 0, 0));
        var white = new ImageColor(255, 255, 255, 255);
        img.DrawText("Select Your Language", white, title, new ImagePosition(696, 82));
        string[] items = { "English (United Kingdom)", "English (United States)", "Español (Latinoamérica)",
                           "Español (España)", "Français (Canada)", "Français (France)", "Indonesia" };
        for (int i = 0; i < items.Length; i++)
            img.DrawText(items[i], white, item, new ImagePosition(660, 290 + i * 98));
        img.DrawText("Select", white, label, new ImagePosition(1592, 930));
        img.DrawText("Console", white, label, new ImagePosition(110, 993));
        img.DrawText("Italic àéîõü 0123456789 ,./:", new ImageColor(255, 200, 120, 255), italic, new ImagePosition(80, 330));
        // Scripts the PUI hands over one visual run at a time (StyledText): Thai (shaped,
        // left to right), Arabic (shaped, right to left), Japanese / Korean (fallback faces).
        var blue = new ImageColor(160, 220, 255, 255);
        img.DrawText("ภาษาไทย", blue, item, new ImagePosition(80, 400));
        img.DrawText("العربية", blue, item, new ImagePosition(80, 460), TextDirection.RTL);
        img.DrawText("日本語 한국어 中文", blue, item, new ImagePosition(80, 520));
        var tex = new Texture2D(img, false);
        Console.WriteLine("[text] texture {0}x{1} {2}", tex.Width, tex.Height, tex.Format);

        var sp = ShaderManager.LoadEmbeddedShader(EmbeddedShader.Texture);
        int uWorld = sp.FindUniform("u_WorldMatrix"), uColor = sp.FindUniform("u_Color");
        var world = Matrix4.Ortho(0, sw, sh, 0, -1, 1);
        var color = new Vector4(1, 1, 1, 1);
        sp.SetUniformValue(uWorld, ref world);
        sp.SetUniformValue(uColor, ref color);
        var vb = new VertexBuffer(4, VertexFormat.Float4, VertexFormat.Float4, VertexFormat.Float2);
        vb.SetVertices(0, new float[] { 0, 0, 0, 1, sw, 0, 0, 1, 0, sh, 0, 1, sw, sh, 0, 1 });
        vb.SetVertices(1, new float[] { 1,1,1,1, 1,1,1,1, 1,1,1,1, 1,1,1,1 });
        vb.SetVertices(2, new float[] { 0,0, 1,0, 0,1, 1,1 });

        for (int frame = 0; frame < 120; frame++)
        {
            gc.SetClearColor(0.10f, 0.12f, 0.18f, 1.0f);
            gc.Clear();
            gc.SetShaderProgram(sp);
            gc.SetVertexBuffer(0, vb);
            gc.SetTexture(0, tex);
            gc.Enable(EnableMode.Blend);
            gc.SetBlendFunc(BlendFuncMode.Add, BlendFuncFactor.SrcAlpha, BlendFuncFactor.OneMinusSrcAlpha);
            gc.DrawArrays(DrawMode.TriangleStrip, 0, 4);
            gc.SwapBuffers();
        }
        return 0;
    }
}
