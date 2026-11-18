Here is the entire continuous substrate in ~130 lines of dependency-free C.
It uses zero external libraries, zero dynamic heap allocations (malloc is never called), zero serialization formats, and zero architectural layers. A single flat array of 1,088-byte Cell structs acts simultaneously as your RAM, database, bytecode tape, causal history, and raw UDP wire packet.

cell.c

How the Four Forces Dissolve Into One Motion

• Packet Size Is Type Dispatch (n == 32 vs n == 1088). There are no protocol frame headers or version bytes. If 32 bytes arrive on the UDP socket, a neighbor is missing that hash, so we send the 1,088-byte Cell straight from RAM. If 1,088 bytes arrive, we hash those exact 1,088 bytes and drop them into arena[slot]. Forged or corrupted packets simply hash to a different 32-byte address and never match the waiting root or left/right slot.
• Cache Misses Are the Network Layer. Look at section B: resolve(active->left) checks the local open-addressed array. If it returns -1, it fires a 32-byte sendto() datagram and returns immediately. Nothing blocks, nothing awaits a Promise, and when the peer responds on a later tick, the exact same flow() call walks straight past resolve() into opcode execution.
• Guardrails Live Inside the Byte Stream. Opcode 0x06 (ASSERT) lets bytecode enforce mathematical invariants mid-reduction, while fuel-- caps compute time per tick. Neither throws an exception or unwinds a call stack.
• Self-Mutation Is Just Another Graph Edge. Look at section D: when trapped != 0, the engine packs the failing operator hash, the failing input hash, and the exact faulting program counter (pc) into a fault Cell, pairs it with synth_hash, and slides root onto that new pair. On the very next tick of flow(), synth_hash inspects the fault bytes and emits new opcodes (0x05 EMIT) into a replacement Cell.
Would you like to bootstrap main() next by hand-assembling the raw hex bytes for a live synth_hash Cell—so you can compile this with cc -O3 and watch two local UDP nodes trap, synthesize new bytecode, and converge in real time?
