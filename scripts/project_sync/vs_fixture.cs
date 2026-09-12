// Optional desktop integration test. Only opens the marked disposable fixture, never the user's solution.
using System;
using System.IO;
using System.Runtime.InteropServices;

internal static class CeridVsFixture
{
    private static object Find(dynamic project)
    {
        if ((string)project.Name == "dense") { return project; }
        dynamic items = project.ProjectItems;
        if (items == null) { return null; }
        for (int index = 1; index <= (int)items.Count; ++index)
        {
            dynamic child = items.Item(index).SubProject;
            if (child == null) { continue; }
            object result = Find(child);
            if (result != null) { return result; }
        }
        return null;
    }

    [STAThread]
    private static int Main(string[] arguments)
    {
        object instance = null;
        try
        {
            if (arguments.Length != 2) { throw new ArgumentException("Expected fixture solution and new source"); }
            string solution = Path.GetFullPath(arguments[0]);
            if (solution.IndexOf("\\build\\project-sync-tests\\native-", StringComparison.OrdinalIgnoreCase) < 0
                || Path.GetFileNameWithoutExtension(solution) != "Fixture")
            {
                throw new ArgumentException("Refusing to open an unmarked/nonfixture solution");
            }
            Type type = Type.GetTypeFromProgID("VisualStudio.DTE.18.0", true);
            instance = Activator.CreateInstance(type);
            dynamic ide = instance;
            ide.SuppressUI = true;
            ide.UserControl = false;
            ide.MainWindow.Visible = false;
            ide.Solution.Open(solution);
            dynamic target = null;
            dynamic projects = ide.Solution.Projects;
            for (int index = 1; index <= (int)projects.Count && target == null; ++index)
            {
                target = Find(projects.Item(index));
            }
            if (target == null) { throw new InvalidOperationException("Fixture target was not loaded"); }
            dynamic native = target.Object;
            dynamic filters = native.Filters;
            dynamic sourceFilter = null;
            for (int index = 1; index <= (int)filters.Count; ++index)
            {
                dynamic candidate = filters.Item(index);
                if ((string)candidate.Name == "src") { sourceFilter = candidate; break; }
            }
            if (sourceFilter == null) { sourceFilter = native.AddFilter("src"); }
            sourceFilter.AddFile(Path.GetFullPath(arguments[1]));
            native.AddFilter("empty-from-vs");
            target.Save();
            ide.Solution.SaveAs(solution);
            Console.WriteLine("PASS: isolated Visual Studio COM Add File + Add Filter + Save");
            return 0;
        }
        catch (Exception error)
        {
            Console.Error.WriteLine(error);
            return 1;
        }
        finally
        {
            if (instance != null)
            {
                try { ((dynamic)instance).Quit(); }
                finally { Marshal.ReleaseComObject(instance); }
            }
        }
    }
}
