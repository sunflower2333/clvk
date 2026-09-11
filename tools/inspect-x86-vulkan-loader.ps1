param(
    [Parameter(Mandatory=$true)][string]$RuntimeDir,
    [Parameter(Mandatory=$true)][string]$DriverManifest
)
$ErrorActionPreference = 'Stop'
if ([IntPtr]::Size -ne 4) { throw 'Run this diagnostic in x86 PowerShell' }
$directory = Join-Path $RuntimeDir ('loader-only-' + [Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $directory | Out-Null
Start-Transcript (Join-Path $directory 'transcript.txt') | Out-Null
try {
    Write-Output "DiagnosticDirectory=$directory UTC=$([DateTime]::UtcNow.ToString('o')) pointer_bits=32"
    $env:VK_DRIVER_FILES = $DriverManifest
    $env:VK_ICD_FILENAMES = $DriverManifest
    $env:VK_LOADER_DEBUG = 'all'
    $icd = (Get-Content $DriverManifest -Raw | ConvertFrom-Json).ICD.library_path
    $icd = [IO.Path]::GetFullPath((Join-Path (Split-Path $DriverManifest) $icd))
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LoaderOnly {
 [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)] static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
 [DllImport("kernel32", SetLastError=true)] static extern IntPtr GetProcAddress(IntPtr module, string name);
 [DllImport("kernel32")] static extern bool FreeLibrary(IntPtr module);
 [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int Negotiate(ref uint version);
 [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate IntPtr GetProc(IntPtr instance, [MarshalAs(UnmanagedType.LPStr)] string name);
 [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int Version(ref uint version);
 [StructLayout(LayoutKind.Sequential)] struct AppInfo { public uint sType; public IntPtr pNext; public IntPtr name; public uint version; public IntPtr engine; public uint engineVersion; public uint apiVersion; }
 [StructLayout(LayoutKind.Sequential)] struct CreateInfo { public uint sType; public IntPtr pNext; public uint flags; public IntPtr app; public uint layers; public IntPtr layerNames; public uint extensions; public IntPtr extensionNames; }
 [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate int Create(ref CreateInfo info, IntPtr allocator, out IntPtr instance);
 [UnmanagedFunctionPointer(CallingConvention.StdCall)] delegate void Destroy(IntPtr instance, IntPtr allocator);
 static T Proc<T>(IntPtr module, string name) where T:class { IntPtr p=GetProcAddress(module,name); if(p==IntPtr.Zero) throw new Exception("Missing "+name); return Marshal.GetDelegateForFunctionPointer(p,typeof(T)) as T; }
 public static void Run(string icd, string loader) {
   IntPtr lib=LoadLibraryEx(icd,IntPtr.Zero,0);
   Console.WriteLine("ICD LoadLibrary normal="+lib+" error="+Marshal.GetLastWin32Error());
   if(lib!=IntPtr.Zero) FreeLibrary(lib);
   lib=LoadLibraryEx(icd,IntPtr.Zero,0x1100);
   Console.WriteLine("ICD LoadLibrary DLL_LOAD_DIR="+lib+" error="+Marshal.GetLastWin32Error());
   if(lib!=IntPtr.Zero) {
     uint n=7; int r=Proc<Negotiate>(lib,"vk_icdNegotiateLoaderICDInterfaceVersion")(ref n);
     Console.WriteLine("ICD negotiate="+r+" interface="+n);
     IntPtr p=Proc<GetProc>(lib,"vk_icdGetInstanceProcAddr")(IntPtr.Zero,"vkEnumerateInstanceVersion");
     uint v=0; r=((Version)Marshal.GetDelegateForFunctionPointer(p,typeof(Version)))(ref v);
     Console.WriteLine("ICD version result="+r+" api="+v);
     FreeLibrary(lib);
   }
   lib=LoadLibraryEx(loader,IntPtr.Zero,0x1100);
   if(lib==IntPtr.Zero) throw new Exception("Vulkan loader error="+Marshal.GetLastWin32Error());
   AppInfo app=new AppInfo(); app.apiVersion=(1u<<22)|(1u<<12);
   IntPtr memory=Marshal.AllocHGlobal(Marshal.SizeOf(app)); Marshal.StructureToPtr(app,memory,false);
   CreateInfo info=new CreateInfo(); info.sType=1; info.app=memory;
   IntPtr instance; int result=Proc<Create>(lib,"vkCreateInstance")(ref info,IntPtr.Zero,out instance);
   Console.WriteLine("INSTANCE_ONLY result="+result+" instance="+instance+"; no physical devices, device creation or submissions");
   if(result==0) Proc<Destroy>(lib,"vkDestroyInstance")(instance,IntPtr.Zero);
   Marshal.FreeHGlobal(memory); FreeLibrary(lib);
 }
}
'@
    [LoaderOnly]::Run($icd, (Join-Path $RuntimeDir 'vulkan-1.dll'))
} finally { Stop-Transcript | Out-Null }
