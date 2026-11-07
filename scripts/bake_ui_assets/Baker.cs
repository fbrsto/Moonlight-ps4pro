// Rasterizes the menu font and the official Moonlight icons (assets-src/) into
// src/ui/ui_assets.{c,h}. Run with scripts/bake_ui_assets/bake.ps1 on Windows.
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.Drawing.Text;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

public class BakedGlyph { public int W, H, XOff, YOff, Advance, Offset; }
public class BakedFont { public string Name; public int Px, LineH; public BakedGlyph[] Glyphs; public byte[] Data; }
public class BakedIcon { public string Name; public int W, H; public byte[] Mask; }

public static class Baker {
    public static List<BakedFont> Fonts = new List<BakedFont>();
    public static List<BakedIcon> Icons = new List<BakedIcon>();
    public static byte[] NoAppPng;

    static readonly string[,] FontSpec = {
        // About 20% above the official sizes, for reading at TV distance.
        { "name", "48" },  // PC names
        { "app", "32" },   // app names on placeholder art
        { "title", "32" }, // toolbar title and empty states
        { "body", "20" },  // settings
        { "small", "17" }, // settings descriptions
    };

    // name, file, pixel size. "check" is Material's check mark, absent from app/res.
    static readonly string[,] IconSpec = {
        { "back", "arrow_left.svg", "56" },
        { "settings", "settings.svg", "56" },
        { "add", "ic_add_to_queue_white_48px.svg", "56" },
        { "desktop", "desktop_windows-48px.svg", "200" },
        { "lock", "baseline-lock-24px.svg", "70" },
        { "warning", "warning_FILL1_wght300_GRAD200_opsz24.svg", "75" },
        { "play", "play_arrow_FILL1_wght700_GRAD200_opsz48.svg", "75" },
        { "stop", "stop_FILL1_wght700_GRAD200_opsz48.svg", "75" },
    };
    const string CheckPath = "M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z";

    public static void Bake(string assetsDir) {
        Fonts.Clear(); Icons.Clear();
        var pfc = new PrivateFontCollection();
        pfc.AddFontFile(Path.Combine(assetsDir, "Roboto-Regular.ttf"));
        for (int i = 0; i < FontSpec.GetLength(0); i++)
            Fonts.Add(BakeFont(pfc.Families[0], FontSpec[i, 0], int.Parse(FontSpec[i, 1])));
        for (int i = 0; i < IconSpec.GetLength(0); i++)
            Icons.Add(BakeSvg(IconSpec[i, 0], Path.Combine(assetsDir, "icons", IconSpec[i, 1]), int.Parse(IconSpec[i, 2])));
        Icons.Add(RasterIcon("check", new List<string> { CheckPath }, 0, 0, 24, 20));
        NoAppPng = File.ReadAllBytes(Path.Combine(assetsDir, "icons", "no_app_image.png"));
    }

    static byte[] ReadCoverage(Bitmap bmp) {
        var data = new byte[bmp.Width * bmp.Height];
        var bd = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height), ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        var row = new byte[bmp.Width * 4];
        for (int y = 0; y < bmp.Height; y++) {
            System.Runtime.InteropServices.Marshal.Copy(new IntPtr(bd.Scan0.ToInt64() + y * bd.Stride), row, 0, row.Length);
            for (int x = 0; x < bmp.Width; x++) data[y * bmp.Width + x] = row[x * 4 + 2];
        }
        bmp.UnlockBits(bd);
        return data;
    }

    static BakedFont BakeFont(FontFamily fam, string name, int px) {
        var font = new BakedFont { Name = name, Px = px };
        float em = fam.GetEmHeight(FontStyle.Regular);
        font.LineH = (int)Math.Ceiling(fam.GetLineSpacing(FontStyle.Regular) * px / em);
        var glyphs = new List<BakedGlyph>();
        var blob = new List<byte>();
        var fmt = (StringFormat)StringFormat.GenericTypographic.Clone();
        fmt.FormatFlags |= StringFormatFlags.MeasureTrailingSpaces;
        using (var measureBmp = new Bitmap(4, 4))
        using (var mg = Graphics.FromImage(measureBmp))
        using (var f = new Font(fam, px, FontStyle.Regular, GraphicsUnit.Pixel)) {
            mg.TextRenderingHint = TextRenderingHint.AntiAlias;
            var cps = new List<int>();
            for (int c = 32; c <= 126; c++) cps.Add(c);
            for (int c = 160; c <= 255; c++) cps.Add(c);
            foreach (int cp in cps) {
                string s = ((char)cp).ToString();
                var g = new BakedGlyph { Offset = blob.Count };
                g.Advance = (int)Math.Round(mg.MeasureString(s, f, 4096, fmt).Width);
                using (var path = new GraphicsPath()) {
                    path.AddString(s, fam, (int)FontStyle.Regular, px, new PointF(0, 0), fmt);
                    if (path.PointCount > 0) {
                        var b = path.GetBounds();
                        g.XOff = (int)Math.Floor(b.Left); g.YOff = (int)Math.Floor(b.Top);
                        g.W = (int)Math.Ceiling(b.Right) - g.XOff + 1;
                        g.H = (int)Math.Ceiling(b.Bottom) - g.YOff + 1;
                        using (var bmp = new Bitmap(g.W, g.H, PixelFormat.Format32bppArgb))
                        using (var gr = Graphics.FromImage(bmp)) {
                            gr.Clear(Color.Black);
                            gr.SmoothingMode = SmoothingMode.AntiAlias;
                            gr.PixelOffsetMode = PixelOffsetMode.HighQuality;
                            gr.TranslateTransform(-g.XOff, -g.YOff);
                            gr.FillPath(Brushes.White, path);
                            blob.AddRange(ReadCoverage(bmp));
                        }
                    }
                }
                glyphs.Add(g);
            }
        }
        font.Glyphs = glyphs.ToArray();
        font.Data = blob.ToArray();
        return font;
    }

    static BakedIcon BakeSvg(string name, string file, int size) {
        string svg = File.ReadAllText(file);
        var vb = Regex.Match(svg, "viewBox=\"([^\"]+)\"").Groups[1].Value.Split(' ');
        float minX = float.Parse(vb[0], CultureInfo.InvariantCulture), minY = float.Parse(vb[1], CultureInfo.InvariantCulture);
        float vbW = float.Parse(vb[2], CultureInfo.InvariantCulture);
        var paths = new List<string>();
        foreach (Match m in Regex.Matches(svg, "<path[^>]*>")) {
            if (m.Value.Contains("fill=\"none\"")) continue; // the icon's bounding box
            paths.Add(Regex.Match(m.Value, " d=\"([^\"]+)\"").Groups[1].Value);
        }
        return RasterIcon(name, paths, minX, minY, vbW, size);
    }

    static BakedIcon RasterIcon(string name, List<string> paths, float minX, float minY, float vbW, int size) {
        using (var bmp = new Bitmap(size, size, PixelFormat.Format32bppArgb))
        using (var gr = Graphics.FromImage(bmp)) {
            gr.Clear(Color.Black);
            gr.SmoothingMode = SmoothingMode.AntiAlias;
            gr.PixelOffsetMode = PixelOffsetMode.HighQuality;
            float k = size / vbW;
            gr.ScaleTransform(k, k);
            gr.TranslateTransform(-minX, -minY);
            foreach (string d in paths)
                using (var p = ParsePath(d)) gr.FillPath(Brushes.White, p);
            return new BakedIcon { Name = name, W = size, H = size, Mask = ReadCoverage(bmp) };
        }
    }

    // SVG path data: M L H V C S Q T Z, absolute and relative. No icon here uses arcs.
    static GraphicsPath ParsePath(string d) {
        var path = new GraphicsPath(FillMode.Winding);
        float x = 0, y = 0, sx = 0, sy = 0, cx = 0, cy = 0;
        char prev = ' ';
        foreach (Match seg in Regex.Matches(d, "([MmLlHhVvCcSsQqTtZz])([^MmLlHhVvCcSsQqTtZz]*)")) {
            char cmd = seg.Groups[1].Value[0];
            var nums = new List<float>();
            foreach (Match n in Regex.Matches(seg.Groups[2].Value, @"-?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"))
                nums.Add(float.Parse(n.Value, CultureInfo.InvariantCulture));
            bool rel = char.IsLower(cmd);
            char op = char.ToUpperInvariant(cmd);
            if (op == 'Z') { path.CloseFigure(); x = sx; y = sy; prev = 'Z'; continue; }
            int argc = op == 'H' || op == 'V' ? 1 : op == 'M' || op == 'L' || op == 'T' ? 2 : op == 'C' ? 6 : 4;
            for (int i = 0; i + argc <= nums.Count; i += argc) {
                float ox = rel ? x : 0, oy = rel ? y : 0;
                switch (op) {
                case 'M':
                    if (i == 0) { path.StartFigure(); x = nums[0] + ox; y = nums[1] + oy; sx = x; sy = y; }
                    else goto case 'L'; // extra pairs after M are line-tos
                    break;
                case 'L': { float nx = nums[i] + ox, ny = nums[i + 1] + oy; path.AddLine(x, y, nx, ny); x = nx; y = ny; break; }
                case 'H': { float nx = nums[i] + ox; path.AddLine(x, y, nx, y); x = nx; break; }
                case 'V': { float ny = nums[i] + oy; path.AddLine(x, y, x, ny); y = ny; break; }
                case 'C': {
                    float c1x = nums[i] + ox, c1y = nums[i + 1] + oy; cx = nums[i + 2] + ox; cy = nums[i + 3] + oy;
                    float nx = nums[i + 4] + ox, ny = nums[i + 5] + oy;
                    path.AddBezier(x, y, c1x, c1y, cx, cy, nx, ny); x = nx; y = ny; break;
                }
                case 'S': {
                    float c1x = prev == 'C' || prev == 'S' ? 2 * x - cx : x, c1y = prev == 'C' || prev == 'S' ? 2 * y - cy : y;
                    cx = nums[i] + ox; cy = nums[i + 1] + oy;
                    float nx = nums[i + 2] + ox, ny = nums[i + 3] + oy;
                    path.AddBezier(x, y, c1x, c1y, cx, cy, nx, ny); x = nx; y = ny; break;
                }
                case 'Q': case 'T': {
                    float qx, qy, nx, ny;
                    if (op == 'Q') { qx = nums[i] + ox; qy = nums[i + 1] + oy; nx = nums[i + 2] + ox; ny = nums[i + 3] + oy; }
                    else { qx = prev == 'Q' || prev == 'T' ? 2 * x - cx : x; qy = prev == 'Q' || prev == 'T' ? 2 * y - cy : y; nx = nums[i] + ox; ny = nums[i + 1] + oy; }
                    path.AddBezier(x, y, x + 2f / 3 * (qx - x), y + 2f / 3 * (qy - y),
                                   nx + 2f / 3 * (qx - nx), ny + 2f / 3 * (qy - ny), nx, ny);
                    cx = qx; cy = qy; x = nx; y = ny; break;
                }
                }
                prev = op;
            }
        }
        return path;
    }

    static void AppendBytes(StringBuilder sb, byte[] data) {
        for (int i = 0; i < data.Length; i++) {
            sb.Append(data[i]).Append(',');
            if (i % 64 == 63) sb.Append('\n');
        }
    }

    public static void WriteC(string dir) {
        var h = new StringBuilder();
        h.Append("// Generated by scripts/bake_ui_assets; do not edit.\n#pragma once\n\n#include \"ui_draw.h\"\n\n");
        var c = new StringBuilder();
        c.Append("// Generated by scripts/bake_ui_assets; do not edit.\n// Roboto: Apache 2.0. Icons: moonlight-qt app/res, GPLv3.\n#include \"ui_assets.h\"\n\n");
        foreach (var f in Fonts) {
            h.AppendFormat("extern const ui_font_t ui_font_{0}; /* {1} px */\n", f.Name, f.Px);
            c.AppendFormat("static const unsigned char font_{0}_data[] = {{\n", f.Name);
            AppendBytes(c, f.Data);
            c.AppendFormat("}};\nstatic const ui_glyph_t font_{0}_glyphs[] = {{\n", f.Name);
            foreach (var g in f.Glyphs)
                c.AppendFormat("{{{0},{1},{2},{3},{4},{5}}},\n", g.W, g.H, g.XOff, g.YOff, g.Advance, g.Offset);
            c.AppendFormat("}};\nconst ui_font_t ui_font_{0} = {{ {1}, font_{0}_glyphs, font_{0}_data }};\n\n", f.Name, f.LineH);
        }
        h.Append('\n');
        foreach (var i in Icons) {
            h.AppendFormat("extern const ui_icon_t ui_icon_{0};\n", i.Name);
            c.AppendFormat("static const unsigned char icon_{0}_mask[] = {{\n", i.Name);
            AppendBytes(c, i.Mask);
            c.AppendFormat("}};\nconst ui_icon_t ui_icon_{0} = {{ {1}, {2}, icon_{0}_mask }};\n\n", i.Name, i.W, i.H);
        }
        h.AppendFormat("\nextern const unsigned char ui_no_app_png[];\n#define UI_NO_APP_PNG_SIZE {0}\n", NoAppPng.Length);
        c.Append("const unsigned char ui_no_app_png[] = {\n");
        AppendBytes(c, NoAppPng);
        c.Append("};\n");
        File.WriteAllText(Path.Combine(dir, "ui_assets.h"), h.ToString());
        File.WriteAllText(Path.Combine(dir, "ui_assets.c"), c.ToString());
    }
}

