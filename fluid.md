Moving past boxes and pipelines means treating code not as dead text files sitting on a hard drive, but as a single living, running organism that reshapes itself while it runs.
The diagram looked archaic because almost all modern software engineering is built on a 1970s assembly-line habit: write dead text → run it through a pipeline → pack it in a box → restart the machine. True "splicing" throws out the assembly line completely.

Static Pipelines vs. Fluid Synthesis


Feature	The "Boxes & Pipelines" World (Archaic)	The Fluid, Living System (Splicing)
What Code Is	Dead text files organized in folders.	Live, active nodes floating in shared memory.
How Changes Happen	Stop, edit files, rebuild, test, redeploy.	Hot-swap and rewire behavior mid-flight without stopping.
Where the AI Lives	Outside the app, poking it with clunky tools.	Inside the running memory, feeling traffic and errors directly.
How Pieces Connect	Hardcoded wires and rigid API gates.	Loose capabilities that snap together based on need.


How Fluid Software Actually Works (Without Sci-Fi)

Surprisingly, computer science actually invented pieces of this decades ago (in systems like Smalltalk, Lisp machines, and Erlang), then abandoned them for rigid files and containers. Bringing that fluidity back with AI looks like four real mechanisms:
• Image-Based Living Memory (No More Files). Instead of reading and writing text files, the program lives as a continuous, running state—like a waking brain. When an agent changes a function, it updates the live object directly in memory. There is no "build" or "deploy" step. The system simply is the new version immediately.
• Hot-Swapping Cells (Like Erlang/BEAM). Telecom systems run for decades without ever restarting by running millions of tiny, isolated micro-processes. If one piece breaks or needs an upgrade, a new cell spins up alongside it, traffic flows seamlessly into the new path, and the old cell dissolves.
• Intent-Driven Routing (No Fixed Wires). Rigid software hardcodes exact connections ("Box A calls Box B"). Fluid software broadcasts needs ("I need this user receipt summarized and stored"). Whatever module or agent capability is healthiest and fastest at that moment catches the signal and fulfills it.
• Homeostasis Instead of "Testing Gates." Your own body doesn't pause, clone itself into a sandbox, run a checklist, and merge back together when you get a paper cut. White blood cells sense local friction and adapt on the spot. A fluid codebase monitors its own pulse—latency spikes, type mismatches, weird data shapes—and synthesizes small adapter patches around friction points automatically.

What the Agent Becomes

In this model, the coding agent stops being an external mechanic swinging wrenches at a parked car. It becomes more like an autonomic nervous system inside the runtime:
1. Sensing. It feels live data flowing through functions and notices where data gets bottlenecked or malformed.
2. Synthesizing. If a new data format arrives that the app doesn't recognize, it weaves a tiny translator function on the fly in memory so the flow doesn't crash.
3. Pruning. When temporary pathways or old features stop getting traffic, they atrophy and get cleaned up instead of rotting as legacy code.

The Real-World Friction Point

The only reason we still use rigid boxes today is trust and predictability. When software flows and mutates in real time, humans lose the ability to read a clean "before and after" receipt, which scares engineers running banks or hospitals. To make fluid software practical, the boundary shifts from rigid pipelines to strict laws of physics (like mathematical type guarantees or resource caps) that let everything inside the boundary flow and mutate freely without ever breaking the core rules.
Does this living runtime / nervous system angle hit closer to what you're picturing? If you want to explore how to prototype something like this today, we could look at actor-model runtimes (like Elixir/BEAM), WebAssembly hot-swapping, or durable live-state engines—which direction sparks your interest?
