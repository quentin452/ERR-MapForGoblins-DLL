// find_serialize.java — locate the ER game-data SERIALIZE (GameData -> save buffer) with REAL
// Ghidra function boundaries (the offline capstone CC-scan mislabels ER entries; Ghidra doesn't).
//
//   analyzeHeadless.bat D:\ghidra_proj2 ER -process eldenring.exe -noanalysis \
//     -scriptPath D:\ghidra_scripts -postScript find_serialize.java [0xDLO_LO 0xDLO_HI]
//
// Method: the save serialize is the function that (a) READS GameDataMan (data-ref to its static slot)
// and (b) WRITES a DLIO::DLOutputStream (calls heavily into the DLOutputStream code region). SaveLoad2
// itself never reads GameDataMan (it writes a pre-serialized buffer), so the GameDataMan∩DLOutputStream
// intersection is the serialize, not the write session. Ranks candidates by DLO-call count, then size.
// DLO region defaults to the RVA span around the confirmed DLOutputStream primitives (0x1edxxxx); pass
// two 0xRVA args to override. Writes D:\ghidra_scripts\out_find_serialize.txt.
//@category MapForGoblins
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class find_serialize extends GhidraScript {
    long B; DecompInterface dec; PrintWriter out;
    long DLO_LO=0x1edc000L, DLO_HI=0x1ee1000L;   // DLIO::DLOutputStream code region (RVA), overridable

    void w(String s){ out.println(s); }
    String rva(long p){ return (p>=B && p<B+0x6000000L)? ("er+0x"+Long.toHexString(p-B)) : Long.toHexString(p); }

    // AOB search (masked) -> first match address, or null
    Address findAob(byte[] b, byte[] m) throws Exception {
        Memory mem=currentProgram.getMemory();
        return mem.findBytes(mem.getMinAddress(), b, m, true, monitor);
    }
    // Resolve the GameDataMan static slot via its accessor AOB:
    //   48 8B 05 <disp32> 48 85 C0 74 05 48 8B 40 58 C3 C3   (mov rax,[rip+d]; test; je; mov rax,[rax+58]; ret)
    Address gameDataManSlot() throws Exception {
        byte[] b={0x48,(byte)0x8B,0x05,0,0,0,0,0x48,(byte)0x85,(byte)0xC0,0x74,0x05,0x48,(byte)0x8B,0x40,0x58,(byte)0xC3,(byte)0xC3};
        byte[] m=new byte[b.length]; for(int i=0;i<m.length;i++)m[i]=-1; m[3]=m[4]=m[5]=m[6]=0;
        Address hit=findAob(b,m); if(hit==null) return null;
        int disp=getInt(hit.add(3));           // little-endian disp32
        return hit.add(7).add(disp);           // rip base = insn end (match+7)
    }

    String aob(Address ep,int n){ StringBuilder sb=new StringBuilder();
        try{ for(int i=0;i<n;i++) sb.append(String.format("%02X ",getByte(ep.add(i))&0xff)); }catch(Exception e){} return sb.toString().trim(); }

    // count CALL targets of f whose containing-function entry lands in [lo,hi] (RVA)
    int[] dloCalls(Function f, long lo, long hi){
        int calls=0; Set<Long> uniq=new HashSet<Long>();
        InstructionIterator it=currentProgram.getListing().getInstructions(f.getBody(),true);
        while(it.hasNext()){ Instruction ins=it.next();
            for(Reference r: ins.getReferencesFrom()){
                if(!r.getReferenceType().isCall()) continue;
                Function t=getFunctionContaining(r.getToAddress()); if(t==null) continue;
                long e=t.getEntryPoint().getOffset()-B;
                if(e>=lo && e<hi){ calls++; uniq.add(e); }
            }
        }
        return new int[]{calls, uniq.size()};
    }
    // any referenced defined-string in f containing one of the needles (case-insensitive)?
    String saveStr(Function f, String[] needles){
        InstructionIterator it=currentProgram.getListing().getInstructions(f.getBody(),true); int n=0;
        while(it.hasNext() && n<400){ Instruction ins=it.next(); n++;
            for(Reference r: ins.getReferencesFrom()){
                Data d=getDataAt(r.getToAddress()); if(d==null) continue;
                if(!d.hasStringValue()) continue;
                String s=d.getDefaultValueRepresentation();
                String ls=s.toLowerCase();
                for(String nd: needles) if(ls.contains(nd)) return s;
            }
        }
        return null;
    }
    String decHead(Function f,int lines){ try{ DecompileResults r=dec.decompileFunction(f,120,monitor);
        if(r!=null&&r.decompileCompleted()){ String[] a=r.getDecompiledFunction().getC().split("\n");
            StringBuilder sb=new StringBuilder(); for(int i=0;i<Math.min(lines,a.length);i++) sb.append("  "+a[i]+"\n"); return sb.toString(); } }catch(Exception e){} return "  <no decomp>\n"; }

    public void run() throws Exception {
        B=currentProgram.getImageBase().getOffset();
        String[] args=getScriptArgs();
        if(args.length>=2){ DLO_LO=Long.parseUnsignedLong(args[0].replaceFirst("0x",""),16); DLO_HI=Long.parseUnsignedLong(args[1].replaceFirst("0x",""),16); }
        dec=new DecompInterface(); dec.toggleCCode(true); dec.openProgram(currentProgram);
        out=new PrintWriter(new OutputStreamWriter(new FileOutputStream("D:\\ghidra_scripts\\out_find_serialize.txt"),"UTF-8"));
        w("== find_serialize  imagebase 0x"+Long.toHexString(B)+"  DLO=[0x"+Long.toHexString(DLO_LO)+",0x"+Long.toHexString(DLO_HI)+") ==");

        Address slot=gameDataManSlot();
        if(slot==null){ w("!! GameDataMan accessor AOB not found"); out.flush(); out.close(); println("WROTE (no slot)"); return; }
        w("GameDataMan slot = "+slot+" ("+rva(slot.getOffset())+")");

        // functions that reference the GameDataMan slot
        LinkedHashMap<Long,Function> readers=new LinkedHashMap<Long,Function>();
        for(Reference r: getReferencesTo(slot)){ Function f=getFunctionContaining(r.getFromAddress());
            if(f!=null) readers.put(f.getEntryPoint().getOffset(), f); }
        w("GameDataMan-reading functions: "+readers.size());

        // score each: DLO calls + size
        String[] needles={"userdata","gamedata","save","slot","serial","backup"};
        ArrayList<long[]> rank=new ArrayList<long[]>();          // {dloCalls, uniq, size, entry}
        HashMap<Long,String> strHit=new HashMap<Long,String>();
        for(Map.Entry<Long,Function> e: readers.entrySet()){ Function f=e.getValue();
            int[] dc=dloCalls(f, DLO_LO, DLO_HI); long size=f.getBody().getNumAddresses();
            rank.add(new long[]{dc[0], dc[1], size, e.getKey()});
            if(dc[0]>0){ String s=saveStr(f,needles); if(s!=null) strHit.put(e.getKey(), s); }
        }
        Collections.sort(rank, new Comparator<long[]>(){ public int compare(long[] a,long[] b){
            if(a[0]!=b[0]) return Long.compare(b[0],a[0]); return Long.compare(b[2],a[2]); } });

        w("\n==== ranked candidates (DLOcalls, uniqDLO, size, entry) ====");
        int shown=0;
        for(long[] c: rank){ if(c[0]==0) break; if(shown++>=20) break;
            Address ep=toAddr(c[3]);
            String s=strHit.get(c[3]);
            w(String.format("  %-10s DLO=%d uniq=%d size=%d  str=%s", rva(c[3]), c[0], c[1], c[2], s==null?"-":s));
        }

        // deep-dump the top few for eyeballing
        w("\n==== top-5 decompiled ====");
        int budget=0;
        for(long[] c: rank){ if(c[0]==0 || budget>=5) break; budget++;
            Address ep=toAddr(c[3]); Function f=getFunctionAt(ep);
            w("\n######## "+f.getName()+" @ "+ep+" ("+rva(c[3])+")  size="+c[2]+"  DLOcalls="+c[0]+" ########");
            w("  AOB@entry: "+aob(ep,24));
            w("  callers:");
            Set<Long> seen=new LinkedHashSet<Long>();
            for(Reference r: getReferencesTo(ep)){ Function cf=getFunctionContaining(r.getFromAddress());
                if(cf!=null&&seen.add(cf.getEntryPoint().getOffset())) w("    "+r.getFromAddress()+"  "+cf.getName()); if(seen.size()>=12){w("    ...");break;} }
            w("  --- decomp (head) ---");
            w(decHead(f,70));
        }
        out.flush(); out.close();
        println("WROTE D:\\ghidra_scripts\\out_find_serialize.txt"); println("== DONE ==");
    }
}
