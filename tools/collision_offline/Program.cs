// Offline ER dvdbnd -> collision (hkxbhd/hkxbdt -> inner hkx.dcx) reader for Linux.
//
// Validated 2026-07-06 (docs/re/far_water_surface_disk_re_findings.md §8). Reads straight
// out of the PACKED install (Data0-3.bhd/.bdt next to eldenring.exe) with our own RSA
// (SoulsFormats.BHD5.Read wants a DECRYPTED header), the prime-0x85 path hash, and a .bdt
// slice; then SoulsFormats.BXF4 opens the collision archive and lists the inner hkx.
//
//   dotnet run -- selftest                      # RSA/hash/slice self-check on a known Data0 file
//   dotnet run -- list-collision m10_00_00_00   # inner hi-collision files of one map tile
//   dotnet run -- extract <vpath> <out.bin>     # raw (still-DCX) bytes of any dvdbnd file
//
// KNOWN WALL: the inner h*_######.hkx.dcx are DCX-**KRAK (Oodle)**. SoulsFormats' DCX
// decompress P/Invokes oo2core_6_win64.dll (a Windows PE) which a native Linux dotnet
// cannot load -> the HKX geometry/material decode is blocked HERE until an Oodle route is
// wired (see the findings doc: ooz .so / RPC hybrid / Wine C++ extractor). Everything up to
// and including the BXF listing works offline on Linux today.
//
// GAME dir: $GAME_DIR (else the default Steam path). Collision lives in Data2 (salt GR_map).
using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Numerics;
using System.Reflection;
using System.Security.Cryptography;

static class Dvdbnd {
    public static string GameDir =>
        Environment.GetEnvironmentVariable("GAME_DIR") is string g && g.Length > 0 ? g
        : "/home/iamacat/.local/share/Steam/steamapps/common/ELDEN RING/Game";
    static readonly Assembly SF =
        Assembly.LoadFrom(Path.Combine(AppContext.BaseDirectory, "..", "..", "..", "..", "lib", "Andre.SoulsFormats.dll"));
    public static Type[] Types(Assembly a){ try{return a.GetTypes();}catch(ReflectionTypeLoadException e){return e.Types.Where(x=>x!=null).ToArray();} }
    public static Type T(string full)=>Types(SF).First(x=>x.FullName==full);

    // RSA raw modexp (m = c^e mod n) per 256-byte block, drop each block's leading byte.
    static byte[] RsaDecrypt(byte[] enc, byte[] mod, byte[] exp){
        var n=new BigInteger(mod,true,true); var e=new BigInteger(exp,true,true);
        var outb=new List<byte>(enc.Length/256*255);
        for(int off=0;off<enc.Length;off+=256){
            var c=new BigInteger(enc.AsSpan(off,256),true,true);
            var mb=BigInteger.ModPow(c,e,n).ToByteArray(true,true);
            var full=new byte[256]; Array.Copy(mb,0,full,256-mb.Length,mb.Length);
            outb.AddRange(full.AsSpan(1,255).ToArray());
        }
        return outb.ToArray();
    }
    public static ulong Hash(string vpath){
        var s=vpath.ToLowerInvariant().Replace('\\','/'); if(!s.StartsWith("/")) s="/"+s;
        ulong h=0; foreach(char ch in s) h=h*0x85UL+ch; return h;
    }

    public class Arch { public string bdt; public Dictionary<ulong,object> map; }
    public static Arch Open(string name, byte[] mod, byte[] exp){
        var enc=File.ReadAllBytes(Path.Combine(GameDir,name+".bhd"));
        var plain=RsaDecrypt(enc,mod,exp);
        var gameE=Enum.Parse(T("SoulsFormats.BHD5+Game"),"EldenRing");
        var bhd5=T("SoulsFormats.BHD5").GetMethod("Read",BindingFlags.Public|BindingFlags.Static)
                   .Invoke(null,new object[]{new Memory<byte>(plain),gameE});
        var buckets=(System.Collections.IEnumerable)bhd5.GetType().GetProperty("Buckets").GetValue(bhd5);
        var map=new Dictionary<ulong,object>();
        foreach(var b in buckets){ var bt=b.GetType();
            int cnt=(int)bt.GetProperty("Count").GetValue(b); var idx=bt.GetProperty("Item");
            for(int i=0;i<cnt;i++){ var fh=idx.GetValue(b,new object[]{i});
                map[(ulong)fh.GetType().GetProperty("FileNameHash").GetValue(fh)]=fh; } }
        return new Arch{bdt=Path.Combine(GameDir,name+".bdt"),map=map};
    }
    public static byte[] Slice(Arch a, string vpath){
        if(!a.map.TryGetValue(Hash(vpath),out var fh)) return null;
        var t=fh.GetType();
        long off=(long)t.GetProperty("FileOffset").GetValue(fh);
        int padded=(int)t.GetProperty("PaddedFileSize").GetValue(fh);
        var aes=t.GetProperty("AESKey").GetValue(fh);
        var buf=new byte[padded];
        using(var fs=File.OpenRead(a.bdt)){ fs.Seek(off,SeekOrigin.Begin); int n=0; while(n<padded){int r=fs.Read(buf,n,padded-n); if(r<=0)break; n+=r;} }
        if(aes!=null){
            var key=(byte[])aes.GetType().GetProperty("Key").GetValue(aes);
            var ranges=(System.Collections.IEnumerable)aes.GetType().GetProperty("Ranges").GetValue(aes);
            using var acs=Aes.Create(); acs.Mode=CipherMode.ECB; acs.Padding=PaddingMode.None; acs.Key=key;
            foreach(var r in ranges){ var rt=r.GetType();
                long st=(long)rt.GetProperty("StartOffset").GetValue(r), en=(long)rt.GetProperty("EndOffset").GetValue(r);
                if(st<0||en<=st) continue; int len=(int)(en-st); len-=len%16; if(len<=0) continue;
                var dec=acs.CreateDecryptor().TransformFinalBlock(buf,(int)st,len);
                Array.Copy(dec,0,buf,(int)st,len);
            }
        }
        return buf;
    }
    // Data2 = the map archive (salt GR_map). Collision + MSB + mapbnd live here.
    public static Arch OpenMap()=>Open("Data2",K.DATA2_MOD,K.DATA2_EXP);

    public static IEnumerable<(string name,int len,byte[] head)> BxfFiles(byte[] bhd, byte[] bdt){
        var bxf=T("SoulsFormats.BXF4");
        var read=bxf.GetMethods(BindingFlags.Public|BindingFlags.Static).First(m=>m.Name=="Read"
            && m.GetParameters().Length==2 && m.GetParameters().All(p=>p.ParameterType==typeof(Memory<byte>)));
        var b=read.Invoke(null,new object[]{new Memory<byte>(bhd),new Memory<byte>(bdt)});
        var files=(System.Collections.IEnumerable)bxf.GetProperty("Files").GetValue(b);
        foreach(var f in files){ var ft=f.GetType();
            string name=(string)ft.GetProperty("Name").GetValue(f);
            var bm=ft.GetProperty("Bytes").GetValue(f);
            byte[] by = bm is byte[] ba ? ba : bm is Memory<byte> mm ? mm.ToArray() : null;
            yield return (name, by?.Length??0, by==null?Array.Empty<byte>():by.Take(0x30).ToArray());
        }
    }
}

static class Program {
    static string Magic(byte[] b)=> b==null?"<null>": System.Text.Encoding.ASCII.GetString(b,0,Math.Min(4,b.Length)).Replace("\0","·");
    static int Main(string[] args){
        if(args.Length==0 || args[0]=="selftest"){
            var d0=Dvdbnd.Open("Data0",K.DATA0_MOD,K.DATA0_EXP);
            var known=Dvdbnd.Slice(d0,"menu/hi/01_common.sblytbnd.dcx");
            Console.WriteLine($"Data0 entries={d0.map.Count} (expect 5824)");
            Console.WriteLine($"known sblytbnd len={known?.Length} magic={Magic(known)} (expect 21056, DCX)");
            Console.WriteLine($"Data2 entries={Dvdbnd.OpenMap().map.Count} (expect 39684)");
            return 0;
        }
        if(args[0]=="list-collision" && args.Length>=2){
            var m=args[1]; // e.g. m10_00_00_00
            var mm=m.Substring(0,3); // m10
            var map=Dvdbnd.OpenMap();
            var bhd=Dvdbnd.Slice(map,$"map/{mm}/{m}/h{m.Substring(1)}.hkxbhd");
            var bdt=Dvdbnd.Slice(map,$"map/{mm}/{m}/h{m.Substring(1)}.hkxbdt");
            if(bhd==null||bdt==null){ Console.Error.WriteLine($"no hi-collision for {m}"); return 2; }
            int i=0; foreach(var (name,len,head) in Dvdbnd.BxfFiles(bhd,bdt)){
                string dcx = head.Length>=0x2c ? System.Text.Encoding.ASCII.GetString(head,0x28,4) : "?";
                Console.WriteLine($"[{i++}] {name}  len={len}  inner-dcx={dcx}");
            }
            Console.WriteLine($"total={i}");
            return 0;
        }
        if(args[0]=="extract" && args.Length>=3){
            var arch = args[1].StartsWith("map/") ? Dvdbnd.OpenMap() : Dvdbnd.Open("Data0",K.DATA0_MOD,K.DATA0_EXP);
            var b=Dvdbnd.Slice(arch,args[1]);
            if(b==null){ Console.Error.WriteLine("not found: "+args[1]); return 2; }
            File.WriteAllBytes(args[2],b);
            Console.WriteLine($"wrote {b.Length} bytes ({Magic(b)}) -> {args[2]}");
            return 0;
        }
        Console.Error.WriteLine("usage: selftest | list-collision <mMM_XX_YY_ZZ> | extract <vpath> <out>");
        return 1;
    }
}
