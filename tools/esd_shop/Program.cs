using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using SoulsFormats;

// Minimal talk-ESD command inspector (rebuild of the uncommitted esd_dump, scoped to the
// merchant-pin RE). Finds the shop-open talk command + proves the shopRange join.
//   esd_shop <talkesdbnd.dcx> hist           -> CommandBank:CommandID histogram
//   esd_shop <talkesdbnd.dcx> dump [bank:id]  -> per-command args (EzState-decoded); optional filter
//   esd_shop <talkesdbnd.dcx> raw  [bank:id]  -> raw EzState arg bytecode (hex)
//
// ESD command ARGUMENTS are EzState bytecode EXPRESSIONS, not plain int32. The overwhelmingly common
// form is a literal: 0x82 <int32 LE> 0xA1 (push-int, end) — 78% of args in m00. DecodeArg handles that
// exactly (the old decoder read the first 4 bytes INCLUDING the 0x82 opcode → garbage). The remaining
// ~21% are stack-machine expressions (operators 0x86/0x89/0x95/0x99…, function calls 0x6F, float pushes)
// that need a full EzState disassembler (SoulsFormats' EzSemble, not in the Andre fork) — those are shown
// as <expr:…> raw so nothing is silently mis-decoded. See docs/re/esd_ezstate_decoder_re_findings.md.
static class Program
{
    // Decode one EzState command-argument expression. Returns the literal int as a string for the
    // canonical `82 <i32> A1` form; otherwise "<expr:hex>" (needs the full EzSemble evaluator).
    static string DecodeArg(byte[] b)
    {
        if (b.Length == 6 && b[0] == 0x82 && b[5] == 0xA1)
            return BitConverter.ToInt32(b, 1).ToString();          // push-int32 literal (the 78% case)
        if (b.Length == 1 && b[0] == 0xA1) return "()";            // empty expression
        return "<expr:" + BitConverter.ToString(b).Replace("-", " ") + ">";
    }
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
                        if (mode == "raw")
                        {
                            // Hexdump each argument's EzState bytecode (an expression, NOT a plain int32) so
                            // the format is visible — the missing decoder (EzSemble) turns these into values.
                            var hex = cmd.Arguments.Select(b => BitConverter.ToString(b).Replace("-", " ")).ToList();
                            Console.WriteLine($"{tid}  {key}  args=[{string.Join("  |  ", hex)}]");
                            continue;
                        }
                        var decoded = cmd.Arguments.Select(DecodeArg).ToList();
                        Console.WriteLine($"{tid}  {key}  args=[{string.Join(", ", decoded)}]");
                    }
        }
        if (mode == "hist")
            foreach (var kv in hist) Console.WriteLine($"{kv.Key}\t{kv.Value}");
    }
}
