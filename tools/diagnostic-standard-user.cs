// SPDX-License-Identifier: Apache-2.0
// CI only: validate the actual resource and original EXE under a standard user.
using System;
using System.ComponentModel;
using System.IO;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;

public static class DiagnosticStandardUser {
    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct StartupInfo {
        public int cb;
        public string reserved, desktop, title;
        public int x, y, xSize, ySize, xChars, yChars, fill, flags;
        public short show, reservedSize;
        public IntPtr reservedBytes, input, output, error;
    }
    [StructLayout(LayoutKind.Sequential)]
    struct ProcessInfo {
        public IntPtr process, thread;
        public uint processId, threadId;
    }
    [DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool CreateProcessWithLogonW(string user, string domain,
        string password, uint logonFlags, string application, StringBuilder command,
        uint flags, IntPtr environment, string directory, ref StartupInfo startup,
        out ProcessInfo process);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool OpenProcessToken(IntPtr process, uint access, out IntPtr token);
    [DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(IntPtr token, int kind, IntPtr data,
        int size, out int needed);
    [DllImport("advapi32.dll")]
    static extern IntPtr GetSidSubAuthorityCount(IntPtr sid);
    [DllImport("advapi32.dll")]
    static extern IntPtr GetSidSubAuthority(IntPtr sid, uint index);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint ResumeThread(IntPtr thread);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetExitCodeProcess(IntPtr process, out uint code);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool TerminateProcess(IntPtr process, uint code);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern IntPtr LoadLibraryExW(string path, IntPtr file, uint flags);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr FindResourceW(IntPtr module, IntPtr name, IntPtr type);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern uint SizeofResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr LoadResource(IntPtr module, IntPtr resource);
    [DllImport("kernel32.dll")]
    static extern IntPtr LockResource(IntPtr resource);
    [DllImport("kernel32.dll")]
    static extern bool FreeLibrary(IntPtr module);

    static void Check(bool success, string operation) {
        if (!success) throw new Win32Exception(Marshal.GetLastWin32Error(), operation);
    }
    public static string Manifest(string path) {
        // Resource-only mapping works for native and emulated PE architectures.
        IntPtr module = LoadLibraryExW(path, IntPtr.Zero, 0x22);
        Check(module != IntPtr.Zero, "resource-only load");
        try {
            IntPtr resource = FindResourceW(module, new IntPtr(1), new IntPtr(24));
            Check(resource != IntPtr.Zero, "RT_MANIFEST resource #1");
            uint size = SizeofResource(module, resource);
            if (size == 0 || size > 65536) throw new Exception("Invalid manifest size");
            IntPtr loaded = LoadResource(module, resource);
            Check(loaded != IntPtr.Zero, "LoadResource");
            IntPtr data = LockResource(loaded);
            Check(data != IntPtr.Zero, "LockResource");
            byte[] bytes = new byte[size];
            Marshal.Copy(data, bytes, 0, bytes.Length);
            using (var reader = new StreamReader(new MemoryStream(bytes), Encoding.UTF8, true))
                return reader.ReadToEnd();
        } finally { Check(FreeLibrary(module), "FreeLibrary resource mapping"); }
    }
    static IntPtr TokenInfo(IntPtr token, int kind) {
        int needed;
        GetTokenInformation(token, kind, IntPtr.Zero, 0, out needed);
        if (needed <= 0) throw new Exception("No token information size");
        IntPtr data = Marshal.AllocHGlobal(needed);
        try {
            Check(GetTokenInformation(token, kind, data, needed, out needed), "token information");
            return data;
        } catch { Marshal.FreeHGlobal(data); throw; }
    }
    public static void Launch(string user, string password, string expectedSid,
        string executable, string arguments) {
        var startup = new StartupInfo();
        startup.cb = Marshal.SizeOf(typeof(StartupInfo));
        ProcessInfo process;
        // Inspect this exact executable's primary token before its first thread
        // runs. No shell, runas verb, elevation, compatibility flag or UAC bypass.
        Check(CreateProcessWithLogonW(user, Environment.MachineName, password, 0,
            executable, new StringBuilder("\"" + executable + "\" " + arguments),
            0x08000004, IntPtr.Zero, Path.GetDirectoryName(executable),
            ref startup, out process), "standard-user CreateProcessWithLogonW");
        IntPtr token = IntPtr.Zero, elevation = IntPtr.Zero, integrity = IntPtr.Zero;
        bool exited = false;
        try {
            Check(OpenProcessToken(process.process, 8, out token), "OpenProcessToken");
            string actualSid;
            using (var identity = new WindowsIdentity(token)) actualSid = identity.User.Value;
            if (actualSid != expectedSid) throw new Exception("Child is not the standard test user");
            elevation = TokenInfo(token, 20); // TokenElevation
            if (Marshal.ReadInt32(elevation) != 0) throw new Exception("Child is elevated");
            integrity = TokenInfo(token, 25); // TokenIntegrityLevel
            IntPtr sid = Marshal.ReadIntPtr(integrity);
            byte count = Marshal.ReadByte(GetSidSubAuthorityCount(sid));
            if (count == 0) throw new Exception("Invalid integrity SID");
            int rid = Marshal.ReadInt32(GetSidSubAuthority(sid, (uint)(count - 1)));
            if (rid > 0x2000) throw new Exception("Child integrity exceeds medium");
            Check(ResumeThread(process.thread) != UInt32.MaxValue, "ResumeThread");
            uint wait = WaitForSingleObject(process.process, 15000);
            if (wait != 0) throw new Exception("Diagnostic did not exit within 15 seconds; wait=" + wait);
            exited = true;
            uint code;
            Check(GetExitCodeProcess(process.process, out code), "GetExitCodeProcess");
            if (code != 0) throw new Exception("Diagnostic exit=" + code);
            Console.WriteLine("PASS ordinary-user exe={0} pid={1} sid={2} elevated=0 integrity={3} exit=0",
                executable, process.processId, actualSid, rid);
        } finally {
            if (!exited) TerminateProcess(process.process, 1);
            if (integrity != IntPtr.Zero) Marshal.FreeHGlobal(integrity);
            if (elevation != IntPtr.Zero) Marshal.FreeHGlobal(elevation);
            if (token != IntPtr.Zero) CloseHandle(token);
            CloseHandle(process.thread); CloseHandle(process.process);
        }
    }
}
