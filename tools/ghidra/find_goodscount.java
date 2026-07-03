// find_goodscount.java — locate EquipInventoryData::GetItemQuantity(id)-style "how many of item
// X does the player hold" in the equip/player game-data TU. The counter is a NON-VIRTUAL member
// (EquipInventoryData vtable has only 3 slots) that walks the held container and resolves each
// {GaItemHandle,qty} entry via the CSGaitemImp slot-map (er+0x670xxx). So: scan every function in
// the equip/player-data TU [LO,HI), and flag ones that LOOP and CALL into the GaItem region.
//
//   analyzeHeadless.bat D:\ghidra_proj2 ER -process eldenring.exe -noanalysis \
//     -scriptPath D:\ghidra_scripts -postScript find_goodscount.java [0xLO 0xHI] [0xGA_LO 0xGA_HI]
//
// Defaults: TU=[0x245000,0x260000) (EquipGameData 0x2458f0 / EquipInventoryData 0x24bxxx /
// EquipItemData 0x24fxxx / PlayerGameData 0x25db40 / player-serialize 0x257f20 all cluster here),
// GaItem region=[0x670000,0x683000) (CSGaitemImp ctor 0x670c70, resolver 0x670f90, hash 0x682200).
// Writes D:\ghidra_scripts\out_find_goodscount.txt.
//@category MapForGoblins
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import java.io.*;
import java.util.*;

public class find_goodscount extends GhidraScript {
    long B; DecompInterface dec; PrintWriter out;
    long LO=0x245000L, HI=0x260000L, GA_LO=0x670000L, GA_HI=0x683000L;

    void w(String s){ out.println(s); }
    String rva(long p){ return (p>=B && p<B+0x6000000L)? ("er+0x"+Long.toHexString(p-B)) : Long.toHexString(p); }
    String aob(Address ep,int n){ StringBuilder sb=new StringBuilder();
        try{ for(int i=0;i<n;i++) sb.append(String.format("%02X ",getByte(ep.add(i))&0xff)); }catch(Exception e){} return sb.toString().trim(); }
    String decC(Function f){ try{ DecompileResults r=dec.decompileFunction(f,150,monitor);
        if(r!=null&&r.decompileCompleted()) return r.getDecompiledFunction().getC(); }catch(Exception e){} return null; }

    // does f contain a backward branch inside its own body (a loop)?
    boolean hasLoop(Function f){
        InstructionIterator it=currentProgram.getListing().getInstructions(f.getBody(),true);
        while(it.hasNext()){ Instruction ins=it.next(); long a=ins.getAddress().getOffset();
            for(Reference r: ins.getReferencesFrom()){
                if(!r.getReferenceType().isFlow()) continue;
                long t=r.getToAddress().getOffset();
                if(t<a && t>=f.getEntryPoint().getOffset()) return true; } }
        return false;
    }
    // count calls from f whose target lands in the GaItem region [GA_LO,GA_HI)
    int gaCalls(Function f){ int n=0;
        InstructionIterator it=currentProgram.getListing().getInstructions(f.getBody(),true);
        while(it.hasNext()){ Instruction ins=it.next();
            for(Reference r: ins.getReferencesFrom()){ if(!r.getReferenceType().isCall()) continue;
                Function t=getFunctionContaining(r.getToAddress()); if(t==null) continue;
                long e=t.getEntryPoint().getOffset()-B; if(e>=GA_LO && e<GA_HI) n++; } }
        return n;
    }
    void callers(Function f){ Set<Long> seen=new LinkedHashSet<Long>();
        for(Reference r: getReferencesTo(f.getEntryPoint())){ Function c=getFunctionContaining(r.getFromAddress());
            if(c!=null&&seen.add(c.getEntryPoint().getOffset())) w("    caller "+r.getFromAddress()+"  "+rva(c.getEntryPoint().getOffset())+" "+c.getName());
            if(seen.size()>=16){ w("    ...(16+)"); break; } } }

    public void run() throws Exception {
        B=currentProgram.getImageBase().getOffset();
        String[] a=getScriptArgs();
        if(a.length>=2){ LO=Long.parseUnsignedLong(a[0].replaceFirst("0x",""),16); HI=Long.parseUnsignedLong(a[1].replaceFirst("0x",""),16); }
        if(a.length>=4){ GA_LO=Long.parseUnsignedLong(a[2].replaceFirst("0x",""),16); GA_HI=Long.parseUnsignedLong(a[3].replaceFirst("0x",""),16); }
        dec=new DecompInterface(); dec.toggleCCode(true); dec.openProgram(currentProgram);
        out=new PrintWriter(new OutputStreamWriter(new FileOutputStream("D:\\ghidra_scripts\\out_find_goodscount.txt"),"UTF-8"));
        w("== find_goodscount  imagebase 0x"+Long.toHexString(B)+"  TU=[0x"+Long.toHexString(LO)+",0x"+Long.toHexString(HI)+")  GA=[0x"+Long.toHexString(GA_LO)+",0x"+Long.toHexString(GA_HI)+") ==");

        // enumerate functions with entry in the TU range
        FunctionIterator fit=currentProgram.getFunctionManager().getFunctions(true);
        ArrayList<Function> fns=new ArrayList<Function>();
        while(fit.hasNext()){ Function f=fit.next(); long e=f.getEntryPoint().getOffset()-B;
            if(e>=LO && e<HI) fns.add(f); }
        w("functions in TU: "+fns.size());

        // index line for every fn + collect candidates (loop && calls GaItem region)
        w("\n==== index (rva, size, params, loop, gaCalls) ====");
        ArrayList<Function> cand=new ArrayList<Function>();
        HashMap<Long,Integer> gaOf=new HashMap<Long,Integer>();
        for(Function f: fns){ long e=f.getEntryPoint().getOffset()-B; long sz=f.getBody().getNumAddresses();
            boolean lp=hasLoop(f); int np=f.getParameterCount(); int ga=gaCalls(f); gaOf.put(e,ga);
            w(String.format("  %-11s size=%-5d p=%d loop=%s ga=%d", rva(f.getEntryPoint().getOffset()), sz, np, lp?"Y":"n", ga));
            if(lp && ga>0) cand.add(f);
        }
        // also add loop+multi-arg fns (in case the resolver is inlined -> no GaItem call)
        for(Function f: fns){ if(cand.contains(f)) continue;
            if(hasLoop(f) && f.getParameterCount()>=2 && f.getBody().getNumAddresses()<0x500) cand.add(f); }

        w("\n==== candidates: "+cand.size()+" (loop && (gaCalls>0 || >=2 params)) ====");
        int budget=0;
        for(Function f: cand){ if(budget++>=24) { w("  ...(more candidates truncated)"); break; }
            long e=f.getEntryPoint().getOffset()-B;
            w("\n######## "+f.getName()+" @ "+rva(f.getEntryPoint().getOffset())+"  size="+f.getBody().getNumAddresses()+"  gaCalls="+gaOf.get(e)+" ########");
            w("  AOB@entry: "+aob(f.getEntryPoint(),24));
            callers(f);
            String c=decC(f); if(c==null){ w("  <no decomp>"); continue; }
            w("  --- decompiled ---");
            for(String ln: c.split("\n")) w("  "+ln);
        }
        out.flush(); out.close();
        println("WROTE D:\\ghidra_scripts\\out_find_goodscount.txt"); println("== DONE ==");
    }
}
