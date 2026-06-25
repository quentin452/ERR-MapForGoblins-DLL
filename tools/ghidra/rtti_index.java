// One-time RTTI index: every C++ class -> vtable RVA -> TypeDescriptor RVA -> ctors.
//   analyzeHeadless.bat D:\ghidra_proj2 ER -process eldenring.exe -noanalysis \
//     -scriptPath D:\ghidra_scripts -postScript rtti_index.java
// Efficient (no per-class memory scan):
//   1) find all Complete Object Locators in .rdata via the x64 self-RVA (COL+0x14 == its own RVA);
//      COL+0x0C = TypeDescriptor RVA.
//   2) one byte pass over .rdata/.data: any qword == a COL VA => vtable starts at qword_addr+8.
//   3) ctors = Ghidra xrefs to the vtable.
// Output: D:\ghidra_scripts\rtti_index.txt (TSV: vtable_rva  td_rva  ctorRVAs  name), grep-able.
//@category MapForGoblins
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.mem.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class rtti_index extends GhidraScript {
    long B; Memory mem;
    long u32(byte[] b,int o){ return ((b[o]&0xffL))|((b[o+1]&0xffL)<<8)|((b[o+2]&0xffL)<<16)|((b[o+3]&0xffL)<<24); }
    long u64(byte[] b,int o){ return u32(b,o)|(u32(b,o+4)<<32); }
    String nameAt(long tdVA){ try { Address a=toAddr(tdVA+0x10); StringBuilder sb=new StringBuilder();
        for(int i=0;i<240;i++){ byte c=getByte(a.add(i)); if(c==0)break; sb.append((char)c);} return sb.toString(); }catch(Exception e){ return "?"; } }
    byte[] blockBytes(MemoryBlock mb){ try { byte[] buf=new byte[(int)mb.getSize()]; mem.getBytes(mb.getStart(),buf); return buf; }catch(Exception e){ return null; } }

    public void run() throws Exception {
        B=currentProgram.getImageBase().getOffset(); mem=currentProgram.getMemory();
        long imgEnd=B+0x6000000L;
        // blocks to scan for COLs + vtable pointers
        MemoryBlock rdata=null,data=null;
        for(MemoryBlock mb: mem.getBlocks()){ if(mb.getName().equals(".rdata")) rdata=mb; if(mb.getName().equals(".data")) data=mb; }
        println("imagebase 0x"+Long.toHexString(B)+"  .rdata="+rdata+"  .data="+data);

        // 1) COLs in .rdata: at 4-aligned p, COL+0x14 RVA == (p - B), sig(+0)<=1, td RVA(+0xC) in image
        HashMap<Long,Long> colToTd=new HashMap<Long,Long>();   // COL VA -> TD VA
        byte[] rb=blockBytes(rdata); long rbase=rdata.getStart().getOffset();
        for(int o=0;o+0x18<=rb.length;o+=4){ long self=u32(rb,o+0x14); long colVA=rbase+o;
            if(self==(colVA-B)){ long sig=u32(rb,o); long tdRVA=u32(rb,o+0x0C);
                if(sig<=1 && tdRVA>0 && B+tdRVA<imgEnd){ colToTd.put(colVA, B+tdRVA); } } }
        println("COLs found: "+colToTd.size());

        // 2) any qword in .rdata/.data == a COL VA => vtable @ thatAddr+8
        //    map TD -> list of vtable VAs
        HashMap<Long,ArrayList<Long>> tdToVts=new HashMap<Long,ArrayList<Long>>();
        for(MemoryBlock mb: new MemoryBlock[]{rdata,data}){ byte[] bb=blockBytes(mb); long bbase=mb.getStart().getOffset();
            for(int o=0;o+8<=bb.length;o+=8){ long q=u64(bb,o); Long td=colToTd.get(q);
                if(td!=null){ long vt=bbase+o+8; tdToVts.computeIfAbsent(td,kk->new ArrayList<Long>()).add(vt); } } }
        println("TDs with a vtable: "+tdToVts.size());

        // 3) emit, sorted by name; ctors = xrefs to vtable
        PrintWriter out=new PrintWriter(new OutputStreamWriter(new FileOutputStream("D:\\ghidra_scripts\\rtti_index.txt"),"UTF-8"));
        out.println("# RTTI index for eldenring.exe (imagebase 0x"+Long.toHexString(B)+")");
        out.println("# vtable_rva\ttd_rva\tctor_rvas\tmangled_name   (RVAs are er_base-relative)");
        ArrayList<long[]> rows=new ArrayList<long[]>();   // {vtRVA, tdRVA} ; name fetched at sort
        ArrayList<String> lines=new ArrayList<String>();
        int nvt=0;
        for(Map.Entry<Long,ArrayList<Long>> e: tdToVts.entrySet()){ long tdVA=e.getKey(); String nm=nameAt(tdVA);
            for(long vtVA: e.getValue()){ nvt++;
                // ctors = functions referencing the vtable
                LinkedHashSet<Long> ctors=new LinkedHashSet<Long>();
                for(Reference r: getReferencesTo(toAddr(vtVA))){ Function f=getFunctionContaining(r.getFromAddress());
                    if(f!=null){ ctors.add(f.getEntryPoint().getOffset()-B); if(ctors.size()>=6) break; } }
                StringBuilder cs=new StringBuilder(); boolean first=true;
                for(long c: ctors){ if(!first) cs.append(","); cs.append("0x"+Long.toHexString(c)); first=false; }
                lines.add(String.format("0x%x\t0x%x\t%s\t%s", vtVA-B, tdVA-B, cs.length()>0?cs:"-", nm)); } }
        Collections.sort(lines, (x,y)->{ String nx=x.substring(x.lastIndexOf('\t')+1), ny=y.substring(y.lastIndexOf('\t')+1); return nx.compareTo(ny); });
        for(String l: lines) out.println(l);
        out.flush(); out.close();
        println("vtables emitted: "+nvt+"  -> WROTE D:\\ghidra_scripts\\rtti_index.txt");
        println("== DONE ==");
    }
}
