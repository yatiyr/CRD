// Out-of-process IDE coordination only. No VS extension install or engine dependency.
using System;
using System.Collections.Generic;
using System.IO;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.ComTypes;
using System.Web.Script.Serialization;

[ComImport, Guid("00000016-0000-0000-C000-000000000046"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
internal interface IOleMessageFilter
{
    [PreserveSig] int HandleInComingCall(int callType, IntPtr taskCaller, int tickCount, IntPtr interfaceInfo);
    [PreserveSig] int RetryRejectedCall(IntPtr taskCallee, int tickCount, int rejectType);
    [PreserveSig] int MessagePending(IntPtr taskCallee, int tickCount, int pendingType);
}

internal sealed class RetryFilter : IOleMessageFilter
{
    public int HandleInComingCall(int callType, IntPtr taskCaller, int tickCount, IntPtr interfaceInfo) { return 0; }
    public int RetryRejectedCall(IntPtr taskCallee, int tickCount, int rejectType)
    {
        return rejectType == 2 && tickCount < 5000 ? 100 : -1;
    }
    public int MessagePending(IntPtr taskCallee, int tickCount, int pendingType) { return 2; }
}

internal static class CeridVsBridge
{
    private static int processId;
    [DllImport("ole32.dll")]
    private static extern int GetRunningObjectTable(int reserved, out IRunningObjectTable table);
    [DllImport("ole32.dll")]
    private static extern int CreateBindCtx(int reserved, out IBindCtx context);
    [DllImport("ole32.dll")]
    private static extern int CoRegisterMessageFilter(IOleMessageFilter filter, out IOleMessageFilter previous);

    private static object FindIde(string solution)
    {
        IRunningObjectTable table;
        Marshal.ThrowExceptionForHR(GetRunningObjectTable(0, out table));
        IEnumMoniker enumeration;
        table.EnumRunning(out enumeration);
        IBindCtx context;
        Marshal.ThrowExceptionForHR(CreateBindCtx(0, out context));
        try
        {
            var names = new IMoniker[1];
            while (enumeration.Next(1, names, IntPtr.Zero) == 0)
            {
                string name;
                names[0].GetDisplayName(context, null, out name);
                if (name.IndexOf("VisualStudio.DTE.", StringComparison.OrdinalIgnoreCase) < 0)
                {
                    Marshal.ReleaseComObject(names[0]);
                    continue;
                }
                object value;
                table.GetObject(names[0], out value);
                Marshal.ReleaseComObject(names[0]);
                dynamic ide = value;
                string loadedSolution = ide.Solution.FullName;
                if (!String.IsNullOrEmpty(loadedSolution) && String.Equals(Path.GetFullPath(loadedSolution), solution,
                                  StringComparison.OrdinalIgnoreCase))
                {
                    processId = Int32.Parse(name.Substring(name.LastIndexOf(':') + 1));
                    return value;
                }
                Marshal.ReleaseComObject(value);
            }
            return null;
        }
        finally
        {
            Marshal.ReleaseComObject(context);
            Marshal.ReleaseComObject(enumeration);
            Marshal.ReleaseComObject(table);
        }
    }

    private static void Projects(dynamic project, List<string> dirty)
    {
        string file = project.FullName;
        if (!String.IsNullOrEmpty(file) && file.EndsWith(".vcxproj", StringComparison.OrdinalIgnoreCase))
        {
            if (!(bool)project.Saved)
            {
                dirty.Add(file);
            }
            return;
        }
        dynamic items = project.ProjectItems;
        if (items == null) { return; }
        for (int index = 1; index <= (int)items.Count; ++index)
        {
            dynamic child = items.Item(index).SubProject;
            if (child != null) { Projects(child, dirty); }
        }
    }

    [STAThread]
    private static int Main(string[] arguments)
    {
        var serializer = new JavaScriptSerializer();
        serializer.MaxJsonLength = 16 * 1024 * 1024;
        IOleMessageFilter previous;
        CoRegisterMessageFilter(new RetryFilter(), out previous);
        object instance = null;
        try
        {
            if (arguments.Length != 2) { throw new ArgumentException("Expected solution and inspect/request-file"); }
            string solution = Path.GetFullPath(arguments[0]);
            instance = FindIde(solution);
            if (instance == null)
            {
                Console.WriteLine("{\"attached\":false}");
                return 0;
            }
            dynamic ide = instance;
            var dirtyProjects = new List<string>();
            dynamic projects = ide.Solution.Projects;
            for (int index = 1; index <= (int)projects.Count; ++index)
            {
                Projects(projects.Item(index), dirtyProjects);
            }
            var documents = new List<Dictionary<string, object>>();
            dynamic open = ide.Documents;
            for (int index = 1; index <= (int)open.Count; ++index)
            {
                dynamic document = open.Item(index);
                documents.Add(new Dictionary<string, object>
                {
                    { "path", (string)document.FullName }, { "saved", (bool)document.Saved }
                });
            }
            if (arguments[1] != "inspect")
            {
                var moves = serializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(arguments[1]));
                for (int index = (int)open.Count; index >= 1; --index)
                {
                    dynamic document = open.Item(index);
                    string oldPath = document.FullName;
                    string destination;
                    if (!moves.TryGetValue(oldPath, out destination)) { continue; }
                    if (!(bool)document.Saved)
                    {
                        throw new InvalidOperationException("Document changed during synchronization; keep its buffer: " + oldPath);
                    }
                    // Only saved buffers are closed, and only after the transaction has preserved their bytes.
                    document.Close(2); // EnvDTE.vsSaveChanges.vsSaveChangesNo
                    if (File.Exists(destination)) { ide.ItemOperations.OpenFile(destination); }
                }
            }
            Console.WriteLine(serializer.Serialize(new Dictionary<string, object>
            {
                { "attached", true }, { "pid", processId }, { "building", (int)ide.Solution.SolutionBuild.BuildState == 2 },
                { "solution_saved", (bool)ide.Solution.Saved },
                { "dirty_projects", dirtyProjects }, { "documents", documents }
            }));
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(serializer.Serialize(new Dictionary<string, object>
            {
                { "status", "conflict" }, { "message", "Visual Studio coordination: " + error.Message }
            }));
            return 2;
        }
        finally
        {
            if (instance != null) { Marshal.ReleaseComObject(instance); }
            IOleMessageFilter ignored;
            CoRegisterMessageFilter(previous, out ignored);
        }
    }
}
