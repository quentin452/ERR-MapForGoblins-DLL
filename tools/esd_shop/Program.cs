using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SoulsFormats;

// Minimal talk-ESD command inspector (rebuild of the uncommitted esd_dump, scoped to the
// merchant-pin RE). Finds the shop-open talk command + proves the shopRange join.
//   esd_shop <talkesdbnd.dcx> hist          -> CommandBank:CommandID histogram
//   esd_shop <talkesdbnd.dcx> dump [bank:id] -> per-command args (int32-decoded); optional filter
static class Program
{
    static IEnumerable<ESD.CommandCall> Cmds(ESD.State st)
    {
        foreach (var c in st.EntryCommands) yield return c;
        foreach (var c in st.WhileCommands) yield return c;
        foreach (var c in st.ExitCommands) yield return c;
        foreach (var cond in st.Conditions)
            foreach (var c in Cond(cond)) yield return c;
    }
    static IEnumerable<ESD.CommandCall> Cond(ESD.Condition cond)
    {
        foreach (var c in cond.PassCommands) yield return c;
        foreach (var sub in cond.Subconditions)
            foreach (var c in Cond(sub)) yield return c;
    }

    static void Main(string[] args)
    {
        string path = args[0];
        string mode = args.Length > 1 ? args[1] : "hist";
        string filter = args.Length > 2 ? args[2] : null;

        BND4 bnd = BND4.Read(path);   // SoulsFile.Read auto-decompresses DCX
        var hist = new SortedDictionary<string, int>();

        foreach (var f in bnd.Files)
        {
            if (!f.Name.EndsWith(".esd")) continue;
            ESD esd;
            try { esd = ESD.Read(f.Bytes); } catch { continue; }
            string tid = Path.GetFileNameWithoutExtension(f.Name);   // t<TalkID>

            foreach (var sg in esd.StateGroups)
                foreach (var stkv in sg.Value)
                    foreach (var cmd in Cmds(stkv.Value))
                    {
                        string key = $"{cmd.CommandBank}:{cmd.CommandID}";
                        if (mode == "hist") { hist[key] = hist.GetValueOrDefault(key) + 1; continue; }
                        if (filter != null && key != filter) continue;
                        var decoded = cmd.Arguments.Select(b =>
                            b.Length >= 4 ? BitConverter.ToInt32(b, 0).ToString()
                                          : (b.Length == 1 ? ((int)b[0]).ToString() : $"[{b.Length}b]")).ToList();
                        Console.WriteLine($"{tid}  {key}  args=[{string.Join(", ", decoded)}]");
                    }
        }
        if (mode == "hist")
            foreach (var kv in hist) Console.WriteLine($"{kv.Key}\t{kv.Value}");
    }
}
