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
// Development builds only (build.cmd with MAXIMO_LAUNCHER_DEV=1): --setup <iso or folder>, --play <seconds>, --zip <notes>, --screenshot <png> [page],
// --hdtex-install <zip or folder>, --hdtex-marcus, --hdtex-screenshot <png>. Results go to launcher.log.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Threading;
using System.Windows.Forms;

[assembly: AssemblyTitle("Maximo Launcher")]
[assembly: AssemblyDescription("Launcher and builder for the unofficial Maximo PC port")]
[assembly: AssemblyCompany("Biorhythmic Studios")]
[assembly: AssemblyProduct("Maximo Launcher")]
[assembly: AssemblyCopyright("Copyright (C) 2026 Biorhythmic Studios. GPL-3.0.")]
[assembly: AssemblyVersion("1.1.0.0")]
[assembly: AssemblyFileVersion("1.1.0.0")]

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
    public static readonly string Art = Path.Combine(Root, "art");
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

    // Copies a verified disc (an ISO, which is only read, or an extracted folder) into .\disc.
    public static void Install(string source, Action<string> progress)
    {
        bool isIso = File.Exists(source) && source.EndsWith(".iso", StringComparison.OrdinalIgnoreCase);
        if (!isIso && !Directory.Exists(source))
            throw new Exception("Choose a .iso file or a folder with the extracted disc.");
        progress("Opening the disc image...");
        using (FileStream image = isIso ? new FileStream(source, FileMode.Open, FileAccess.Read, FileShare.Read) : null)
        {
            List<DiscFile> files = isIso ? Iso.Files(image) : DiscFile.InFolder(source);
            DiscFile elf = files.FirstOrDefault(f => string.Equals(f.Relative, Build.ElfName, StringComparison.OrdinalIgnoreCase));
            if (elf == null)
                throw new Exception("This is not the PAL Maximo disc: " + Build.ElfName + " is missing. " +
                                    "This build needs Maximo: Ghosts to Glory, Europe (SLES-50703).");
            progress("Checking the disc...");
            string hash;
            using (var sha = SHA256.Create())
            {
                using (var hashing = new CryptoStream(Stream.Null, sha, CryptoStreamMode.Write))
                    elf.CopyTo(hashing);
                hash = BitConverter.ToString(sha.Hash).Replace("-", "");
            }
            Log.Write("disc source " + source + ", " + Build.ElfName + " sha256 " + hash);
            if (!string.Equals(hash, Build.ElfSha256, StringComparison.OrdinalIgnoreCase))
                throw new Exception("This disc's game executable does not match the PAL release (SLES-50703, v1.00). " +
                                    "Other versions are not supported by this build.");

            string temp = Paths.Disc + ".partial";
            if (Directory.Exists(temp)) Directory.Delete(temp, true);
            long total = files.Sum(f => f.Size), done = 0;
            for (int i = 0; i < files.Count; ++i)
            {
                string target = Path.Combine(temp, files[i].Relative);
                Directory.CreateDirectory(Path.GetDirectoryName(target));
                using (var output = new FileStream(target, FileMode.Create, FileAccess.Write))
                    files[i].CopyTo(output);
                done += files[i].Size;
                progress("Copying game files: " + (100 * done / Math.Max(1, total)) + "% (" + (i + 1) + " of " + files.Count + ")");
            }
            if (Directory.Exists(Paths.Disc)) Directory.Delete(Paths.Disc, true);
            Directory.Move(temp, Paths.Disc);
            string why;
            if (!DiscReady(out why))
                throw new Exception(why);
            Log.Write("disc installed: " + files.Count + " files, " + total + " bytes");
        }
    }
}

// One file of the disc, from an extracted folder or from inside an ISO.
class DiscFile
{
    public string Relative;
    public long Size;
    public Stream Image;   // the ISO, or null for a file on disk
    public long Offset;    // where the file starts in the ISO
    public string Path2;   // the file on disk

    public static List<DiscFile> InFolder(string folder)
    {
        string root = Path.GetFullPath(folder);
        return Directory.GetFiles(root, "*", SearchOption.AllDirectories)
            .Select(f => new DiscFile { Relative = f.Substring(root.TrimEnd('\\').Length + 1), Size = new FileInfo(f).Length, Path2 = f })
            .ToList();
    }

    public void CopyTo(Stream output)
    {
        if (Image == null)
        {
            using (var input = File.OpenRead(Path2))
                input.CopyTo(output);
            return;
        }
        Image.Position = Offset;
        var buffer = new byte[1 << 20];
        for (long left = Size; left > 0; )
        {
            int got = Image.Read(buffer, 0, (int)Math.Min(left, buffer.Length));
            if (got <= 0)
                throw new Exception("The disc image is shorter than it should be: " + Relative);
            output.Write(buffer, 0, got);
            left -= got;
        }
    }
}

// Lists the files of an ISO 9660 disc image. It only reads the image.
static class Iso
{
    const int Sector = 2048;

    public static List<DiscFile> Files(Stream image)
    {
        var header = new byte[Sector];
        for (long sector = 16; ; ++sector)
        {
            Read(image, sector * Sector, header);
            if (Encoding.ASCII.GetString(header, 1, 5) != "CD001" || header[0] == 255)
                throw new Exception("This file is not a plain disc image (.iso).");
            if (header[0] == 1)
                break;
        }
        var files = new List<DiscFile>();
        const int root = 156; // the root folder's record, inside the header
        Folder(image, BitConverter.ToUInt32(header, root + 2), BitConverter.ToUInt32(header, root + 10), "", files, 0);
        return files;
    }

    static void Folder(Stream image, long sector, long size, string prefix, List<DiscFile> files, int depth)
    {
        if (depth > 16 || size > 1 << 24)
            throw new Exception("The disc image's folders are damaged.");
        var data = new byte[size];
        Read(image, sector * Sector, data);
        for (int at = 0; at + 33 < data.Length; )
        {
            int length = data[at];
            if (length == 0) // records do not cross sectors: the rest of this one is empty
            {
                at = (at / Sector + 1) * Sector;
                continue;
            }
            int flags = data[at + 25], nameLength = data[at + 32];
            long start = BitConverter.ToUInt32(data, at + 2), bytes = BitConverter.ToUInt32(data, at + 10);
            string name = Encoding.ASCII.GetString(data, at + 33, nameLength);
            at += length;
            if (nameLength == 1 && name[0] < 2)
                continue; // the "." and ".." records
            if ((flags & 0x80) != 0)
                throw new Exception("This disc image has a file in several parts, which is not supported.");
            int version = name.IndexOf(';');
            if (version >= 0) name = name.Substring(0, version);
            name = name.TrimEnd('.');
            if ((flags & 2) != 0)
                Folder(image, start, bytes, prefix + name + "\\", files, depth + 1);
            else
                files.Add(new DiscFile { Relative = prefix + name, Size = bytes, Image = image, Offset = start * Sector });
        }
    }

    static void Read(Stream image, long offset, byte[] data)
    {
        image.Position = offset;
        int done = 0;
        while (done < data.Length)
        {
            int got = image.Read(data, done, data.Length - done);
            if (got <= 0)
                throw new Exception("This file is not a plain disc image (.iso).");
            done += got;
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
        Bindings.Apply(info);

        Log.Write("play: session " + Path.GetFileName(session) + ", picture: " + Picture.Describe() + Bindings.Describe() +
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
                w.WriteLine("Picture: " + Picture.Describe() + Bindings.Describe());
                if (File.Exists(Bindings.File_))
                    w.WriteLine("Controls file:\r\n" + File.ReadAllText(Bindings.File_));
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
        Bindings.Load();
#if DEV_SWITCHES
        try
        {
            if (args.Length == 2 && args[0] == "--setup")
            {
                Setup.Install(args[1], s => { });
                return 0;
            }
            if (args.Length == 2 && args[0] == "--screenshot-controls")
            {
                Application.EnableVisualStyles();
                using (var form = new ControlsForm())
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
            if (args.Length >= 1 && args[0] == "--build")
            {
                Builder.ForceOwnGit = args.Length > 1 && args[1] == "--own-git";
                Builder.Run((text, percent) => { if (percent <= 0) Console.WriteLine("== " + text); }, Console.WriteLine);
                return 0;
            }
            if (args.Length == 2 && args[0] == "--signed")
            {
                Console.WriteLine(Builder.SignedByMicrosoft(args[1]) ? "signed" : "not signed");
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
#endif
        Log.Write("launcher started, build " + Build.Version);
        Application.EnableVisualStyles();
        Application.Run(new LauncherForm());
        return 0;
    }
}
