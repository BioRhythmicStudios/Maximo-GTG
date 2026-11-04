// Maximo: Ghosts to Glory (PAL SLES-50703) native test build: a minimal launcher.
//
// It never contains game data. The player points it once at their own disc image (or an extracted copy of it); the
// launcher checks that it is the PAL disc (SHA-256 of the game executable), copies the disc files into .\disc, and then
// starts game\maximo_boot.exe with the same settings as the developer's play script (GPU renderer, 50 Hz, native audio,
// persistent memory card in .\memcard\mc0). Each session's logs go to .\logs\session-<date>-<time>.
//
// Built with the C# 5 compiler that ships with Windows (.NET Framework 4.8): see build.cmd.
// Experimental build: the Picture setting adds the enhance branch's upscaling (MAXIMO_SSAA, PS2X_PRESENT_FILTER).
// HD textures (experimental, HdTextures.cs): packs in .\textures, MAXIMO_HDTEX_PACKS for the game.
// Hidden switches for automated testing: --setup <iso or folder>, --play <seconds>, --zip <notes>, --screenshot <png> [page],
// --hdtex-install <zip or folder>, --hdtex-marcus, --hdtex-screenshot <png>. Results go to launcher.log.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;

static class Build
{
    public const string Name = "Maximo: Ghosts to Glory - PC port";
    public const string Version = "Maximo-GTG source build";
    public const string ElfName = "SLES_507.03";
    public const string ElfSha256 = "03615557E6291B882A54165E068F653C8613856852E4250F7DE4B075ABFC3C99";
    public const int DiscFileCount = 294;
    public const int KeepSessions = 10;
}

static class Paths
{
    public static readonly string Root = AppDomain.CurrentDomain.BaseDirectory.TrimEnd('\\');
    public static readonly string Game = Path.Combine(Root, "game\\maximo_boot.exe");
    public static readonly string Disc = Path.Combine(Root, "disc");
    public static readonly string MemoryCard = Path.Combine(Root, "memcard\\mc0");
    public static readonly string Logs = Path.Combine(Root, "logs");
    public static readonly string Screenshots = Path.Combine(Root, "screenshots");
    public static readonly string LauncherLog = Path.Combine(Root, "launcher.log");
}

// The picture settings of this experimental build, saved in settings.ini next to the launcher.
static class Picture
{
    public static readonly string[] Names =
    {
        "Original: 640x512, the exact PS2 picture",
        "Sharper: double resolution (1280x1024), 4x supersampling",
        "Sharper: double resolution (1280x1024), 8x supersampling",
        "Sharpest: double resolution (1280x1024), 16x supersampling",
        "Ultra: 4x resolution (2560x1024), 16x samples",
        "Ultra + SMAA: 4x resolution, 16x samples, smoothed edges (new)",
    };
    // Ultra (4 and 5) scans out one sample per pixel (MAXIMO_SCANOUT=4); 5 adds SMAA anti-aliasing (MAXIMO_SMAA=1).
    static readonly string[] Ssaa = { null, "4", "8", "16", "16", "16" };
    public static int Mode = 3;
    public static bool Smooth = true;
    // How the picture is scaled to the window: sharp pixels, smooth (bilinear), or AMD FSR 1 on the graphics card
    // (MAXIMO_FSR=1: edge-adaptive upscaling plus sharpening to the exact window size). Saved as smooth= and fsr=.
    public static readonly string[] ScalingNames = { "Sharp pixels", "Smooth", "FSR (sharpest)" };
    public static bool Fsr = false;
    public static int Scaling { get { return Fsr ? 2 : Smooth ? 1 : 0; } set { Fsr = value == 2; Smooth = value != 0; } }
    // Experimental: VU1 (the 3D geometry unit) and the GS submission on their own thread (PS2X_VU1_THREAD). Off by default.
    public static bool Vu1Thread = false;
    // Experimental, with Faster 3D only: the 3D worker draws and shows frame N while the game works on frame N+1
    // (PS2X_VU1_PIPELINE). On by default.
    public static bool Vu1Pipeline = true;
    // Borderless fullscreen on the current monitor (PS2X_WINDOW_MODE=borderless); Alt+Enter switches in the game. Off by default.
    public static bool Borderless = false;
    // Smooth effects (MAXIMO_SOFT_PARTICLES=1, with supersampling only): fire, smoke and sparkle particles are drawn at
    // the full upscaled resolution instead of in blocks of one PS2 pixel. On by default.
    public static bool SoftParticles = true;
    // VSync (PS2X_VSYNC=1): the window swaps on the monitor's refresh, so the picture does not tear. On by default.
    public static bool VSync = true;
    // Experimental widescreen (MAXIMO_ASPECT=16:9): the 3D view shows more at the sides. 4:3 by default.
    public static readonly string[] AspectNames = { "4:3 (original)", "16:9 widescreen (experimental)" };
    static readonly string[] AspectValues = { "4:3", "16:9" };
    public static int Aspect = 0;
    // Experimental 60 Hz gameplay (MAXIMO_FRAME_RATE=60): the same game speed with more frames; the front end, attract
    // demo and movies stay at 50 Hz. 50 Hz by default.
    public static readonly string[] FrameRateNames = { "50 Hz (original)", "60 Hz in gameplay (experimental)" };
    static readonly string[] FrameRateValues = { "50", "60" };
    public static int FrameRate = 0;
    static string File_ { get { return Path.Combine(Paths.Root, "settings.ini"); } }

    public static void Load()
    {
        try
        {
            if (!File.Exists(File_)) return;
            foreach (string line in File.ReadAllLines(File_))
            {
                string[] kv = line.Split(new[] { '=' }, 2);
                if (kv.Length != 2) continue;
                int n;
                if (kv[0].Trim() == "picture" && int.TryParse(kv[1].Trim(), out n) && n >= 0 && n < Names.Length) Mode = n;
                if (kv[0].Trim() == "smooth") Smooth = kv[1].Trim() != "0";
                if (kv[0].Trim() == "fsr") Fsr = kv[1].Trim() == "1";
                if (kv[0].Trim() == "vu1thread") Vu1Thread = kv[1].Trim() == "1";
                if (kv[0].Trim() == "vu1pipeline") Vu1Pipeline = kv[1].Trim() != "0";
                if (kv[0].Trim() == "borderless") Borderless = kv[1].Trim() == "1";
                if (kv[0].Trim() == "softfx") SoftParticles = kv[1].Trim() != "0";
                if (kv[0].Trim() == "vsync") VSync = kv[1].Trim() != "0";
                if (kv[0].Trim() == "aspect") Aspect = Math.Max(0, Array.IndexOf(AspectValues, kv[1].Trim()));
                if (kv[0].Trim() == "framerate") FrameRate = Math.Max(0, Array.IndexOf(FrameRateValues, kv[1].Trim()));
                HdTextures.Load(kv[0].Trim(), kv[1].Trim());
            }
        }
        catch (IOException) { }
    }

    public static void Save()
    {
        try { File.WriteAllText(File_, "picture=" + Mode + "\r\nsmooth=" + (Smooth ? 1 : 0) + "\r\nfsr=" + (Fsr ? 1 : 0) +
                                        "\r\nvu1thread=" + (Vu1Thread ? 1 : 0) +
                                        "\r\nvu1pipeline=" + (Vu1Pipeline ? 1 : 0) +
                                        "\r\nborderless=" + (Borderless ? 1 : 0) +
                                        "\r\nsoftfx=" + (SoftParticles ? 1 : 0) +
                                        "\r\nvsync=" + (VSync ? 1 : 0) +
                                        "\r\naspect=" + AspectValues[Aspect] +
                                        "\r\nframerate=" + FrameRateValues[FrameRate] + "\r\n" + HdTextures.SaveLines()); }
        catch (IOException) { }
    }

    public static string Describe()
    {
        return (Ssaa[Mode] == null ? "original resolution" : (Mode >= 4 ? "4x resolution, " : "2x resolution, ") + Ssaa[Mode] + "x SSAA" + (Mode == 5 ? ", SMAA" : "")) +
               (Fsr ? ", FSR scaling" : Smooth ? ", smooth scaling" : ", sharp scaling") + (Vu1Thread ? ", 3D on a second CPU core" + (Vu1Pipeline ? " with overlapped frames" : "") : "") +
               (Borderless ? ", borderless fullscreen" : "") + (SoftParticles && Ssaa[Mode] != null ? ", smooth effects" : "") + (VSync ? ", VSync" : "") + (Aspect == 1 ? ", 16:9 widescreen" : "") +
               (FrameRate == 1 ? ", 60 Hz gameplay" : "") + HdTextures.Describe();
    }

    // The settings of the developer's play_enhanced.cmd; a value already in the environment wins.
    public static void Apply(ProcessStartInfo info)
    {
        if (Ssaa[Mode] != null && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MAXIMO_SSAA")))
            info.EnvironmentVariables["MAXIMO_SSAA"] = Ssaa[Mode];
        if (Mode >= 4 && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MAXIMO_SCANOUT")))
            info.EnvironmentVariables["MAXIMO_SCANOUT"] = "4";
        if (Mode == 5 && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MAXIMO_SMAA")))
            info.EnvironmentVariables["MAXIMO_SMAA"] = "1";
        if (Smooth && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PS2X_PRESENT_FILTER")))
            info.EnvironmentVariables["PS2X_PRESENT_FILTER"] = "linear";
        if (Fsr && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MAXIMO_FSR")))
            info.EnvironmentVariables["MAXIMO_FSR"] = "1";
        if (Vu1Thread && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PS2X_VU1_THREAD")))
            info.EnvironmentVariables["PS2X_VU1_THREAD"] = "1";
        if (Vu1Thread && Vu1Pipeline && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PS2X_VU1_PIPELINE")))
            info.EnvironmentVariables["PS2X_VU1_PIPELINE"] = "1";
        if (Borderless && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PS2X_WINDOW_MODE")))
            info.EnvironmentVariables["PS2X_WINDOW_MODE"] = "borderless";
        info.EnvironmentVariables["PS2X_SHOT_DIR"] = Paths.Screenshots;
        if (VSync && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PS2X_VSYNC")))
            info.EnvironmentVariables["PS2X_VSYNC"] = "1";
        if (SoftParticles && Ssaa[Mode] != null && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MAXIMO_SOFT_PARTICLES")))
            info.EnvironmentVariables["MAXIMO_SOFT_PARTICLES"] = "1";
        if (Aspect == 1 && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MAXIMO_ASPECT")))
            info.EnvironmentVariables["MAXIMO_ASPECT"] = "16:9";
        if (FrameRate == 1 && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MAXIMO_FRAME_RATE")))
            info.EnvironmentVariables["MAXIMO_FRAME_RATE"] = "60";
        string packs = HdTextures.PackList();
        if (packs.Length > 0 && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("MAXIMO_HDTEX_PACKS")))
            info.EnvironmentVariables["MAXIMO_HDTEX_PACKS"] = packs;
    }
}

static class Log
{
    static readonly object Gate = new object();
    public static void Write(string text)
    {
        lock (Gate)
        {
            try { File.AppendAllText(Paths.LauncherLog, DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + "  " + text + "\r\n"); }
            catch (IOException) { }
        }
    }
}

static class Setup
{
    public static string Sha256(string path)
    {
        using (var sha = SHA256.Create())
        using (var stream = File.OpenRead(path))
            return BitConverter.ToString(sha.ComputeHash(stream)).Replace("-", "");
    }

    public static bool DiscReady(out string why)
    {
        string elf = Path.Combine(Paths.Disc, Build.ElfName);
        if (!File.Exists(elf)) { why = "Game files not set up yet."; return false; }
        int count = Directory.GetFiles(Paths.Disc, "*", SearchOption.AllDirectories).Length;
        if (count != Build.DiscFileCount)
        {
            why = "The game folder is incomplete (" + count + " of " + Build.DiscFileCount + " files). Set it up again.";
            return false;
        }
        why = "Game files ready (PAL SLES-50703).";
        return true;
    }

    static string PowerShell(string script)
    {
        var info = new ProcessStartInfo("powershell.exe",
            "-NoProfile -NonInteractive -Command \"" + script.Replace("\"", "\\\"") + "\"");
        info.UseShellExecute = false;
        info.CreateNoWindow = true;
        info.RedirectStandardOutput = true;
        info.RedirectStandardError = true;
        using (var p = Process.Start(info))
        {
            string output = p.StandardOutput.ReadToEnd();
            string error = p.StandardError.ReadToEnd();
            p.WaitForExit();
            if (p.ExitCode != 0)
                throw new Exception("Windows could not open the disc image: " + error.Trim());
            return output;
        }
    }

    static string Quote(string path) { return "'" + path.Replace("'", "''") + "'"; }

    // Copies a verified disc (an ISO, which Windows mounts read-only, or an extracted folder) into .\disc.
    public static void Install(string source, Action<string> progress)
    {
        bool isIso = File.Exists(source) && source.EndsWith(".iso", StringComparison.OrdinalIgnoreCase);
        bool mountedHere = false;
        string root = source;
        if (isIso)
        {
            progress("Opening the disc image...");
            string output = PowerShell(
                "$i = Get-DiskImage -ImagePath " + Quote(source) + "; $m = 0; " +
                "if (-not $i.Attached) { $i = Mount-DiskImage -ImagePath " + Quote(source) + " -Access ReadOnly -PassThru; $m = 1 }; " +
                "Start-Sleep -Milliseconds 500; 'MOUNTED=' + $m; 'DRIVE=' + ($i | Get-Volume).DriveLetter");
            foreach (string line in output.Split('\n'))
            {
                string l = line.Trim();
                if (l == "MOUNTED=1") mountedHere = true;
                if (l.StartsWith("DRIVE=") && l.Length > 6) root = l.Substring(6) + ":\\";
            }
            if (root == source)
                throw new Exception("Windows mounted the image but gave it no drive letter.");
        }
        else if (!Directory.Exists(source))
            throw new Exception("Choose a .iso file or a folder with the extracted disc.");

        try
        {
            string elf = Path.Combine(root, Build.ElfName);
            if (!File.Exists(elf))
                throw new Exception("This is not the PAL Maximo disc: " + Build.ElfName + " is missing. " +
                                    "This build needs Maximo: Ghosts to Glory, Europe (SLES-50703).");
            progress("Checking the disc...");
            string hash = Sha256(elf);
            Log.Write("disc source " + source + " (" + root + "), " + Build.ElfName + " sha256 " + hash);
            if (!string.Equals(hash, Build.ElfSha256, StringComparison.OrdinalIgnoreCase))
                throw new Exception("This disc's game executable does not match the PAL release (SLES-50703, v1.00). " +
                                    "Other versions are not supported by this build.");

            string[] files = Directory.GetFiles(root, "*", SearchOption.AllDirectories);
            string temp = Paths.Disc + ".partial";
            if (Directory.Exists(temp)) Directory.Delete(temp, true);
            long total = files.Sum(f => new FileInfo(f).Length), done = 0;
            for (int i = 0; i < files.Length; ++i)
            {
                string relative = files[i].Substring(root.Length).TrimStart('\\');
                string target = Path.Combine(temp, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                File.Copy(files[i], target, true);
                File.SetAttributes(target, FileAttributes.Normal); // files copied from a disc are read-only
                done += new FileInfo(target).Length;
                progress("Copying game files: " + (100 * done / Math.Max(1, total)) + "% (" + (i + 1) + " of " + files.Length + ")");
            }
            if (Directory.Exists(Paths.Disc)) Directory.Delete(Paths.Disc, true);
            Directory.Move(temp, Paths.Disc);
            string why;
            if (!DiscReady(out why))
                throw new Exception(why);
            Log.Write("disc installed: " + files.Length + " files, " + total + " bytes");
        }
        finally
        {
            if (mountedHere)
            {
                try { PowerShell("Dismount-DiskImage -ImagePath " + Quote(source) + " | Out-Null"); }
                catch (Exception e) { Log.Write("dismount failed: " + e.Message); }
            }
        }
    }
}

static class Game
{
    public static string LatestSession()
    {
        if (!Directory.Exists(Paths.Logs)) return null;
        return Directory.GetDirectories(Paths.Logs, "session-*").OrderBy(d => d).LastOrDefault();
    }

    static void PruneSessions()
    {
        if (!Directory.Exists(Paths.Logs)) return;
        var sessions = Directory.GetDirectories(Paths.Logs, "session-*").OrderBy(d => d).ToList();
        for (int i = 0; i + Build.KeepSessions < sessions.Count; ++i)
        {
            try { Directory.Delete(sessions[i], true); }
            catch (IOException) { }
        }
    }

    // Starts the game and waits for it to end. Returns a one-line summary from its log.
    public static string Run(double seconds, bool recordAudio)
    {
        PruneSessions();
        string session = Path.Combine(Paths.Logs, "session-" + DateTime.Now.ToString("yyyyMMdd-HHmmss"));
        Directory.CreateDirectory(session);
        Directory.CreateDirectory(Paths.MemoryCard);

        var info = new ProcessStartInfo(Paths.Game,
            "\"" + Paths.Disc + "\" " + seconds.ToString(System.Globalization.CultureInfo.InvariantCulture) + " boot_log.txt");
        info.WorkingDirectory = session;
        info.UseShellExecute = false;
        info.CreateNoWindow = true;
        info.RedirectStandardOutput = true;
        info.RedirectStandardError = true;
        // The settings of the developer's play_with_audio.cmd.
        info.EnvironmentVariables["MAXIMO_WINDOW"] = "1";
        info.EnvironmentVariables["MAXIMO_GS_BACKEND"] = "gpu";
        info.EnvironmentVariables["MAXIMO_AUDIO"] = "1";
        info.EnvironmentVariables["MAXIMO_MC_ROOT"] = Paths.MemoryCard;
        // One present per 50 Hz VBlank (PRESENT_PACING.md); a value already in the environment (e.g. "off") wins.
        if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable("PS2X_PRESENT_PACING")))
            info.EnvironmentVariables["PS2X_PRESENT_PACING"] = "deadline";
        if (recordAudio)
            info.EnvironmentVariables["MAXIMO_AUDIO_WAV"] = "audio.wav";
        Picture.Apply(info);

        Log.Write("play: session " + Path.GetFileName(session) + ", picture: " + Picture.Describe() +
                  (recordAudio ? ", recording audio" : ""));
        var started = DateTime.Now;
        int exitCode;
        using (var stdout = new StreamWriter(Path.Combine(session, "stdout.txt")))
        using (var p = new Process())
        {
            p.StartInfo = info;
            DataReceivedEventHandler sink = (s, e) => { if (e.Data != null) lock (stdout) stdout.WriteLine(e.Data); };
            p.OutputDataReceived += sink;
            p.ErrorDataReceived += sink;
            p.Start();
            Running = p;
            p.BeginOutputReadLine();
            p.BeginErrorReadLine();
            // The game's last log line is ShutdownMarker. A process still alive well after it is hanging in process
            // exit: that is logged (once) for bug reports. The process is left alone; the player can end it.
            string bootLog = Path.Combine(session, "boot_log.txt");
            DateTime? shutdownSeen = null;
            bool hangLogged = false;
            while (!p.WaitForExit(2000))
            {
                if (shutdownSeen == null && LogEndsWithShutdown(bootLog))
                    shutdownSeen = DateTime.Now;
                if (shutdownSeen != null && !hangLogged && (DateTime.Now - shutdownSeen.Value).TotalSeconds >= 10)
                {
                    Log.Write("warning: the game finished shutting down but its process (" + p.Id + ") is still running after 10 s");
                    hangLogged = true;
                }
            }
            p.WaitForExit(); // the redirected output is complete
            Running = null;
            exitCode = p.ExitCode;
            if (hangLogged)
                Log.Write("the game process exited " + (DateTime.Now - shutdownSeen.Value).TotalSeconds.ToString("0") +
                          " s after it finished shutting down");
        }
        double minutes = (DateTime.Now - started).TotalMinutes;
        string summary = Summarise(Path.Combine(session, "boot_log.txt"));
        string hd = HdSummary(Path.Combine(session, "stdout.txt"));
        if (hd.Length > 0) summary += (summary.Length > 0 ? ", " : "") + hd;
        string line = "Last session: " + minutes.ToString("0.0") + " min" + (summary.Length > 0 ? ", " + summary : "") +
                      (exitCode != 0 ? " (game exit code " + exitCode + ")" : "");
        Log.Write(line);
        if (minutes < 0.25)
            line += ". The game closed almost at once: please use 'Zip logs for a bug report' and send the zip.";
        return line;
    }

    // The game process while it runs, for the launcher's own exit.
    public static volatile Process Running;
    const string ShutdownMarker = "shutdown: log complete, exiting process";

    static bool LogEndsWithShutdown(string logPath)
    {
        try
        {
            using (var stream = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                stream.Seek(Math.Max(0, stream.Length - 4096), SeekOrigin.Begin);
                return new StreamReader(stream).ReadToEnd().Contains(ShutdownMarker);
            }
        }
        catch (IOException) { return false; }
    }

    // Reads the end-of-run lines: game speed against PAL 50 Hz and audio underruns.
    static string Summarise(string logPath)
    {
        if (!File.Exists(logPath)) return "no log written";
        string tail;
        using (var stream = new FileStream(logPath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
        {
            long start = Math.Max(0, stream.Length - 512 * 1024);
            stream.Seek(start, SeekOrigin.Begin);
            tail = new StreamReader(stream).ReadToEnd();
        }
        var parts = new List<string>();
        foreach (string l in tail.Split('\n'))
        {
            int i = l.IndexOf("[perf] whole run guest vblanks");
            if (i >= 0)
            {
                int s = l.IndexOf("speed ");
                if (s >= 0) parts.Add("average speed " + l.Substring(s + 6).Trim());
            }
            i = l.IndexOf("[audio] host playback: ");
            if (i >= 0) parts.Add("audio " + l.Substring(i + 23).Trim());
        }
        return string.Join(", ", parts);
    }

    // "[hdtex] session: N of M textures looked at had a replacement; K loaded (X MB)" -> "HD textures: K loaded (X MB)".
    static string HdSummary(string path)
    {
        if (!File.Exists(path)) return "";
        try
        {
            using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite))
            {
                stream.Seek(Math.Max(0, stream.Length - 64 * 1024), SeekOrigin.Begin);
                foreach (string l in new StreamReader(stream).ReadToEnd().Split('\n'))
                {
                    int i = l.IndexOf("[hdtex] session: ");
                    int j = l.IndexOf("; ");
                    if (i >= 0 && j > i) return "HD textures: " + l.Substring(j + 2).Trim();
                }
            }
        }
        catch (IOException) { }
        return "";
    }

    public static string ZipLatest(string notes)
    {
        string session = LatestSession();
        if (session == null) throw new Exception("There is no session to zip yet: play first.");
        string zip = Path.Combine(Paths.Logs, "maximo-report-" + Path.GetFileName(session).Substring(8) + ".zip");
        if (File.Exists(zip)) File.Delete(zip);
        using (var archive = ZipFile.Open(zip, ZipArchiveMode.Create))
        {
            foreach (string f in Directory.GetFiles(session))
                archive.CreateEntryFromFile(f, Path.GetFileName(f), CompressionLevel.Optimal);
            // The problem may have been in an earlier session: add the logs (not audio recordings) of the two before.
            var earlier = Directory.GetDirectories(Paths.Logs, "session-*").OrderBy(d => d)
                                   .Where(d => string.CompareOrdinal(d, session) < 0).ToList();
            foreach (string previous in earlier.Skip(Math.Max(0, earlier.Count - 2)))
                foreach (string f in Directory.GetFiles(previous))
                    if (!f.EndsWith(".wav", StringComparison.OrdinalIgnoreCase))
                        archive.CreateEntryFromFile(f, "earlier/" + Path.GetFileName(previous) + "/" + Path.GetFileName(f),
                                                    CompressionLevel.Optimal);
            if (File.Exists(Paths.LauncherLog))
                archive.CreateEntryFromFile(Paths.LauncherLog, "launcher.log", CompressionLevel.Optimal);
            using (var w = new StreamWriter(archive.CreateEntry("notes.txt").Open(), new UTF8Encoding(false)))
            {
                w.WriteLine("Build: " + Build.Version);
                w.WriteLine("Picture: " + Picture.Describe());
                w.WriteLine("Session: " + Path.GetFileName(session) + ", zipped " + DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss"));
                w.WriteLine();
                w.WriteLine(string.IsNullOrWhiteSpace(notes) ? "(no description given)" : notes.Trim());
            }
        }
        Log.Write("zipped " + zip);
        return zip;
    }
}

static class Program
{
    [STAThread]
    static int Main(string[] args)
    {
        Picture.Load();
        try
        {
            if (args.Length == 2 && args[0] == "--setup")
            {
                Setup.Install(args[1], s => { });
                return 0;
            }
            if (args.Length >= 2 && args[0] == "--screenshot")
            {
                Application.EnableVisualStyles();
                using (var form = new LauncherForm())
                {
                    form.Show();
                    if (args.Length > 2) form.ShowPage(int.Parse(args[2]));
                    Application.DoEvents();
                    using (var bitmap = new Bitmap(form.Width, form.Height))
                    {
                        form.DrawToBitmap(bitmap, new Rectangle(0, 0, form.Width, form.Height));
                        bitmap.Save(args[1], System.Drawing.Imaging.ImageFormat.Png);
                    }
                }
                return 0;
            }
            if (args.Length == 2 && args[0] == "--hdtex-install")
            {
                Log.Write("hdtex-install: " + HdTextures.InstallFrom(args[1], s => Console.WriteLine(s)));
                return 0;
            }
            if (args.Length == 1 && args[0] == "--hdtex-marcus")
            {
                Log.Write("hdtex-marcus: " + HdTextures.InstallMarcus(s => Console.WriteLine(s)));
                return 0;
            }
            if (args.Length == 2 && args[0] == "--hdtex-screenshot")
            {
                Application.EnableVisualStyles();
                using (var form = new HdTexturesForm())
                {
                    form.Show();
                    Application.DoEvents();
                    using (var bitmap = new Bitmap(form.Width, form.Height))
                    {
                        form.DrawToBitmap(bitmap, new Rectangle(0, 0, form.Width, form.Height));
                        bitmap.Save(args[1], System.Drawing.Imaging.ImageFormat.Png);
                    }
                }
                return 0;
            }
            if (args.Length == 2 && args[0] == "--zip")
            {
                Game.ZipLatest(args[1]);
                return 0;
            }
            if (args.Length == 2 && args[0] == "--play")
            {
                Game.Run(double.Parse(args[1], System.Globalization.CultureInfo.InvariantCulture), false);
                return 0;
            }
        }
        catch (Exception e)
        {
            Log.Write("error: " + e);
            return 1;
        }
        Log.Write("launcher started, build " + Build.Version);
        Application.EnableVisualStyles();
        Application.Run(new LauncherForm());
        return 0;
    }
}
