// V3-07 switch-speed harness primitives (compiled by Add-Type -Path).
// ASCII only: PowerShell/cmake toolchain here is not reliable with BOM-less CJK.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.Drawing.Imaging;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using System.Windows.Forms;

public static class NvSwitch
{
    // ---------- native ----------
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int Left, Top, Right, Bottom; }

    [StructLayout(LayoutKind.Sequential)]
    public struct KEYBDINPUT { public ushort wVk, wScan; public uint dwFlags, time; public IntPtr dwExtraInfo; }

    [StructLayout(LayoutKind.Sequential)]
    public struct MOUSEINPUT { public int dx, dy; public uint mouseData, dwFlags, time; public IntPtr dwExtraInfo; }

    [StructLayout(LayoutKind.Sequential)]
    public struct HARDWAREINPUT { public uint uMsg; public ushort wParamL, wParamH; }

    [StructLayout(LayoutKind.Explicit)]
    public struct INPUTUNION
    {
        [FieldOffset(0)] public MOUSEINPUT mi;
        [FieldOffset(0)] public KEYBDINPUT ki;
        [FieldOffset(0)] public HARDWAREINPUT hi;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct INPUT { public uint type; public INPUTUNION u; }

    [DllImport("user32.dll", SetLastError = true)]
    static extern uint SendInput(uint n, INPUT[] p, int cb);

    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h, int x, int y, int w, int ht, bool repaint);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr param);
    [DllImport("user32.dll")] static extern bool PostMessageW(IntPtr h, uint msg, IntPtr w, IntPtr l);

    public static void PostMessageClose(IntPtr h) { PostMessageW(h, 0x0010, IntPtr.Zero, IntPtr.Zero); }
    [DllImport("kernel32.dll")] static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("user32.dll")] static extern IntPtr SetThreadDpiAwarenessContext(IntPtr ctx);
    [DllImport("user32.dll")] static extern bool SetProcessDpiAwarenessContext(IntPtr ctx);

    delegate bool EnumProc(IntPtr h, IntPtr p);

    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }

    public const int SW_RESTORE = 9;
    const uint INPUT_KEYBOARD = 1;
    const uint KEYEVENTF_KEYUP = 0x0002;
    const uint KEYEVENTF_EXTENDEDKEY = 0x0001;
    const int VK_RIGHT = 0x27;
    const int VK_LEFT = 0x25;

    // ---------- basic ----------
    public static void MakeDpiAware()
    {
        try { SetProcessDpiAwarenessContext(new IntPtr(-4)); } catch { }   // per-monitor v2
    }

    public static string Title(IntPtr h)
    {
        var sb = new StringBuilder(512);
        GetWindowTextW(h, sb, sb.Capacity);
        return sb.ToString();
    }

    public static string ClassOf(IntPtr h)
    {
        var sb = new StringBuilder(256);
        GetClassNameW(h, sb, sb.Capacity);
        return sb.ToString();
    }

    public static int PidOf(IntPtr h)
    {
        uint pid;
        GetWindowThreadProcessId(h, out pid);
        return (int)pid;
    }

    public static int[] RectOf(IntPtr h)
    {
        RECT r;
        if (!GetWindowRect(h, out r)) return new int[] { 0, 0, 0, 0 };
        return new int[] { r.Left, r.Top, r.Right - r.Left, r.Bottom - r.Top };
    }

    public static int[] ClientRectOf(IntPtr h)
    {
        RECT r;
        if (!GetClientRect(h, out r)) return new int[] { 0, 0, 0, 0 };
        var p = new POINT { X = 0, Y = 0 };
        ClientToScreen(h, ref p);
        return new int[] { p.X, p.Y, r.Right - r.Left, r.Bottom - r.Top };
    }

    public static bool ForceForeground(IntPtr h)
    {
        if (GetForegroundWindow() == h) return true;
        // Only un-minimize. SW_RESTORE on a normal window would reset the
        // restored rect and silently undo our MoveWindow (V3-07 pitfall).
        if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
        uint targetPid;
        uint targetThread = GetWindowThreadProcessId(h, out targetPid);
        uint self = GetCurrentThreadId();
        bool attached = AttachThreadInput(self, targetThread, true);
        bool ok = SetForegroundWindow(h);
        if (attached) AttachThreadInput(self, targetThread, false);
        Thread.Sleep(60);
        return GetForegroundWindow() == h || ok;
    }

    [DllImport("user32.dll")] static extern bool IsZoomed(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsIconic(IntPtr h);

    public static bool RestoreIfMaximized(IntPtr h)
    {
        if (!IsZoomed(h)) return false;
        ShowWindow(h, SW_RESTORE);
        Thread.Sleep(150);
        return true;
    }

    // MoveWindow + verify; some apps clamp or ignore the first request.
    public static bool PlaceWindow(IntPtr h, int x, int y, int w, int ht, int attempts)
    {
        for (int i = 0; i < attempts; i++)
        {
            MoveWindow(h, x, y, w, ht, true);
            Thread.Sleep(250);
            int[] r = RectOf(h);
            if (r[0] == x && r[1] == y && r[2] == w && r[3] == ht) return true;
        }
        return false;
    }


    public static IntPtr FindLargestWindowOfPid(int pid, int minArea)
    {
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows(delegate (IntPtr h, IntPtr p)
        {
            uint wpid;
            GetWindowThreadProcessId(h, out wpid);
            if ((int)wpid != pid) return true;
            if (!IsWindowVisible(h)) return true;
            int[] r = RectOf(h);
            long area = (long)r[2] * r[3];
            if (area >= minArea && area > bestArea) { bestArea = area; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    public static IntPtr FindWindowByTitlePart(string part, int minArea)
    {
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows(delegate (IntPtr h, IntPtr p)
        {
            if (!IsWindowVisible(h)) return true;
            string title = Title(h);
            if (title.IndexOf(part, StringComparison.OrdinalIgnoreCase) < 0) return true;
            int[] r = RectOf(h);
            long area = (long)r[2] * r[3];
            if (area >= minArea && area > bestArea) { bestArea = area; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    // 2345 draws a separate RCShadowWnd that also carries the "2345" title, so
    // match the frame class explicitly.
    public static IntPtr FindWindowByClassPrefix(string prefix, int minArea)
    {
        IntPtr best = IntPtr.Zero;
        long bestArea = 0;
        EnumWindows(delegate (IntPtr h, IntPtr p)
        {
            if (!IsWindowVisible(h)) return true;
            if (!ClassOf(h).StartsWith(prefix, StringComparison.Ordinal)) return true;
            int[] r = RectOf(h);
            long area = (long)r[2] * r[3];
            if (area >= minArea && area > bestArea) { bestArea = area; best = h; }
            return true;
        }, IntPtr.Zero);
        return best;
    }

    // Debug: every visible top-level window as "pid|hwnd|x,y,w,h|class|title".
    public static string[] TopWindows()
    {
        var list = new List<string>();
        EnumWindows(delegate (IntPtr h, IntPtr p)
        {
            if (!IsWindowVisible(h)) return true;
            int[] r = RectOf(h);
            if (r[2] < 200 || r[3] < 120) return true;
            list.Add(string.Format("{0}|0x{1:X}|{2},{3},{4},{5}|{6}|{7}",
                PidOf(h), h.ToInt64(), r[0], r[1], r[2], r[3], ClassOf(h), Title(h)));
            return true;
        }, IntPtr.Zero);
        return list.ToArray();
    }

    // ---------- keys ----------
    public class KeyHit
    {
        public int Index;
        public double T0Ms;
        public bool ForegroundOk;
    }

    static void SendArrow(int vk)
    {
        var inputs = new INPUT[2];
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].u.ki.wVk = (ushort)vk;
        inputs[0].u.ki.wScan = 0;
        inputs[0].u.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
        inputs[1] = inputs[0];
        inputs[1].u.ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
        SendInput(2, inputs, Marshal.SizeOf(typeof(INPUT)));
    }

    public static KeyHit[] RunKeys(IntPtr hwnd, int direction, int count, int intervalMs, string eventName)
    {
        int vk = direction < 0 ? VK_LEFT : VK_RIGHT;
        EventWaitHandle ev = null;
        try { ev = EventWaitHandle.OpenExisting(eventName); } catch { }
        var hits = new KeyHit[count];
        var sw = Stopwatch.StartNew();
        long freq = Stopwatch.Frequency;
        double startMs = 400.0;                      // let the first frame settle
        for (int i = 0; i < count; i++)
        {
            double target = startMs + i * (double)intervalMs;
            double now = sw.Elapsed.TotalMilliseconds;
            if (target - now > 1.5) Thread.Sleep((int)(target - now - 1.0));
            while (sw.Elapsed.TotalMilliseconds < target) Thread.SpinWait(40);

            bool fg = GetForegroundWindow() == hwnd;
            if (!fg) ForceForeground(hwnd);

            var hit = new KeyHit();
            hit.Index = i;
            hit.T0Ms = sw.Elapsed.TotalMilliseconds;
            hit.ForegroundOk = fg || GetForegroundWindow() == hwnd;
            hits[i] = hit;

            SendArrow(vk);
            if (ev != null) ev.Set();                 // sync marker flash
        }
        return hits;
    }

    // ---------- memory ----------
    public class MemoryWatch
    {
        Thread thread;
        volatile bool stop;
        public long PeakPrivate;
        public long PeakWs;
        public int Samples;

        public void Start(int pid, int intervalMs)
        {
            stop = false;
            thread = new Thread(delegate ()
            {
                try
                {
                    using (var p = Process.GetProcessById(pid))
                    {
                        while (!stop)
                        {
                            try
                            {
                                p.Refresh();
                                if (p.HasExited) break;
                                long priv = p.PrivateMemorySize64;
                                long ws = p.WorkingSet64;
                                if (priv > PeakPrivate) PeakPrivate = priv;
                                if (ws > PeakWs) PeakWs = ws;
                                Samples++;
                            }
                            catch { break; }
                            Thread.Sleep(intervalMs);
                        }
                    }
                }
                catch { }
            });
            thread.IsBackground = true;
            thread.Start();
        }

        public void Stop()
        {
            stop = true;
            if (thread != null) thread.Join(1000);
        }
    }

    // ---------- screen stability ----------
    static long HashBitmap(Bitmap bmp)
    {
        BitmapData data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height),
                                       ImageLockMode.ReadOnly, PixelFormat.Format32bppArgb);
        try
        {
            long hash = 1469598103934665603L;
            int bytes = Math.Abs(data.Stride) * bmp.Height;
            byte[] buffer = new byte[bytes];
            Marshal.Copy(data.Scan0, buffer, 0, bytes);
            for (int i = 0; i < bytes; i += 4)          // sample every 4th byte pair-wise
            {
                hash ^= buffer[i];
                hash *= 1099511628211L;
            }
            return hash;
        }
        finally { bmp.UnlockBits(data); }
    }

    public static long HashRect(int x, int y, int w, int h)
    {
        using (var bmp = new Bitmap(w, h, PixelFormat.Format32bppArgb))
        {
            using (var g = Graphics.FromImage(bmp))
            {
                g.CopyFromScreen(x, y, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
            }
            return HashBitmap(bmp);
        }
    }

    // Returns ms until the rect stayed identical for stableMs, or -1 on timeout.
    public static int WaitStable(int x, int y, int w, int h, int stableMs, int timeoutMs, int pollMs)
    {
        var sw = Stopwatch.StartNew();
        long previous = HashRect(x, y, w, h);
        double lastChange = 0;
        while (sw.ElapsedMilliseconds < timeoutMs)
        {
            Thread.Sleep(pollMs);
            long now = HashRect(x, y, w, h);
            if (now != previous) { previous = now; lastChange = sw.Elapsed.TotalMilliseconds; }
            if (sw.Elapsed.TotalMilliseconds - lastChange >= stableMs)
                return (int)sw.ElapsedMilliseconds;
        }
        return -1;
    }

    // ---------- log tail ----------
    // Records "qpc_ms<TAB>line" for every line appended after Start(), so the
    // caller can measure key -> log-line arrival at millisecond precision
    // (ZoreView's own log timestamps only carry whole seconds).
    public class LogTail
    {
        Thread thread;
        volatile bool stop;
        public readonly List<string> Lines = new List<string>();
        long offset;
        readonly object gate = new object();

        public void Start(string path)
        {
            stop = false;
            var sw = Stopwatch.StartNew();
            thread = new Thread(delegate ()
            {
                FileStream fs = null;
                try
                {
                    while (!stop)
                    {
                        try
                        {
                            if (fs == null)
                            {
                                if (!File.Exists(path)) { Thread.Sleep(5); continue; }
                                fs = new FileStream(path, FileMode.Open, FileAccess.Read,
                                                    FileShare.ReadWrite | FileShare.Delete);
                                fs.Seek(0, SeekOrigin.End);
                                offset = fs.Length;
                            }
                            if (fs.Length > offset)
                            {
                                fs.Seek(offset, SeekOrigin.Begin);
                                var buffer = new byte[fs.Length - offset];
                                int read = fs.Read(buffer, 0, buffer.Length);
                                offset += read;
                                string text = Encoding.UTF8.GetString(buffer, 0, read);
                                double ms = sw.Elapsed.TotalMilliseconds;
                                lock (gate)
                                {
                                    foreach (var line in text.Split('\n'))
                                    {
                                        if (line.Length == 0) continue;
                                        Lines.Add(ms.ToString("F1") + "\t" + line.TrimEnd('\r'));
                                    }
                                }
                            }
                            else if (fs.Length < offset)
                            {
                                offset = fs.Length;       // truncated by the app
                            }
                        }
                        catch { }
                        Thread.Sleep(2);
                    }
                }
                finally { if (fs != null) fs.Dispose(); }
            });
            thread.IsBackground = true;
            thread.Start();
        }

        public void Stop() { stop = true; if (thread != null) thread.Join(500); }

        public string[] Snapshot()
        {
            lock (gate) { return Lines.ToArray(); }
        }
    }

    // ---------- screenshot ----------
    public static void SaveScreenshot(int x, int y, int w, int h, string path)
    {
        using (var bmp = new Bitmap(w, h, PixelFormat.Format24bppRgb))
        {
            using (var g = Graphics.FromImage(bmp))
            {
                g.CopyFromScreen(x, y, 0, 0, new Size(w, h), CopyPixelOperation.SourceCopy);
            }
            bmp.Save(path, ImageFormat.Png);
        }
    }

    // ---------- sync marker ----------
    public class MarkerForm : Form
    {
        bool white;
        public MarkerForm()
        {
            FormBorderStyle = FormBorderStyle.None;
            StartPosition = FormStartPosition.Manual;
            Bounds = new Rectangle(0, 0, 24, 24);
            TopMost = true;
            ShowInTaskbar = false;
            BackColor = Color.Black;
            DoubleBuffered = true;
        }
        protected override bool ShowWithoutActivation { get { return true; } }
        protected override CreateParams CreateParams
        {
            get
            {
                var cp = base.CreateParams;
                cp.ExStyle |= 0x08000000;   // WS_EX_NOACTIVATE
                cp.ExStyle |= 0x00000080;   // WS_EX_TOOLWINDOW
                return cp;
            }
        }
        public void Flash()
        {
            white = true;
            Invalidate();
            Update();
            var t = new Thread(delegate ()
            {
                Thread.Sleep(40);
                try { BeginInvoke((Action)delegate { white = false; Invalidate(); Update(); }); }
                catch { }
            });
            t.IsBackground = true;
            t.Start();
        }
        protected override void OnPaint(PaintEventArgs e)
        {
            e.Graphics.Clear(white ? Color.White : Color.Black);
        }
    }

    public static void RunMarker(string eventName)
    {
        MakeDpiAware();
        var form = new MarkerForm();
        var thread = new Thread(delegate ()
        {
            EventWaitHandle ev = null;
            while (ev == null)
            {
                try { ev = EventWaitHandle.OpenExisting(eventName); }
                catch { Thread.Sleep(20); }
            }
            while (true)
            {
                ev.WaitOne();
                try { form.BeginInvoke((Action)form.Flash); }
                catch { }
            }
        });
        thread.IsBackground = true;
        thread.Start();
        Application.Run(form);
    }
}
