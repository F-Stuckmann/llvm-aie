# VirtRegMap Serialization for AIE2p Register Allocation Testing

## Goal

Enable serialization/deserialization of VirtRegMap in MIR format to:
1. Stop at any point in the RA pipeline and save state
2. Resume compilation from that point
3. Test each RA stage independently with FileCheck

---

## Background

### Problem Statement

The AIE2p register allocation pipeline runs multiple stages:

```
1. RegisterCoalescer (optional)
2. aie-split-instrs-create
3. greedy (for M registers - optional)
4. greedy (for 3D registers)         <- First greedy run
5. aie-superreg-rewrite              <- First rewrite
6. greedy (for 2D+3D registers)      <- Second greedy run
7. aie-superreg-rewrite              <- Second rewrite
8. aie-unallocated-superreg-rewrite (optional)
9. greedy (final - all registers)    <- Third greedy run
10. aie-waw-reg-rewrite (optional)
11. greedy (final after WAW)         <- Fourth greedy run (optional)
12. virtregrewriter
```

Currently, when stopping between passes with `-stop-after`, the VirtRegMap (which contains vreg→physreg assignments) is **not serialized** into the MIR output. This makes it impossible to:
- Test intermediate allocation states
- Resume compilation from a saved intermediate state
- Verify allocation decisions at each stage

### Solution: Option A - YAML Serialization

Extend the MIR YAML format to include VirtRegMap assignments in the `registers:` section:

```yaml
registers:
  - { id: 0, class: eds, assigned-register: '$ds0' }
  - { id: 1, class: ed, assigned-register: '$dn0' }
  - { id: 2, class: ep, stack-slot: 0 }  # spilled
```

This approach:
- Has no redundancy (mapping stored once in YAML)
- Uses existing YAML infrastructure
- Integrates with test update scripts (with minor extension)
- Enables true serialization/deserialization round-trips

---

## Implementation Plan

### Phase 1: MIR Format Extension

**File: `llvm/include/llvm/CodeGen/MIRYamlMapping.h`**

1. Extend `VirtualRegisterDefinition` struct:

```cpp
struct VirtualRegisterDefinition {
  UnsignedValue ID;
  StringValue Class;
  StringValue PreferredRegister;
  StringValue AssignedRegister;      // NEW: physreg from VirtRegMap
  std::optional<int> StackSlot;      // NEW: spill slot if spilled
  std::vector<FlowStringValue> RegisterFlags;

  bool operator==(const VirtualRegisterDefinition &Other) const {
    return ID == Other.ID && Class == Other.Class &&
           PreferredRegister == Other.PreferredRegister &&
           AssignedRegister == Other.AssignedRegister &&
           StackSlot == Other.StackSlot &&
           RegisterFlags == Other.RegisterFlags;
  }
};
```

2. Update `MappingTraits<VirtualRegisterDefinition>`:

```cpp
template <> struct MappingTraits<VirtualRegisterDefinition> {
  static void mapping(IO &YamlIO, VirtualRegisterDefinition &Reg) {
    YamlIO.mapRequired("id", Reg.ID);
    YamlIO.mapRequired("class", Reg.Class);
    YamlIO.mapOptional("preferred-register", Reg.PreferredRegister,
                       StringValue());
    YamlIO.mapOptional("assigned-register", Reg.AssignedRegister,
                       StringValue());  // NEW
    YamlIO.mapOptional("stack-slot", Reg.StackSlot,
                       std::optional<int>());  // NEW
    YamlIO.mapOptional("flags", Reg.RegisterFlags,
                       std::vector<FlowStringValue>());
  }
};
```

---

### Phase 2: MIR Printer (Serialization)

**File: `llvm/lib/CodeGen/MIRPrinter.cpp`**

1. Add VirtRegMap parameter to relevant functions:

```cpp
void MIRPrinter::convert(yaml::MachineFunction &YamlMF,
                         const MachineFunction &MF,
                         const VirtRegMap *VRM = nullptr);
```

2. Serialize VirtRegMap assignments:

```cpp
void MIRPrinter::convert(yaml::MachineFunction &YamlMF,
                         const MachineRegisterInfo &RegInfo,
                         const VirtRegMap *VRM) {
  for (unsigned I = 0, E = RegInfo.getNumVirtRegs(); I < E; ++I) {
    Register Reg = Register::index2VirtReg(I);
    yaml::VirtualRegisterDefinition VReg;
    VReg.ID = I;
    VReg.Class = StringValue(TRI->getRegClassName(RegInfo.getRegClass(Reg)));

    // Serialize VirtRegMap assignment if available
    if (VRM && VRM->hasPhys(Reg)) {
      MCRegister PhysReg = VRM->getPhys(Reg);
      VReg.AssignedRegister = StringValue(TRI->getName(PhysReg));
    }

    // Serialize spill slot if spilled
    if (VRM) {
      int SS = VRM->getStackSlot(Reg);
      if (SS != VirtRegMap::NO_STACK_SLOT) {
        VReg.StackSlot = SS;
      }
    }

    YamlMF.VirtualRegisters.push_back(VReg);
  }
}
```

**File: `llvm/lib/CodeGen/MIRPrintingPass.cpp`**

3. Query VirtRegMap if available:

```cpp
bool MIRPrintingPass::runOnMachineFunction(MachineFunction &MF) {
  // Get VirtRegMap if available (after RA passes)
  const VirtRegMap *VRM = nullptr;
  if (auto *VRMW = getAnalysisIfAvailable<VirtRegMapWrapperLegacy>())
    VRM = &VRMW->getVRM();

  printMIR(OS, MF, VRM);
  return false;
}

void MIRPrintingPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addUsedIfAvailable<VirtRegMapWrapperLegacy>();  // NEW
  MachineFunctionPass::getAnalysisUsage(AU);
}
```

---

### Phase 3: MIR Parser (Deserialization)

**File: `llvm/lib/CodeGen/MIRParser/MIRParser.cpp`**

1. Parse and restore VirtRegMap assignments:

```cpp
bool MIRParserImpl::initializeRegisterInfo(
    PerFunctionMIParsingState &PFS,
    const yaml::MachineFunction &YamlMF) {

  MachineFunction &MF = PFS.MF;
  MachineRegisterInfo &MRI = MF.getRegInfo();

  // Get or create VirtRegMap
  VirtRegMap *VRM = nullptr;
  // ... obtain VRM ...

  for (const auto &VReg : YamlMF.VirtualRegisters) {
    Register Reg = Register::index2VirtReg(VReg.ID.Value);

    // Restore physical register assignment
    if (!VReg.AssignedRegister.Value.empty() && VRM) {
      MCRegister PhysReg;
      if (parseNamedRegisterReference(PFS, PhysReg,
                                      VReg.AssignedRegister.Value, Error))
        return error(Error, VReg.AssignedRegister.SourceRange);

      VRM->assignVirt2Phys(Reg, PhysReg);
    }

    // Restore spill slot
    if (VReg.StackSlot.has_value() && VRM) {
      VRM->assignVirt2StackSlot(Reg, *VReg.StackSlot);
    }
  }

  return false;
}
```

2. Ensure VirtRegMap analysis is preserved for subsequent passes.

---

### Phase 4: Test Script Update

**File: `llvm/utils/update_mir_test_checks.py`**

1. Add command-line flag:

```python
parser.add_argument(
    "--print-registers",
    action="store_true",
    help="Add check lines for virtual register assignments",
)
```

2. Extend regex to capture registers section:

```python
MIR_FUNC_RE = re.compile(
    r"^---$"
    r"\n"
    r"^ *name: *(?P<func>[A-Za-z0-9_.-]+)$"
    r".*?"
    r"(?:^ *registers: *\n"              # NEW
    r"(?P<registers>.*?)"                 # NEW
    r"(?=^ *[a-z]+:))?.*?"               # NEW
    r"(?:^ *fixedStack: *(\[\])? *\n"
    r"(?P<fixedStack>.*?)\n?"
    r"^ *stack:"
    r".*?)?"
    r"^ *body: *\|\n"
    r"(?P<body>.*?)\n"
    r"^\.\.\.$",
    flags=(re.M | re.S),
)
```

3. Extend `FunctionInfo` class:

```python
class FunctionInfo:
    def __init__(self, body, fixedStack, registers):
        self.body = body
        self.fixedStack = fixedStack
        self.registers = registers

    def __eq__(self, other):
        if not isinstance(other, FunctionInfo):
            return False
        return (self.body == other.body and
                self.fixedStack == other.fixedStack and
                self.registers == other.registers)
```

4. Generate CHECK lines for registers:

```python
if args.print_registers and func_info.registers:
    output_lines.append("{}: registers:".format(check))
    for reg_line in func_info.registers.splitlines():
        if reg_line.strip():
            filecheck_directive = check + "-NEXT"
            output_lines.append("{}: {}".format(filecheck_directive,
                                                reg_line.rstrip()))
```

---

### Phase 5: AIE-Specific Integration (Optional)

**File: `llvm/lib/Target/AIE/aie2p/AIE2PTargetMachine.cpp`**

Create named wrapper passes for each greedy stage to enable precise stopping points:

```cpp
namespace {

class AIE3DGreedyAllocator : public MachineFunctionPass {
public:
  static char ID;
  AIE3DGreedyAllocator() : MachineFunctionPass(ID) {}

  StringRef getPassName() const override {
    return "AIE Greedy Register Allocator (3D)";
  }

  bool runOnMachineFunction(MachineFunction &MF) override {
    // Delegate to greedy with 3D filter
    return runGreedyWithFilter(MF, onlyAllocate3DRegisters);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Same as RAGreedy
  }
};

class AIE2D3DGreedyAllocator : public MachineFunctionPass {
  // Similar for 2D+3D allocation
};

class AIEFinalGreedyAllocator : public MachineFunctionPass {
  // Similar for final allocation
};

} // end anonymous namespace

char AIE3DGreedyAllocator::ID = 0;
char AIE2D3DGreedyAllocator::ID = 0;
char AIEFinalGreedyAllocator::ID = 0;

INITIALIZE_PASS(AIE3DGreedyAllocator, "aie-greedy-3d",
                "AIE Greedy RA for 3D registers", false, false)
INITIALIZE_PASS(AIE2D3DGreedyAllocator, "aie-greedy-2d3d",
                "AIE Greedy RA for 2D+3D registers", false, false)
INITIALIZE_PASS(AIEFinalGreedyAllocator, "aie-greedy-final",
                "AIE Final Greedy RA", false, false)
```

Update pipeline to use named passes:

```cpp
bool AIE2PPassConfig::addRegAssignAndRewriteOptimized() {
  // ...
  if (EnableStagedRA) {
    addPass(createAIE3DGreedyAllocator());      // Instead of createGreedyRegisterAllocator(onlyAllocate3DRegisters)
    addPass(createAIESuperRegRewriter());
    addPass(createAIE2D3DGreedyAllocator());    // Instead of createGreedyRegisterAllocator(onlyAllocate3D2DRegisters)
    addPass(createAIESuperRegRewriter());
    // ...
  }
  addPass(createAIEFinalGreedyAllocator());     // Instead of createGreedyRegisterAllocator()
  // ...
}
```

---

### Phase 6: Tests

**Directory: `llvm/test/CodeGen/AIE/aie2p/ra/`**

Create comprehensive tests:

**Test 1: Serialization verification (`serialize-vrm.mir`)**

```mir
# RUN: llc -mtriple=aie2p --aie-staged-ra -stop-after=aie-superreg-rewrite \
#      %s -o - | FileCheck %s --check-prefix=STAGE1

# STAGE1: registers:
# STAGE1-NEXT: - { id: 0, class: eds, assigned-register: '$ds0' }
# STAGE1-NEXT: - { id: 1, class: ed, assigned-register: '$dn0' }

---
name: test_serialize
tracksRegLiveness: true
body: |
  bb.0:
    liveins: $p0
    %0:eds = COPY $p0
    ; ...
```

**Test 2: Round-trip verification (`roundtrip-vrm.mir`)**

```mir
# RUN: llc -mtriple=aie2p --aie-staged-ra -stop-after=aie-superreg-rewrite \
#      %s -o %t.stage1.mir
# RUN: llc -mtriple=aie2p -start-after=aie-superreg-rewrite \
#      %t.stage1.mir -o - | FileCheck %s --check-prefix=FINAL

# Verify assignments are preserved through round-trip
# FINAL: $ds0 =
```

**Test 3: Independent stage testing (`staged-ra-independent.mir`)**

```mir
# Test each stage independently by providing pre-allocated MIR

# RUN: llc -mtriple=aie2p -run-pass=aie-superreg-rewrite %s -o - \
#      | FileCheck %s

---
name: test_superreg_rewrite
registers:
  - { id: 0, class: eds, assigned-register: '$ds0' }
  - { id: 1, class: ed, assigned-register: '$dn0' }
tracksRegLiveness: true
body: |
  bb.0:
    %0:eds = ...
    %1:ed = ...
```

---

## Implementation Order

| Step | Component | Effort | Dependencies |
|------|-----------|--------|--------------|
| 1 | MIRYamlMapping.h | Small (~20 lines) | None |
| 2 | MIRPrinter.cpp | Medium (~50 lines) | Step 1 |
| 3 | MIRPrintingPass.cpp | Small (~15 lines) | Step 2 |
| 4 | MIRParser.cpp | Medium (~60 lines) | Step 1 |
| 5 | update_mir_test_checks.py | Small (~40 lines) | Steps 1-4 |
| 6 | AIE named greedy passes | Medium (~100 lines) | None (optional) |
| 7 | Tests | Small (~100 lines) | Steps 1-5 |

**Total estimated effort:** ~400 lines of code

---

## Expected Workflow After Implementation

```bash
# 1. Stop after 3D allocation, serialize VirtRegMap
llc -mtriple=aie2p --aie-staged-ra -stop-after=aie-superreg-rewrite \
    input.mir -o stage1.mir

# 2. Inspect stage1.mir - see assigned registers in YAML
cat stage1.mir
# ---
# name: test_func
# registers:
#   - { id: 0, class: eds, assigned-register: '$ds0' }
#   - { id: 1, class: ed, assigned-register: '$dn0' }
# body: |
#   bb.0:
#     %0:eds = ...

# 3. Resume from stage1.mir - VirtRegMap is automatically restored
llc -mtriple=aie2p -start-after=aie-superreg-rewrite \
    stage1.mir -o final.s

# 4. Auto-generate test checks including register assignments
utils/update_mir_test_checks.py --print-registers test.mir
```

---

## File Summary

| File | Changes |
|------|---------|
| `llvm/include/llvm/CodeGen/MIRYamlMapping.h` | Add fields to VirtualRegisterDefinition |
| `llvm/lib/CodeGen/MIRPrinter.cpp` | Serialize VirtRegMap to YAML |
| `llvm/lib/CodeGen/MIRPrintingPass.cpp` | Query VirtRegMap analysis |
| `llvm/lib/CodeGen/MIRParser/MIRParser.cpp` | Deserialize VirtRegMap from YAML |
| `llvm/utils/update_mir_test_checks.py` | Add --print-registers flag |
| `llvm/lib/Target/AIE/aie2p/AIE2PTargetMachine.cpp` | Named greedy passes (optional) |
| `llvm/test/CodeGen/AIE/aie2p/ra/*.mir` | New tests |

---

## Benefits

1. **True serialization** - VirtRegMap survives MIR round-trip
2. **No redundancy** - Assignment stored once in YAML header
3. **Native LLVM format** - Uses existing YAML infrastructure
4. **Testable** - FileCheck can verify assignments
5. **Resumable** - Can run from any intermediate pipeline point
6. **Script integration** - Works with update_mir_test_checks.py
