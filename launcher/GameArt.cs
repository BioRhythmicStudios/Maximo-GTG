// The launcher's banner is built in memory from the player's own files each time it is needed, and it is never saved:
// the logo and first scenes come from their disc, and their F12 screenshots replace the scenes. Nothing of the game's art
// is in this repository. Without a set-up disc the launcher shows a text title.
//
// A .TEX file: 0x20 byte header (u32 palettes at +8, u32 images at +12), palettes * 0x400 bytes, then for each image:
//   u16 log2 width, u16 log2 height, u8 psm (0x13 T8, 1 CT24), u8 mips, u16 flags, u32 top level bytes, u16 kind,
//   u16 palette, pixels. Kind 3 stores a mip chain after the top level. Rows are bottom-up, and colours use the GS range
//   (0x80 = full), so they are doubled here. T8 palettes are in the GS's swizzled order.
using System;
using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Imaging;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;

static class GameArt
{
    // Decodes one image of a .TEX file. keyGain > 0 turns dark pixels transparent, for art drawn on black.
    static Bitmap Load(string file, int index, int keyGain)
    {
        byte[] d = File.ReadAllBytes(file);
        int palettes = BitConverter.ToInt32(d, 8), images = BitConverter.ToInt32(d, 12);
        if (BitConverter.ToUInt32(d, 0) != 0x04030201u || index >= images) throw new InvalidDataException(file);
        int off = 0x20 + palettes * 0x400;
        for (int t = 0; ; t++)
        {
            int w = 1 << BitConverter.ToUInt16(d, off), h = 1 << BitConverter.ToUInt16(d, off + 2);
            int psm = d[off + 4], size = BitConverter.ToInt32(d, off + 8), kind = d[off + 12];
            int palette = BitConverter.ToUInt16(d, off + 14);
            if (t == index) return Decode(d, off + 16, w, h, psm, 0x20 + Math.Min(palette, Math.Max(palettes - 1, 0)) * 0x400, keyGain);
            off += 16 + (kind == 3 ? (((size * 4 + 2) / 3 + 15) & ~15) : size);
        }
    }

    static Bitmap Decode(byte[] d, int px, int w, int h, int psm, int pal, int keyGain)
    {
        var argb = new int[w * h];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
            {
                int r, g, b;
                if (psm == 0x13)
                {
                    int i = d[px + y * w + x];
                    i = (i & ~0x18) | ((i & 8) << 1) | ((i & 0x10) >> 1);
                    r = d[pal + i * 4]; g = d[pal + i * 4 + 1]; b = d[pal + i * 4 + 2];
                }
                else if (psm == 1)
                {
                    int o = px + (y * w + x) * 3;
                    r = d[o]; g = d[o + 1]; b = d[o + 2];
                }
                else throw new InvalidDataException("texture format " + psm);
                r = Math.Min(255, r * 2); g = Math.Min(255, g * 2); b = Math.Min(255, b * 2);
                int a = keyGain > 0 ? Math.Min(255, Math.Max(r, Math.Max(g, b)) * keyGain) : 255;
                argb[(h - 1 - y) * w + x] = (a << 24) | (r << 16) | (g << 8) | b;
            }
        var bitmap = new Bitmap(w, h, PixelFormat.Format32bppArgb);
        BitmapData lockData = bitmap.LockBits(new Rectangle(0, 0, w, h), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        Marshal.Copy(argb, 0, lockData.Scan0, argb.Length);
        bitmap.UnlockBits(lockData);
        return bitmap;
    }

    static void Panel(Graphics g, PointF[] shape, Action draw)
    {
        GraphicsState state = g.Save();
        using (var path = new GraphicsPath())
        {
            path.AddPolygon(shape);
            g.SetClip(path);
            draw();
        }
        g.Restore(state);
    }

    static void Part(Graphics g, Bitmap image, RectangleF to, RectangleF from)
    {
        g.DrawImage(image, to, from, GraphicsUnit.Pixel);
        image.Dispose();
    }

    static void Whole(Graphics g, Bitmap image, float x, float y, float w, float h)
    {
        Part(g, image, new RectangleF(x, y, w, h), new RectangleF(0, 0, image.Width, image.Height));
    }

    // Draws a screenshot over a panel, cropped to fill it. False when the file cannot be read.
    static bool Shot(Graphics g, string file, RectangleF to)
    {
        try
        {
            // Through a stream, so the file stays free for the player to delete.
            using (var stream = new MemoryStream(File.ReadAllBytes(file)))
            using (var image = new Bitmap(stream))
            {
                float scale = Math.Max(to.Width / image.Width, to.Height / image.Height);
                float w = to.Width / scale, h = to.Height / scale;
                g.DrawImage(image, to, new RectangleF((image.Width - w) / 2, (image.Height - h) / 2, w, h), GraphicsUnit.Pixel);
            }
            return true;
        }
        catch (Exception e)
        {
            Log.Write("banner shot " + Path.GetFileName(file) + ": " + e.Message);
            return false;
        }
    }

    // The player's shots, newest first.
    static string[] Shots(string folder)
    {
        if (!Directory.Exists(folder)) return new string[0];
        return Directory.GetFiles(folder, "*.png").OrderByDescending(File.GetLastWriteTimeUtc).Take(2).ToArray();
    }

    // The banner, w x h, or null when the disc's art cannot be read.
    public static Bitmap Banner(string disc, string shotFolder, int w, int h)
    {
        try
        {
            string data = Path.Combine(disc, "PSXDATA");
            string menu = Path.Combine(data, "GAME\\SELECT.TEX"), sky = Path.Combine(data, "GAME\\SEL_SKY.TEX");
            string graveSky = Path.Combine(data, "GRAVE\\G_SKY.TEX"), grave = Path.Combine(data, "GRAVE\\G_INTRO.TEX");
            string title = Path.Combine(data, "GAME2\\COPYR.TEX");
            string[] shots = Shots(shotFolder);
            var banner = new Bitmap(w, h, PixelFormat.Format32bppArgb);
            using (var g = Graphics.FromImage(banner))
            {
                g.InterpolationMode = InterpolationMode.HighQualityBicubic;
                g.SmoothingMode = SmoothingMode.AntiAlias;
                g.Clear(Color.Black);
                float split = h * 0.38f;
                // Left: the player's second newest shot, or the main menu's night, moon and dead trees.
                Panel(g, new[] { new PointF(0, 0), new PointF(500, 0), new PointF(500 - split, h), new PointF(0, h) }, delegate
                {
                    Whole(g, Load(sky, 2, 0), 0, -60, 520, h + 120);
                    Whole(g, Load(sky, 1, 3), 250, 10, 170, 170);
                    Whole(g, Load(menu, 0, 6), -40, 26, 540, h - 20);
                    if (shots.Length > 1) Shot(g, shots[1], new RectangleF(0, 0, 500, h));
                });
                // Right: the player's newest shot, or the Graveyard's sky and gate.
                float rightTop = w - 416;
                Panel(g, new[] { new PointF(rightTop, 0), new PointF(w, 0), new PointF(w, h), new PointF(rightTop - split, h) }, delegate
                {
                    Whole(g, Load(graveSky, 3, 0), w - 600, -130, 620, 540);
                    Whole(g, Load(graveSky, 4, 3), w - 150, 18, 120, 120);
                    Whole(g, Load(graveSky, 1, 3), w - 640, 80, 660, 190);
                    Part(g, Load(grave, 3, 5), new RectangleF(w - 440, 52, 400, 280), new RectangleF(0, 0, 256, 180));
                    if (shots.Length > 0) Shot(g, shots[0], new RectangleF(rightTop - split, 0, w - rightTop + split, h));
                });
                // Centre: the game's own logo.
                using (var glow = new GraphicsPath())
                {
                    glow.AddEllipse(w / 2 - 430, -120, 860, h + 240);
                    using (var brush = new PathGradientBrush(glow) { CenterColor = Color.FromArgb(170, 62, 34, 92), SurroundColors = new[] { Color.FromArgb(0, 0, 0, 0) } })
                        g.FillPath(brush, glow);
                }
                float logoH = h - 14, logoW = logoH * 470f / 270f;
                Part(g, Load(title, 1, 5), new RectangleF((w - logoW) / 2, 8, logoW, logoH), new RectangleF(30, 22, 470, 270));
                using (var gold = new Pen(Color.FromArgb(232, 178, 52), 4))
                {
                    g.DrawLine(gold, 500, 0, 500 - split, h);
                    g.DrawLine(gold, rightTop, 0, rightTop - split, h);
                }
            }
            return banner;
        }
        catch (Exception e)
        {
            Log.Write("banner: " + e.Message);
            return null;
        }
    }
}
