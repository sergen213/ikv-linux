// Correct false no-return propagation in an isolated analysis project only.
// @category IKV
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Instruction;
import ghidra.program.model.listing.FlowOverride;
import java.util.ArrayDeque;
import java.util.HashSet;
import java.util.Iterator;
import java.util.LinkedHashSet;
import java.util.Set;
import ghidra.program.disassemble.Disassembler;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.listing.Listing;
import ghidra.program.model.pcode.PcodeOp;
import ghidra.program.model.lang.Register;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.symbol.Reference;
import ghidra.program.model.symbol.RefType;
import ghidra.program.model.symbol.FlowType;
import ghidra.program.model.symbol.SourceType;

public class AssetReturnTypes extends GhidraScript {
 private static final int MAX_SEEDS=16, MAX_INSTRUCTIONS=4096, MAX_BYTES=65536;
 private final Set<Address> returningCleanup=new HashSet<>();
 private FunctionManager functions;
 private Listing listing;
 private AddressSet executable;
 private Disassembler decoder;

 public void run() throws Exception {
  for (String address : new String[]{"0049fbd0","0056e033","0056dc5d"}) {
   Function f=currentProgram.getFunctionManager().getFunctionAt(toAddr(address));
   if(f==null)throw failure(toAddr(address),"required cleanup function missing");
   f.setNoReturn(false);println("returning allocator cleanup "+address);
   returningCleanup.add(toAddr(address));
  }
  // Actual PE continuations after returning free(): ambd RET 004a192e; resize RET 0047c7cd.
  for (String address : new String[]{"004a18d2","0047c729","0047c756"}) {
   Instruction instruction=currentProgram.getListing().getInstructionAt(toAddr(address));
   if(instruction!=null)instruction.setFlowOverride(FlowOverride.NONE);
  }
  for (String address : new String[]{"004a18d7","0047c72e","0047c75b"})disassemble(toAddr(address));
  for (String[] range : new String[][]{{"004a1870","004a192e"},{"0047c690","0047c7cf"}}) {
   Function f=currentProgram.getFunctionManager().getFunctionAt(toAddr(range[0]));
   if(f!=null) {
    AddressSet body=new AddressSet(f.getBody());body.add(toAddr(range[0]),toAddr(range[1]));
    f.setBody(body);println("restored returning ambient body "+range[0]);
   }
  }
  Set<Address> seeds=new LinkedHashSet<>();
  for(String arg:getScriptArgs()) {
   if(!arg.matches("[0-9a-fA-F]{8}"))throw new IllegalArgumentException("Expected exact seed entrypoint: "+arg);
   seeds.add(toAddr(arg));
   if(seeds.size()>MAX_SEEDS)throw new IllegalArgumentException("At most "+MAX_SEEDS+" unique seeds allowed");
  }
  functions=currentProgram.getFunctionManager();
  listing=currentProgram.getListing();
  executable=new AddressSet(currentProgram.getMemory().getLoadedAndInitializedAddressSet())
   .intersect(currentProgram.getMemory().getExecuteSet());
  // Direct Disassembler does not schedule AutoAnalysisManager code analysis.
  decoder=Disassembler.getDisassembler(currentProgram,monitor,null);
  for(Address seed:seeds)recover(seed);
  println("RETURN RECOVERY COMPLETE");
 }

 private void recover(Address seed) throws Exception {
  Function function=functions.getFunctionAt(seed);
  AddressSet body=new AddressSet();
  Set<Address> visited=new HashSet<>();
  Set<Address> returns=new LinkedHashSet<>();
  Address[] dressingSwitchTargets=null;
  ArrayDeque<Address> pending=new ArrayDeque<>();
  enqueue(seed,pending,visited);
  int instructions=0, bytes=0;
  while(!pending.isEmpty()) {
   monitor.checkCancelled();
   Address pc=pending.removeFirst();
   requireUnownedOrSame(new AddressSet(pc,pc),seed);
   if(!executable.contains(pc))throw failure(pc,"not loaded initialized executable memory");
   Instruction instruction=listing.getInstructionAt(pc);
   if(instruction==null) {
    if(listing.getInstructionContaining(pc)!=null||listing.getDefinedDataContaining(pc)!=null)
     throw failure(pc,"reachable start overlaps existing instruction or defined data");
    // Singleton restriction admits this complete x86 instruction, not its next start.
    AddressSet added=decoder.disassemble(pc,new AddressSet(pc,pc),false);
    instruction=listing.getInstructionAt(pc);
    if(instruction==null||!added.contains(pc))throw failure(pc,"single-instruction decode failed");
    if(added.getNumAddresses()!=instruction.getLength())
     throw failure(pc,"single-instruction decode exceeded its instruction span");
   }
   if(instruction.getLength()!=instruction.getParsedLength())
    throw failure(pc,"instruction length override prevents exact machine coverage");
   Address end=instruction.getMaxAddress();
   if(!executable.contains(pc,end))throw failure(pc,"instruction crosses nonexecutable or uninitialized memory");
   requireUnownedOrSame(new AddressSet(pc,end),seed);
   if(++instructions>MAX_INSTRUCTIONS||bytes+instruction.getLength()>MAX_BYTES)
    throw failure(pc,"instruction or byte safety ceiling exceeded");
   bytes+=instruction.getLength();
   body.add(pc,end);
   repairCleanupCall(instruction);
   FlowType flow=instruction.getFlowType();
   Address fallthrough=instruction.getFallThrough();
   Address[] targets=instruction.getFlows();
   if(flow.isCall()&&allNoReturn(targets)) {
    println("terminal no-return call "+seed+" "+pc+" targets="+java.util.Arrays.toString(targets));
    continue;
   }
   if(flow.isJump()) {
    Address[] resolved=resolveDressingSwitch(seed,instruction);
    if(resolved!=null)targets=dressingSwitchTargets=resolved;
    if(targets==null||targets.length==0)throw failure(pc,"unresolved jump prevents full closure");
    for(Address target:targets)enqueue(target,pending,visited);
   }
   boolean realReturn=false;
   if(flow.isTerminal()&&!flow.isCall()&&!flow.isJump()&&
      (instruction.getMnemonicString().equalsIgnoreCase("RET")||
       instruction.getMnemonicString().equalsIgnoreCase("RETF"))) {
    for(PcodeOp op:instruction.getPcode(false)) {
     if(op.getOpcode()==PcodeOp.RETURN)realReturn=true;
    }
   }
   if(realReturn) {
    returns.add(pc);
    println("discovered RET "+seed+" "+pc);
   }
   if(fallthrough!=null)enqueue(fallthrough,pending,visited);
   else if(!flow.isJump()&&!realReturn) {
    if(!flow.isTerminal())throw failure(pc,"missing fallthrough prevents full closure");
    println("terminal instruction "+seed+" "+pc+" "+instruction);
   }
   else if(flow.isJump()&&flow.hasFallthrough()&&fallthrough==null)
    throw failure(pc,"conditional jump is missing its fallthrough");
  }
  if(returns.isEmpty())throw failure(seed,"closure has zero real RET instructions");
  if(body.getNumAddresses()!=bytes)throw failure(seed,"overlapping instruction spans");
  requireUnownedOrSame(body,seed);
  if(function==null) {
   function=functions.createFunction(null,seed,body,SourceType.ANALYSIS);
   if(function==null)throw failure(seed,"exact-body function creation failed");
  }
  else function.setBody(body);
  function.setNoReturn(false);
  if(dressingSwitchTargets!=null) {
   Address site=toAddr("00440006");
   for(Address target:dressingSwitchTargets) {
    Reference reference=currentProgram.getReferenceManager().getReference(site,target,0);
    if(reference==null||!reference.getReferenceType().isJump())
     currentProgram.getReferenceManager().addMemoryReference(site,target,RefType.COMPUTED_JUMP,SourceType.ANALYSIS,0);
   }
  }
  println("recovered body "+seed+" instructions="+instructions+" instructionBytes="+bytes+
   " bodyBytes="+body.getNumAddresses()+" RETs="+returns);
 }

 private Address[] resolveDressingSwitch(Address seed,Instruction jump) throws Exception {
  if(!seed.equals(toAddr("0043fe40"))||!jump.getAddress().equals(toAddr("00440006")))return null;
  Instruction add=requireSwitchInstruction("0043fff7","ADD");
  Instruction cmp=requireSwitchInstruction("0043fffa","CMP");
  Instruction guard=requireSwitchInstruction("0043fffd","JA");
  Instruction map=requireSwitchInstruction("0043ffff","MOVZX");
  requireSwitchInstruction("00440006","JMP");
  if(!isRegister(add,0,"EAX")||!hasScalar(add,1,-2)||
     !isRegister(cmp,0,"EAX")||!hasScalar(cmp,1,10)||
     !isRegister(map,0,"EDX")||!hasRegister(map,1,"EAX")||!hasScalar(map,1,0x4400fc)||
     !hasRegister(jump,0,"EDX")||!hasScalar(jump,0,4)||!hasScalar(jump,0,0x4400f4)||
     !map.getDefaultOperandRepresentation(1).replace(" ","").equals("byteptr[EAX+0x4400fc]")||
     !jump.getDefaultOperandRepresentation(0).replace(" ","").equals("dwordptr[EDX*0x4+0x4400f4]"))
   throw failure(jump.getAddress(),"dressing switch operand pattern changed");
  Address[] defaultEdge=guard.getPrototype().getFlows(guard.getInstructionContext());
  if(!guard.getFlowType().isConditional()||defaultEdge==null||defaultEdge.length!=1||
     !defaultEdge[0].equals(toAddr("00440016"))||
     !toAddr("0043fffa").equals(add.getFallThrough())||
     !toAddr("0043fffd").equals(cmp.getFallThrough())||
     !toAddr("0043ffff").equals(guard.getFallThrough())||
     !jump.getAddress().equals(map.getFallThrough())||
     !jump.getFlowType().isJump()||!jump.getFlowType().isComputed()||jump.getFallThrough()!=null)
   throw failure(jump.getAddress(),"dressing switch guard or machine flow changed");
  Address mapAddress=toAddr("004400fc"), tableAddress=toAddr("004400f4");
  byte[] indices=new byte[11]; // CMP EAX,10 plus unsigned JA proves eleven possible indices.
  if(!currentProgram.getMemory().getLoadedAndInitializedAddressSet().contains(mapAddress,mapAddress.add(10))||
     currentProgram.getMemory().getBytes(mapAddress,indices)!=indices.length)
   throw failure(mapAddress,"dressing switch byte map unavailable");
  Set<Address> targets=new LinkedHashSet<>();
  for(byte value:indices) {
   monitor.checkCancelled();
   Address slot=tableAddress.add(4L*Byte.toUnsignedInt(value));
   if(!currentProgram.getMemory().getLoadedAndInitializedAddressSet().contains(slot,slot.add(3)))
    throw failure(slot,"dressing switch target DWORD unavailable");
   Address target=toAddr(Integer.toUnsignedLong(currentProgram.getMemory().getInt(slot,false)));
   if(!executable.contains(target))throw failure(target,"dressing switch target is not executable");
   requireUnownedOrSame(new AddressSet(target,target),seed);
   targets.add(target);
  }
  println("resolved dressing switch "+jump.getAddress()+" byteMap="+java.util.Arrays.toString(indices)+" targets="+targets);
  return targets.toArray(new Address[0]);
 }

 private Instruction requireSwitchInstruction(String address,String mnemonic) {
  Instruction instruction=listing.getInstructionAt(toAddr(address));
  if(instruction==null||!instruction.getMnemonicString().equals(mnemonic)||
     instruction.getFlowOverride()!=FlowOverride.NONE||
     instruction.getLength()!=instruction.getParsedLength())
   throw failure(toAddr(address),"dressing switch instruction pattern changed");
  return instruction;
 }

 private boolean isRegister(Instruction instruction,int operand,String name) {
  Register register=instruction.getRegister(operand);
  return register!=null&&register.getName().equals(name);
 }

 private boolean hasRegister(Instruction instruction,int operand,String name) {
  for(Object object:instruction.getOpObjects(operand))
   if(object instanceof Register&&((Register)object).getName().equals(name))return true;
  return false;
 }

 private boolean hasScalar(Instruction instruction,int operand,long value) {
  for(Object object:instruction.getOpObjects(operand))
   if(object instanceof Scalar&&((Scalar)object).getSignedValue()==value)return true;
  return false;
 }

 private void repairCleanupCall(Instruction instruction) throws Exception {
  if(!instruction.getPrototype().getFlowType(instruction.getInstructionContext()).isCall())return;
  Address[] targets=instruction.getPrototype().getFlows(instruction.getInstructionContext());
  if(targets==null||targets.length==0)targets=instruction.getFlows();
  if(targets==null||targets.length==0)return;
  for(Address target:targets) {
   Function callee=resolvedFunction(target);
   if(!returningCleanup.contains(target)&&
      (callee==null||!returningCleanup.contains(callee.getEntryPoint())))return;
  }
  if(instruction.getFlowOverride()!=FlowOverride.NONE) {
   println("cleared cleanup CALL flow override "+instruction.getAddress()+" "+instruction.getFlowOverride());
   instruction.setFlowOverride(FlowOverride.NONE);
  }
  if(instruction.isFallThroughOverridden()&&instruction.getFallThrough()==null) {
   instruction.clearFallThroughOverride();
   println("cleared cleanup CALL null fallthrough "+instruction.getAddress());
  }
 }

 private boolean allNoReturn(Address[] targets) {
  if(targets==null||targets.length==0)return false;
  for(Address target:targets) {
   Function callee=resolvedFunction(target);
   if(callee==null||!callee.hasNoReturn())return false;
  }
  return true;
 }

 private Function resolvedFunction(Address target) {
  if(target==null||target.equals(Address.NO_ADDRESS))return null;
  Function callee=functions.getFunctionAt(target);
  return callee!=null&&callee.isThunk()?callee.getThunkedFunction(true):callee;
 }

 private void requireUnownedOrSame(AddressSet span,Address seed) {
  Iterator<Function> overlaps=functions.getFunctionsOverlapping(span);
  while(overlaps.hasNext()) {
   Function owner=overlaps.next();
   if(!owner.getEntryPoint().equals(seed))
    throw failure(span.getMinAddress(),"overlaps function "+owner.getEntryPoint());
  }
 }

 private void enqueue(Address target,ArrayDeque<Address> pending,Set<Address> visited) {
  if(target==null||target.equals(Address.NO_ADDRESS))throw failure(target,"unresolved flow target");
  if(visited.add(target)) {
   if(visited.size()>MAX_INSTRUCTIONS)throw failure(target,"instruction safety ceiling exceeded");
   pending.addLast(target);
  }
 }

 private IllegalStateException failure(Address address,String reason) {
  return new IllegalStateException("Return recovery failed at "+address+": "+reason);
 }
}
