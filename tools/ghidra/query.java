// Reusable Ghidra query — no more one-off scripts per RE.
//   analyzeHeadless.bat D:\ghidra_proj2 ER -process eldenring.exe -noanalysis \
//     -scriptPath D:\ghidra_scripts -postScript query.java <target> [<target> ...]
// <target> forms:
//   0x<hex>         -> address (>= imagebase = VA, else RVA): decompile the fn + entry AOB +
//                      rip-relative static globals + callers.
//   name:<substr>   -> RTTI mangled-name substring: walk every matching TypeDescriptor -> vtable
//                      -> vmethods + ctors (decompiled).
//   (a bare token with no 0x/name: prefix is treated as name:<token>)
// Writes a clean UTF-8 report to D:\ghidra_scripts\out_query.txt (headless logging mangles multi-line).
//@category MapForGoblins
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.mem.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class query extends GhidraScript {
    long B; DecompInterface dec; PrintWriter out;
    void w(String s){ out.println(s); }
    String decC(Function f){ try { DecompileResults r=dec.decompileFunction(f,150,monitor);
        if(r!=null&&r.decompileCompleted()) return r.getDecompiledFunction().getC(); }catch(Exception e){ w("  decErr:"+e);} return null; }
    List<Address> findAscii(String s) throws Exception {
        List<Address> h=new ArrayList<Address>(); byte[] b=s.getBytes("US-ASCII"); byte[] m=new byte[b.length];
        for(int i=0;i<m.length;i++)m[i]=(byte)0xff;
        Memory mem=currentProgram.getMemory(); Address at=mem.getMinAddress();
        while(at!=null&&h.size()<400){ Address x=mem.findBytes(at,b,m,true,monitor); if(x==null)break; h.add(x); at=x.add(1);} return h;
    }
    List<Address> scan32(int v) throws Exception {
        List<Address> h=new ArrayList<Address>();
        byte[] b={(byte)v,(byte)(v>>8),(byte)(v>>16),(byte)(v>>24)}; byte[] m={-1,-1,-1,-1};
        Memory mem=currentProgram.getMemory(); Address at=mem.getMinAddress();
        while(at!=null&&h.size()<200){ Address x=mem.findBytes(at,b,m,true,monitor); if(x==null)break; h.add(x); at=x.add(1);} return h;
    }
    List<Address> scan64(long v) throws Exception {
        List<Address> h=new ArrayList<Address>(); byte[] b=new byte[8],m=new byte[8];
        for(int i=0;i<8;i++){b[i]=(byte)(v>>(8*i));m[i]=-1;}
        Memory mem=currentProgram.getMemory(); Address at=mem.getMinAddress();
        while(at!=null&&h.size()<200){ Address x=mem.findBytes(at,b,m,true,monitor); if(x==null)break; h.add(x); at=x.add(1);} return h;
    }
    Address cstrStart(Address hit){ try { Address c=hit; for(int i=0;i<240;i++){ Address p=c.subtract(1); if(getByte(p)==0) return c; c=p; } }catch(Exception e){} return hit; }
    String readName(Address ns){ try { StringBuilder sb=new StringBuilder(); for(int i=0;i<240;i++){ byte c=getByte(ns.add(i)); if(c==0)break; sb.append((char)c);} return sb.toString(); }catch(Exception e){ return "?"; } }
    Address vtableOfTD(Address td){ try { int rva=(int)(td.getOffset()-B);
        for(Address colTd: scan32(rva)){ Address col=colTd.subtract(0x0c);
            for(Address cp: scan64(col.getOffset())){ Address vt=cp.add(8);
                try { long fp=getLong(vt); if(getFunctionContaining(toAddr(fp))!=null) return vt; }catch(Exception e){} } } }catch(Exception e){} return null; }
    String rva(long p){ return (p>=B && p<B+0x6000000L)? ("er+0x"+Long.toHexString(p-B)) : Long.toHexString(p); }
    void aob(Address ep,int n){ try { StringBuilder sb=new StringBuilder(); for(int i=0;i<n;i++) sb.append(String.format("%02X ",getByte(ep.add(i))&0xff)); w("  AOB@entry: "+sb.toString().trim()); }catch(Exception e){} }
    void ripGlobals(Function f){ if(f==null) return; w("  -- rip-relative static globals --");
        InstructionIterator it=currentProgram.getListing().getInstructions(f.getBody(),true); int n=0;
        while(it.hasNext()&&n<50){ Instruction ins=it.next();
            for(int op=0; op<ins.getNumOperands(); op++) for(Object o: ins.getOpObjects(op)) if(o instanceof Address){ Address a=(Address)o; long off=a.getOffset();
                if(off>=B&&off<B+0x6000000L&&getFunctionContaining(a)==null){ w(String.format("    %s: %-7s -> %s (er+0x%x)",ins.getAddress(),ins.getMnemonicString(),a,off-B)); n++; } } } }
    void callers(Function f){ if(f==null) return; w("  -- callers --");
        Set<Long> seen=new LinkedHashSet<Long>();
        for(Reference r: getReferencesTo(f.getEntryPoint())){ Function c=getFunctionContaining(r.getFromAddress());
            if(c!=null&&seen.add(c.getEntryPoint().getOffset())) w("    "+r.getFromAddress()+"  "+c.getName()+" @ "+c.getEntryPoint()); if(seen.size()>=30){ w("    ...(30+)"); break; } } }
    void dumpFn(Function f){ if(f==null){ w("  <no function>"); return; }
        w("\n######## "+f.getName()+" @ "+f.getEntryPoint()+" size="+f.getBody().getNumAddresses()+" ########");
        aob(f.getEntryPoint(),24); ripGlobals(f); callers(f);
        String c=decC(f); if(c==null){ w("  <no decomp>"); return; } w("  --- decompiled ---");
        for(String ln: c.split("\n")) w("  "+ln); }
    void doAddr(long v){ long va = v>=B ? v : B+v; Address a=toAddr(va);
        w("\n================ ADDR "+a+" ("+rva(va)+") ================");
        Function f=getFunctionAt(a); if(f==null) f=getFunctionContaining(a); dumpFn(f); }
    void doName(String sub) throws Exception {
        w("\n================ NAME ~ \""+sub+"\" ================");
        LinkedHashSet<Long> tds=new LinkedHashSet<Long>();
        for(Address hit: findAscii(sub)){ Address ns=cstrStart(hit); String nm=readName(ns);
            if(nm.startsWith(".?A")&&nm.contains(sub)) tds.add(ns.getOffset()); }
        if(tds.isEmpty()){ w("  (no RTTI TypeDescriptor name contains \""+sub+"\")"); return; }
        for(Long s: tds){ Address ns=toAddr(s); Address td=ns.subtract(0x10);
            w("\n---- "+readName(ns)+"   TD "+td+" ("+rva(td.getOffset())+") ----");
            Address vt=vtableOfTD(td);
            if(vt==null){ w("  <no vtable: abstract/template-only or not instantiated>"); continue; }
            w("  vtable "+vt+" ("+rva(vt.getOffset())+")");
            try { for(int i=0;i<6;i++){ long p=getLong(vt.add(i*8L)); if(p<B||p>B+0x6000000L){ w(String.format("    vt[%d]->%s (stop)",i,rva(p))); break; }
                Function vf=getFunctionContaining(toAddr(p)); w(String.format("    vt[%d]-> %s %s",i,rva(p),vf!=null?vf.getName():"?")); } }catch(Exception e){}
            Set<Long> ctors=new LinkedHashSet<Long>();
            for(Reference r: getReferencesTo(vt)){ Function f=getFunctionContaining(r.getFromAddress()); if(f!=null) ctors.add(f.getEntryPoint().getOffset()); }
            w("  ctors/users (vtable refs): "+ctors.size());
            int budget=0; for(long ep: ctors){ if(budget++>=3) break; dumpFn(getFunctionAt(toAddr(ep))); } }
    }
    public void run() throws Exception {
        B=currentProgram.getImageBase().getOffset();
        dec=new DecompInterface(); dec.toggleCCode(true); dec.openProgram(currentProgram);
        out=new PrintWriter(new OutputStreamWriter(new FileOutputStream("D:\\ghidra_scripts\\out_query.txt"),"UTF-8"));
        String[] args=getScriptArgs();
        w("== query  imagebase 0x"+Long.toHexString(B)+"  args="+Arrays.toString(args)+" ==");
        if(args.length==0){ w("usage: query.java <0xADDR|name:SUBSTR|SUBSTR> ..."); }
        for(String a: args){ a=a.trim(); if(a.isEmpty()) continue;
            if(a.startsWith("0x")||a.startsWith("0X")) doAddr(Long.parseUnsignedLong(a.substring(2),16));
            else if(a.startsWith("name:")) doName(a.substring(5));
            else doName(a);
        }
        out.flush(); out.close();
        println("WROTE D:\\ghidra_scripts\\out_query.txt"); println("== DONE ==");
    }
}
