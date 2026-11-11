# wubulens

**What is using my GPU right now, and why?**

A plain-language NVIDIA GPU lens for Linux, written from scratch in C11.

It reads the card through NVML, attributes GPU time and video memory to the
programs actually running, groups those programs into the apps you recognise
(a game, your browser, a language-model server, the desktop), and writes a
sentence about the situation — numbers computed from real readings, never
guessed.

## Provenance

`wubulens` is a **clean-room reimplementation**: it was written from the
behaviour and the public NVML interface of the Rust tool `gpulens`
(© guitaripod, GPL-3.0), with **no source code copied, linked, or derived**.
Every algorithm here — the grouping rules, the classification, the plain
wording, the JSON shape — is our own ground-up design. The name is ours; the
license is the WaefreBeorn Umbrella License v3.0.

## What it does

- **A story line.** "The GPU is busy (62%). vLLM is doing most of the work
  (56%), with KWin on the side." Always computed from real readings.
- **Per-app GPU time and memory**, grouped from the driver's per-process
  figures; the unclaimed remainder is named, not hidden.
- **Idle-but-hogging** callouts: apps doing nothing yet holding memory, with
  how long they have been idle.
- **Hardware at a glance:** power against limit, temperature, clocks, PCIe
  traffic, encoder and decoder load.
- **Scriptable:** `wubulens snapshot --json` prints the same picture for
  scripts and agents, with a stable schema number.
- **Works without a GPU:** `wubulens --demo` runs against a scripted card, so
  the whole pipeline is testable on any machine.

## Build

```
make            # build src/wubulens
make test       # the gate (runs on any machine, no GPU needed)
```

Requires a C11 compiler and, at run time, the NVIDIA driver's
`libnvidia-ml.so.1` (loaded dynamically; the tool says so when it is absent).

## Use

```
wubulens            # the report
wubulens --demo     # a scripted fake GPU
wubulens snapshot --json
```

## Layout

| File | Concern |
|------|---------|
| `src/gpu.c` | the NVML backend (dlopen) and the fake backend, one interface |
| `src/proc.c` | what a pid is: executable, command line, owner |
| `src/classify.c` | grouping pids into apps, and naming what an app is |
| `src/narrate.c` | the plain-language headline and findings |
| `src/snapshot.c` | the text and JSON renderers |
| `src/main.c` | the command line |

C11, opaque structs, minimal includes, one concern per file.
