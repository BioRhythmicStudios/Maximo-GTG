// HD textures for the launcher (experimental; ../HD_TEXTURES.md).
//
// Texture packs are never shipped with the game. Marcus' remaster pack is downloaded from archive.org (resumable,
// MD5-checked) or taken from a zip the player already has; any PCSX2-format pack for SLES-50703 can be installed from
// a zip or folder; PNGs dropped into textures\replacements (PCSX2 layout) are converted on request. Each pack is
// converted once by `game\maximo_boot.exe --hdtex-convert` into textures\<folder>\pack.mhtx. The game reads the packs
// in priority order (MAXIMO_HDTEX_PACKS); the player can tick packs on and off and move them up and down.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Net;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;

static class HdTextures
{
    public const string MarcusFolder = "marcus-remaster";
    public const string MarcusName = "Marcus' Ultimate Remaster texture pack";
    public const string MarcusFile = "Maximo Ghosts to Glory Remastered.zip";
    public const string MarcusUrl = "https://archive.org/download/maximo-ghosts-to-glory-remastered/Maximo%20Ghosts%20to%20Glory%20Remastered.zip";
    public const long MarcusSize = 9663353493;
    public const string MarcusMd5 = "fd50b092c122759fe87dd793f2d17cc8";
    public const string DropInFolder = "my-replacements";

    public static readonly string Root = Path.Combine(Paths.Root, "textures");
    public static readonly string Downloads = Path.Combine(Root, "downloads");
    public static readonly string DropIn = Path.Combine(Root, "replacements");

    // Settings (settings.ini): hdtex=0/1, hdtexcap=2048/1024, hdtexdelete=0/1, hdpacks=<folder>:<0/1>,... (priority order).
    public static bool Enabled = false;
    public static int Cap = 2048;
    public static bool DeleteDownload = true;
    public static List<KeyValuePair<string, bool>> Order = new List<KeyValuePair<string, bool>>();

    public static bool Load(string key, string value)
    {
        if (key == "hdtex") Enabled = value == "1";
        else if (key == "hdtexcap") Cap = value == "1024" ? 1024 : 2048;
        else if (key == "hdtexdelete") DeleteDownload = value != "0";
        else if (key == "hdpacks")
        {
            Order.Clear();
            foreach (string item in value.Split(','))
            {
                int colon = item.LastIndexOf(':');
                if (colon > 0) Order.Add(new KeyValuePair<string, bool>(item.Substring(0, colon), item.Substring(colon + 1) != "0"));
            }
        }
        else return false;
        return true;
    }

    public static string SaveLines()
    {
        return "hdtex=" + (Enabled ? 1 : 0) + "\r\nhdtexcap=" + Cap + "\r\nhdtexdelete=" + (DeleteDownload ? 1 : 0) +
               "\r\nhdpacks=" + string.Join(",", Order.Select(kv => kv.Key + ":" + (kv.Value ? 1 : 0))) + "\r\n";
    }

    public class Pack
    {
        public string Folder, Name;
        public int Textures, Skipped, CapUsed;
        public long Bytes;
        public bool On, Old; // Old: converted by an older version; must be installed again
        public string Dir { get { return Path.Combine(Root, Folder); } }
    }

    // Installed packs in priority order (highest first). New packs go on top, except Marcus' pack, which goes to the
    // bottom so players' own packs override it.
    public static List<Pack> Packs()
    {
        var found = new Dictionary<string, Pack>(StringComparer.OrdinalIgnoreCase);
        if (Directory.Exists(Root))
            foreach (string dir in Directory.GetDirectories(Root))
            {
                if (!File.Exists(Path.Combine(dir, "pack.mhtx"))) continue;
                var p = new Pack { Folder = Path.GetFileName(dir), Name = Path.GetFileName(dir), On = true };
                p.Old = PackVersion(Path.Combine(dir, "pack.mhtx")) < PackFormat;
                string info = Path.Combine(dir, "pack.txt");
                if (File.Exists(info))
                    foreach (string line in File.ReadAllLines(info))
                    {
                        string[] kv = line.Split(new[] { '=' }, 2);
                        if (kv.Length != 2) continue;
                        if (kv[0] == "name") p.Name = kv[1];
                        if (kv[0] == "textures") int.TryParse(kv[1], out p.Textures);
                        if (kv[0] == "skipped") int.TryParse(kv[1], out p.Skipped);
                        if (kv[0] == "cap") int.TryParse(kv[1], out p.CapUsed);
                        if (kv[0] == "bytes") long.TryParse(kv[1], out p.Bytes);
                    }
                found[p.Folder] = p;
            }
        var result = new List<Pack>();
        foreach (var kv in Order)
        {
            Pack p;
            if (found.TryGetValue(kv.Key, out p)) { p.On = kv.Value; result.Add(p); found.Remove(kv.Key); }
        }
        foreach (Pack p in found.Values.OrderBy(x => x.Folder))
        {
            if (string.Equals(p.Folder, MarcusFolder, StringComparison.OrdinalIgnoreCase)) result.Add(p);
            else result.Insert(0, p);
        }
        return result;
    }

    // pack.mhtx header: magic, version (hdtex_format.h kVersion).
    public const uint PackFormat = 3;
    static uint PackVersion(string file)
    {
        try
        {
            using (var f = new BinaryReader(File.OpenRead(file)))
            {
                f.ReadUInt32();
                return f.ReadUInt32();
            }
        }
        catch { return 0; }
    }

    public static void SaveOrder(List<Pack> packs)
    {
        Order = packs.Select(p => new KeyValuePair<string, bool>(p.Folder, p.On)).ToList();
        Picture.Save();
    }

    // What the game gets: the ticked packs, highest priority first. Empty when HD textures are off or none is ticked.
    public static string PackList()
    {
        if (!Enabled) return "";
        return string.Join(";", Packs().Where(p => p.On && !p.Old).Select(p => p.Dir));
    }

    public static string Describe()
    {
        if (!Enabled) return "";
        var on = Packs().Where(p => p.On).ToList();
        return on.Count == 0 ? ", HD textures on but no pack ticked" : ", HD textures (" + string.Join(", ", on.Select(p => p.Name)) + ")";
    }

    public static string Size(long bytes)
    {
        return bytes >= (1L << 30) ? (bytes / (double)(1L << 30)).ToString("0.0") + " GB" : (bytes >> 20) + " MB";
    }

    // ---- Converting ----

    static volatile Process converter;
    public static volatile bool CancelRequested;

    public static void Cancel()
    {
        CancelRequested = true;
        Process p = converter;
        try { if (p != null && !p.HasExited) p.Kill(); }
        catch (Exception) { }
    }

    // Runs the converter: source = a zip or a folder with PCSX2 replacement PNGs. Returns "N textures (M skipped)".
    public static string Convert(string source, string folder, string name, Action<string> progress)
    {
        string out_ = Path.Combine(Root, folder);
        Directory.CreateDirectory(out_);
        var info = new ProcessStartInfo(Paths.Game, "--hdtex-convert \"" + source.TrimEnd('\\') + "\" \"" + out_ + "\" --cap " + Cap);
        info.UseShellExecute = false;
        info.CreateNoWindow = true;
        info.RedirectStandardOutput = true;
        info.RedirectStandardError = true;
        Log.Write("hdtex: converting " + source + " -> " + out_ + " (cap " + Cap + ")");
        string done = null, error = null;
        int files = 0;
        var started = DateTime.Now;
        using (var p = new Process())
        {
            p.StartInfo = info;
            p.ErrorDataReceived += (s, e) => { };
            p.Start();
            converter = p;
            p.BeginErrorReadLine();
            string line;
            while ((line = p.StandardOutput.ReadLine()) != null)
            {
                string[] w = line.Split(' ');
                if (w.Length >= 3 && w[0] == "HDTEX" && w[1] == "files") int.TryParse(w[2], out files);
                else if (w.Length >= 4 && w[0] == "HDTEX" && w[1] == "progress")
                {
                    int i, n;
                    if (int.TryParse(w[2], out i) && int.TryParse(w[3], out n) && n > 0)
                    {
                        double secs = (DateTime.Now - started).TotalSeconds;
                        string eta = i > 3 ? ", about " + Math.Max(1, (int)Math.Ceiling(secs / i * (n - i) / 60)) + " min left" : "";
                        progress("Converting textures: " + (100 * i / n) + "% (" + i + " of " + n + ")" + eta);
                    }
                }
                else if (w.Length >= 2 && w[0] == "HDTEX" && w[1] == "done") done = line;
                else if (w.Length >= 2 && w[0] == "HDTEX" && w[1] == "error") error = line.Substring(Math.Min(line.Length, 12));
            }
            p.WaitForExit();
            converter = null;
        }
        if (done == null && !File.Exists(Path.Combine(out_, "pack.mhtx")))
        {
            try { Directory.Delete(out_, true); }
            catch (IOException) { }
        }
        if (CancelRequested) throw new OperationCanceledException("Stopped.");
        if (done == null) throw new Exception("The texture conversion failed" + (error != null ? ": " + error : "."));
        // The pack's display name goes into its pack.txt, which the converter has just written.
        string infoPath = Path.Combine(out_, "pack.txt");
        File.AppendAllText(infoPath, "name=" + name + "\r\n");
        int used = 0, skipped = 0;
        foreach (string part in done.Split(' '))
        {
            if (part.StartsWith("used=")) int.TryParse(part.Substring(5), out used);
            if (part.StartsWith("skipped=")) int.TryParse(part.Substring(8), out skipped);
        }
        Log.Write("hdtex: " + done + " (" + (DateTime.Now - started).TotalMinutes.ToString("0.0") + " min)");
        if (used == 0)
        {
            try { Directory.Delete(out_, true); }
            catch (IOException) { }
            throw new Exception("No usable textures were found (" + skipped + " files skipped). The pack needs PCSX2 " +
                                "replacement names like 1a2b3c4d5e6f7a8b-9c8d7e6f5a4b3c2d-00001e13.png.");
        }
        return used + " textures" + (skipped > 0 ? " (" + skipped + " files skipped: see convert_log.txt in its folder)" : "");
    }

    // ---- Marcus' pack ----

    public static string MarcusZip { get { return Path.Combine(Downloads, MarcusFile); } }
    static string MarcusPart { get { return MarcusZip + ".part"; } }

    public static string MarcusDownloadState()
    {
        if (File.Exists(MarcusZip)) return "Downloaded (" + Size(new FileInfo(MarcusZip).Length) + ", kept in textures\\downloads).";
        if (File.Exists(MarcusPart))
            return "Download paused at " + Size(new FileInfo(MarcusPart).Length) + " of " + Size(MarcusSize) + ": it continues where it stopped.";
        return "";
    }

    static string Md5(string path, Action<string> progress)
    {
        using (var md5 = MD5.Create())
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20))
        {
            var buffer = new byte[4 << 20];
            long done = 0, total = stream.Length;
            int n, lastPct = -1;
            while ((n = stream.Read(buffer, 0, buffer.Length)) > 0)
            {
                if (CancelRequested) throw new OperationCanceledException("Stopped.");
                md5.TransformBlock(buffer, 0, n, null, 0);
                done += n;
                int pct = (int)(100 * done / Math.Max(1, total));
                if (pct != lastPct) { progress("Checking the pack (MD5): " + pct + "%"); lastPct = pct; }
            }
            md5.TransformFinalBlock(buffer, 0, 0);
            return BitConverter.ToString(md5.Hash).Replace("-", "").ToLowerInvariant();
        }
    }

    // True when this zip is Marcus' pack (same size, same MD5).
    public static bool IsMarcusZip(string path, Action<string> progress)
    {
        return new FileInfo(path).Length == MarcusSize && Md5(path, progress) == MarcusMd5;
    }

    // Test hook: MAXIMO_HDTEX_TEST_URL/_SIZE/_MD5 download another file through the same code.
    static string Url { get { return Environment.GetEnvironmentVariable("MAXIMO_HDTEX_TEST_URL") ?? MarcusUrl; } }
    static long ExpectedSize
    {
        get
        {
            long v;
            return long.TryParse(Environment.GetEnvironmentVariable("MAXIMO_HDTEX_TEST_SIZE"), out v) ? v : MarcusSize;
        }
    }
    static string ExpectedMd5 { get { return Environment.GetEnvironmentVariable("MAXIMO_HDTEX_TEST_MD5") ?? MarcusMd5; } }

    // Downloads (or continues downloading) the pack into textures\downloads, then checks its size and MD5.
    public static void Download(string target, Action<string> progress)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(target));
        string part = target + ".part";
        long need = ExpectedSize;
        var drive = new DriveInfo(Path.GetPathRoot(Path.GetFullPath(target)));
        long have = File.Exists(part) ? new FileInfo(part).Length : 0;
        if (drive.AvailableFreeSpace < need - have + (4L << 30))
            throw new Exception("Not enough free disk space on " + drive.Name + ": the download needs " + Size(need - have) +
                                " more, plus about 4 GB for the converted textures.");
        ServicePointManager.SecurityProtocol |= SecurityProtocolType.Tls12;
        for (int attempt = 0; have < need; attempt++)
        {
            if (CancelRequested) throw new OperationCanceledException("Stopped.");
            try
            {
                var request = (HttpWebRequest)WebRequest.Create(Url);
                request.UserAgent = "MaximoLauncher/1.0";
                request.AllowAutoRedirect = true;
                request.ReadWriteTimeout = 60000;
                request.Timeout = 60000;
                if (have > 0) request.AddRange(have);
                Log.Write("hdtex: download " + Url + " from byte " + have);
                using (var response = (HttpWebResponse)request.GetResponse())
                {
                    bool partial = response.StatusCode == HttpStatusCode.PartialContent;
                    if (have > 0 && !partial) { have = 0; Log.Write("hdtex: server ignored the range; starting again"); }
                    using (var input = response.GetResponseStream())
                    using (var output = new FileStream(part, have > 0 ? FileMode.Append : FileMode.Create, FileAccess.Write, FileShare.Read, 1 << 20))
                    {
                        var buffer = new byte[1 << 20];
                        var clock = Stopwatch.StartNew();
                        long since = have;
                        double lastShown = -1;
                        int n;
                        while ((n = input.Read(buffer, 0, buffer.Length)) > 0)
                        {
                            if (CancelRequested) throw new OperationCanceledException("Stopped. The download continues where it stopped next time.");
                            output.Write(buffer, 0, n);
                            have += n;
                            double t = clock.Elapsed.TotalSeconds;
                            if (t - lastShown >= 0.5)
                            {
                                lastShown = t;
                                double rate = (have - since) / Math.Max(0.1, t);
                                string eta = rate > 0 ? ", about " + Math.Max(1, (int)Math.Ceiling((need - have) / rate / 60)) + " min left" : "";
                                progress("Downloading: " + (100 * have / need) + "% (" + Size(have) + " of " + Size(need) + ", " +
                                         (rate / (1 << 20)).ToString("0.0") + " MB/s" + eta + ")");
                            }
                        }
                    }
                }
                attempt = 0;
            }
            catch (OperationCanceledException) { throw; }
            catch (WebException e)
            {
                Log.Write("hdtex: download interrupted at byte " + have + ": " + e.Message);
                if (attempt >= 4) throw new Exception("The download keeps failing (" + e.Message + "). Try again later: it continues where it stopped.");
                progress("Connection lost; trying again...");
                Thread.Sleep(3000 * (attempt + 1));
            }
            catch (IOException e)
            {
                Log.Write("hdtex: download interrupted at byte " + have + ": " + e.Message);
                if (attempt >= 4) throw new Exception("The download keeps failing (" + e.Message + "). Try again later: it continues where it stopped.");
                progress("Connection lost; trying again...");
                Thread.Sleep(3000 * (attempt + 1));
            }
            if (File.Exists(part)) have = new FileInfo(part).Length;
        }
        if (have != need)
        {
            File.Delete(part);
            throw new Exception("The download has the wrong size (" + have + " bytes instead of " + need + "). It was deleted; please try again.");
        }
        string md5 = Md5(part, progress);
        if (md5 != ExpectedMd5)
        {
            File.Delete(part);
            throw new Exception("The download is damaged (MD5 " + md5 + "). It was deleted; please try again.");
        }
        if (File.Exists(target)) File.Delete(target);
        File.Move(part, target);
        Log.Write("hdtex: downloaded and checked " + target);
    }

    public static string InstallMarcus(Action<string> progress)
    {
        CancelRequested = false;
        if (!File.Exists(MarcusZip)) Download(MarcusZip, progress);
        string result = Convert(MarcusZip, MarcusFolder, MarcusName, progress);
        if (DeleteDownload)
        {
            File.Delete(MarcusZip);
            Log.Write("hdtex: deleted the download after installing");
        }
        return result;
    }

    static string FolderFor(string source)
    {
        string name = Path.GetFileNameWithoutExtension(source.TrimEnd('\\'));
        var sb = new StringBuilder();
        foreach (char c in name.ToLowerInvariant()) sb.Append(char.IsLetterOrDigit(c) ? c : '-');
        string folder = "pack-" + sb.ToString().Trim('-');
        if (folder == "pack-") folder = "pack-1";
        if (string.Equals(folder, "pack-" + DropInFolder, StringComparison.OrdinalIgnoreCase)) folder += "-2";
        return folder.Length > 60 ? folder.Substring(0, 60) : folder;
    }

    // Any PCSX2-format pack (a .zip, or a folder searched with its subfolders). Marcus' zip is recognised by size + MD5.
    public static string InstallFrom(string source, Action<string> progress)
    {
        CancelRequested = false;
        if (File.Exists(source) && new FileInfo(source).Length == MarcusSize && IsMarcusZip(source, progress))
            return Convert(source, MarcusFolder, MarcusName, progress);
        string name = Path.GetFileName(source.TrimEnd('\\'));
        return Convert(source, FolderFor(source), name, progress);
    }

    // ---- Drop-in folder (textures\replacements) ----

    static string DropInStamp { get { return Path.Combine(Root, DropInFolder, "source.stamp"); } }

    // The PNG files in textures\replacements and a stamp of them (count, total size, newest change).
    public static string DropInState(out int files)
    {
        files = 0;
        if (!Directory.Exists(DropIn)) return "";
        long bytes = 0, newest = 0;
        foreach (string f in Directory.GetFiles(DropIn, "*.*", SearchOption.AllDirectories))
        {
            if (!f.EndsWith(".png", StringComparison.OrdinalIgnoreCase)) continue;
            var fi = new FileInfo(f);
            files++;
            bytes += fi.Length;
            newest = Math.Max(newest, fi.LastWriteTimeUtc.Ticks);
        }
        return files + ":" + bytes + ":" + newest + ":" + Cap;
    }

    // True when textures\replacements has PNGs that have not been converted yet (or changed since).
    public static bool DropInChanged(out int files)
    {
        string state = DropInState(out files);
        if (files == 0) return false;
        string old = File.Exists(DropInStamp) ? File.ReadAllText(DropInStamp).Trim() : "";
        return old != state;
    }

    public static string ConvertDropIn(Action<string> progress)
    {
        CancelRequested = false;
        int files;
        string state = DropInState(out files);
        if (files == 0) throw new Exception("textures\\replacements has no PNG files yet.");
        string result = Convert(DropIn, DropInFolder, "Your textures (textures\\replacements)", progress);
        File.WriteAllText(DropInStamp, state);
        return result;
    }
}

// The "HD textures" window.
class HdTexturesForm : Form
{
    readonly Label status = new Label(), marcusState = new Label(), dropInState = new Label();
    readonly Button marcus = new Button(), fromZip = new Button(), fromFolder = new Button(), convertDropIn = new Button(),
                    openDropIn = new Button(), up = new Button(), down = new Button(), remove = new Button(), cancel = new Button(),
                    close = new Button();
    readonly ListView list = new ListView();
    readonly ComboBox cap = new ComboBox();
    readonly CheckBox deleteDownload = new CheckBox(), enabled = new CheckBox();
    readonly ProgressBar bar = new ProgressBar();
    List<HdTextures.Pack> packs = new List<HdTextures.Pack>();
    bool busy, filling;

    public HdTexturesForm()
    {
        Text = "HD textures";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(620, 612);
        Font = new Font("Segoe UI", 9.5f);
        const int W = 584;
        int y = 14;

        enabled.Text = "Use HD textures in the game";
        enabled.Font = new Font("Segoe UI", 10.5f, FontStyle.Bold);
        enabled.Checked = HdTextures.Enabled;
        enabled.SetBounds(18, y, W, 26);
        enabled.CheckedChanged += delegate { HdTextures.Enabled = enabled.Checked; Picture.Save(); };
        Controls.Add(enabled);
        y += 30;
        Controls.Add(new Label
        {
            Text = "Replacement textures in PCSX2's format, shown instead of the game's own where they match. They " +
                   "use more graphics memory, and a texture may look wrong or out of place.",
            ForeColor = LauncherForm.Text2, Location = new Point(18, y), Size = new Size(W, 36)
        });
        y += 42;

        var marcusBox = new GroupBox { Text = HdTextures.MarcusName, Location = new Point(18, y), Size = new Size(W, 118) };
        marcusBox.Controls.Add(new Label
        {
            Text = "By Marcus (marcusmart17), free on archive.org: 1339 textures, a 9.7 GB download. It is converted once " +
                   "for the game (about 4 GB, a few minutes).",
            Location = new Point(10, 22), Size = new Size(W - 20, 36)
        });
        marcus.SetBounds(10, 62, 250, 32);
        marcus.Click += delegate { Run(HdTextures.InstallMarcus, "Installed " + HdTextures.MarcusName + ": "); };
        marcusBox.Controls.Add(marcus);
        deleteDownload.Text = "Delete the 9.7 GB download after installing";
        deleteDownload.Checked = HdTextures.DeleteDownload;
        deleteDownload.SetBounds(272, 66, W - 282, 24);
        deleteDownload.CheckedChanged += delegate { HdTextures.DeleteDownload = deleteDownload.Checked; Picture.Save(); };
        marcusBox.Controls.Add(deleteDownload);
        marcusState.SetBounds(10, 96, W - 20, 18);
        marcusState.ForeColor = LauncherForm.Text2;
        marcusBox.Controls.Add(marcusState);
        Controls.Add(marcusBox);
        y += 126;

        var otherBox = new GroupBox { Text = "Other packs and your own textures", Location = new Point(18, y), Size = new Size(W, 120) };
        fromZip.SetBounds(10, 24, 200, 32);
        fromZip.Text = "Install a pack from a zip...";
        fromZip.Click += delegate { ChooseZip(); };
        fromFolder.SetBounds(218, 24, 200, 32);
        fromFolder.Text = "...or from a folder";
        fromFolder.Click += delegate { ChooseFolder(); };
        otherBox.Controls.AddRange(new Control[] { fromZip, fromFolder });
        otherBox.Controls.Add(new Label
        {
            Text = "Your own textures: PNGs with PCSX2 names (SLES-50703) go into textures\\replacements.",
            Location = new Point(10, 60), Size = new Size(W - 20, 20)
        });
        openDropIn.SetBounds(10, 82, 130, 30);
        openDropIn.Text = "Open the folder";
        openDropIn.Click += delegate { Directory.CreateDirectory(HdTextures.DropIn); Process.Start("explorer.exe", "\"" + HdTextures.DropIn + "\""); };
        convertDropIn.SetBounds(148, 82, 120, 30);
        convertDropIn.Text = "Convert them";
        convertDropIn.Click += delegate { Run(HdTextures.ConvertDropIn, "Converted your textures: "); };
        dropInState.SetBounds(276, 88, W - 286, 20);
        dropInState.ForeColor = LauncherForm.Text2;
        otherBox.Controls.AddRange(new Control[] { openDropIn, convertDropIn, dropInState });
        Controls.Add(otherBox);
        y += 128;

        Controls.Add(new Label { Text = "Installed packs (ticked ones are used; a pack higher in the list wins where two have the same texture):", Location = new Point(18, y), Size = new Size(W, 20) });
        y += 22;
        list.View = View.Details;
        list.CheckBoxes = true;
        list.FullRowSelect = true;
        list.HideSelection = false;
        list.MultiSelect = false;
        list.HeaderStyle = ColumnHeaderStyle.Nonclickable;
        list.Columns.Add("Pack", 222);
        list.Columns.Add("Textures", 70);
        list.Columns.Add("Size", 76);
        list.Columns.Add("Largest", 80);
        list.SetBounds(18, y, W - 110, 120);
        list.ItemChecked += delegate(object s, ItemCheckedEventArgs e)
        {
            if (filling) return;
            packs[e.Item.Index].On = e.Item.Checked;
            HdTextures.SaveOrder(packs);
        };
        Controls.Add(list);
        up.SetBounds(18 + W - 100, y, 100, 30);
        up.Text = "Up";
        up.Click += delegate { MovePack(-1); };
        down.SetBounds(18 + W - 100, y + 36, 100, 30);
        down.Text = "Down";
        down.Click += delegate { MovePack(1); };
        remove.SetBounds(18 + W - 100, y + 90, 100, 30);
        remove.Text = "Remove...";
        remove.Click += delegate { Remove(); };
        Controls.AddRange(new Control[] { up, down, remove });
        y += 128;

        Controls.Add(new Label { Text = "Largest texture size:", Location = new Point(18, y + 4), AutoSize = true });
        cap.DropDownStyle = ComboBoxStyle.DropDownList;
        cap.Items.AddRange(new object[] { "2048 pixels (recommended)", "1024 pixels (less graphics memory and disk space)" });
        cap.SelectedIndex = HdTextures.Cap == 1024 ? 1 : 0;
        cap.SetBounds(160, y, 330, 24);
        cap.SelectedIndexChanged += delegate { HdTextures.Cap = cap.SelectedIndex == 1 ? 1024 : 2048; Picture.Save(); };
        Controls.Add(cap);
        Controls.Add(new Label { Text = "(for packs installed from now on)", ForeColor = LauncherForm.Text2, Location = new Point(18, y + 28), AutoSize = true });
        y += 54;

        bar.SetBounds(18, y, W - 110, 22);
        bar.Style = ProgressBarStyle.Marquee;
        bar.Visible = false;
        cancel.SetBounds(18 + W - 100, y - 4, 100, 30);
        cancel.Text = "Stop";
        cancel.Visible = false;
        cancel.Click += delegate { HdTextures.Cancel(); status.Text = "Stopping..."; };
        Controls.AddRange(new Control[] { bar, cancel });
        y += 26;
        status.SetBounds(18, y, W, 40);
        Controls.Add(status);
        close.SetBounds(18 + W - 100, ClientSize.Height - 44, 100, 32);
        close.Text = "Close";
        close.Click += delegate { Close(); };
        Controls.Add(close);
        LauncherForm.Theme(this);
        FormClosing += delegate(object s, FormClosingEventArgs e)
        {
            if (busy && MessageBox.Show(this, "Stop the download or conversion?", Text, MessageBoxButtons.YesNo) != DialogResult.Yes) e.Cancel = true;
            else if (busy) HdTextures.Cancel();
        };
        Fill();
    }

    public void ConvertDropInNow() { Run(HdTextures.ConvertDropIn, "Converted your textures: "); }

    void Fill()
    {
        filling = true;
        packs = HdTextures.Packs();
        list.Items.Clear();
        foreach (var p in packs)
        {
            var item = new ListViewItem(p.Old ? p.Name + " (old format: install it again)" : p.Name) { Checked = p.On };
            item.SubItems.Add(p.Textures.ToString());
            item.SubItems.Add(HdTextures.Size(p.Bytes));
            item.SubItems.Add(p.CapUsed > 0 ? p.CapUsed + " px" : "");
            list.Items.Add(item);
        }
        filling = false;
        bool marcusInstalled = packs.Any(p => string.Equals(p.Folder, HdTextures.MarcusFolder, StringComparison.OrdinalIgnoreCase));
        string dl = HdTextures.MarcusDownloadState();
        marcus.Text = File.Exists(HdTextures.MarcusZip) ? (marcusInstalled ? "Convert it again" : "Install it") :
                      dl.Length > 0 ? "Continue the download" : marcusInstalled ? "Download and install again" : "Download and install";
        marcusState.Text = (marcusInstalled ? "Installed. " : "") + dl;
        int files;
        bool changed = HdTextures.DropInChanged(out files);
        dropInState.Text = files == 0 ? "No PNG files there yet." : files + " PNG files" + (changed ? ", not converted yet." : ", converted.");
        convertDropIn.Enabled = !busy && files > 0;
        up.Enabled = down.Enabled = remove.Enabled = !busy && packs.Count > 0;
    }

    void SetBusy(bool on)
    {
        busy = on;
        foreach (Control c in new Control[] { marcus, fromZip, fromFolder, convertDropIn, openDropIn, up, down, remove, cap, deleteDownload })
            c.Enabled = !on;
        bar.Visible = cancel.Visible = on;
        if (!on) Fill();
    }

    void Run(Func<Action<string>, string> work, string donePrefix)
    {
        SetBusy(true);
        status.ForeColor = LauncherForm.Text1;
        status.Text = "Starting...";
        Action<string> progress = text => BeginInvoke((Action)(() => { status.Text = text; }));
        var t = new Thread(() =>
        {
            string result = null, error = null;
            bool stopped = false;
            try { result = work(progress); }
            catch (OperationCanceledException e) { stopped = true; error = e.Message; }
            catch (Exception e) { error = e.Message; Log.Write("hdtex error: " + e); }
            try
            {
                BeginInvoke((Action)(() =>
                {
                    SetBusy(false);
                    if (error == null)
                    {
                        status.ForeColor = LauncherForm.Good;
                        status.Text = donePrefix + result + "." + (HdTextures.Enabled ? "" : " Tick \"Use HD textures\" to use it.");
                    }
                    else
                    {
                        status.ForeColor = stopped ? LauncherForm.Text1 : LauncherForm.Bad;
                        status.Text = error;
                    }
                }));
            }
            catch (InvalidOperationException) { } // the window was closed
        });
        t.IsBackground = true;
        t.Start();
    }

    void ChooseZip()
    {
        using (var dialog = new OpenFileDialog { Filter = "Texture packs (*.zip)|*.zip", Title = "A PCSX2 texture pack for Maximo (SLES-50703)" })
            if (dialog.ShowDialog(this) == DialogResult.OK)
            {
                string source = dialog.FileName;
                Run(p => HdTextures.InstallFrom(source, p), "Installed " + Path.GetFileName(source) + ": ");
            }
    }

    void ChooseFolder()
    {
        using (var dialog = new FolderBrowserDialog { Description = "The folder with the pack's PNG files (subfolders are searched too)" })
            if (dialog.ShowDialog(this) == DialogResult.OK)
            {
                string source = dialog.SelectedPath;
                Run(p => HdTextures.InstallFrom(source, p), "Installed " + Path.GetFileName(source.TrimEnd('\\')) + ": ");
            }
    }

    int Selected() { return list.SelectedIndices.Count > 0 ? list.SelectedIndices[0] : -1; }

    void MovePack(int step)
    {
        int i = Selected(), j = i + step;
        if (i < 0 || j < 0 || j >= packs.Count) return;
        var p = packs[i];
        packs.RemoveAt(i);
        packs.Insert(j, p);
        HdTextures.SaveOrder(packs);
        Fill();
        list.Items[j].Selected = true;
        list.Focus();
    }

    void Remove()
    {
        int i = Selected();
        if (i < 0) { status.Text = "Choose a pack in the list first."; return; }
        var p = packs[i];
        if (MessageBox.Show(this, "Remove \"" + p.Name + "\" (" + HdTextures.Size(p.Bytes) + ")? Its converted textures are deleted; " +
                                  "the original pack (zip or folder) is not touched.", Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return;
        try { Directory.Delete(p.Dir, true); }
        catch (Exception e) { status.Text = "Could not remove it: " + e.Message; }
        packs.RemoveAt(i);
        HdTextures.SaveOrder(packs);
        Fill();
    }
}
