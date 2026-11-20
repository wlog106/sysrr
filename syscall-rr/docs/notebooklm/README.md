# sysrr source pack for NotebookLM

These files explain sysrr in a form that NotebookLM (and people) can digest:
one topic per file, a short summary at the top of each, a clean heading
hierarchy (it becomes the branches of a mind map), the same concept names in
every file, a "How it connects" line under each concept, real numbers only,
and a question-and-answer section at the end of each file.

The other Markdown files of the project (`README.md`, `DESIGN.md`,
`RECORD.md`, `REPLAY.md`) are working notes: complete, but full of dates,
commit hashes and dead ends. Use this pack for learning and visualizing, and
those for detail.

## The files

| File | What it covers |
|---|---|
| `01-overview.md` | The problem, the four-step workflow, the tools, what replay and divergence mean, a worked example |
| `02-recording.md` | The eBPF recorder: what one syscall record holds, follow mode, generations, the log format, overhead |
| `03-checkpoint-restore-attach.md` | Checkpoint with CRIU, restore left stopped, how the replayer injects its seccomp filter, checkpoint signals |
| `04-replay-engine.md` | How each syscall is answered: faked from the log or run for real, the checks, fds, memory and pid pinning |
| `05-threads-and-signals.md` | Serial mode, stretches, the order search; which signals are replayed and where they are delivered |
| `06-results-and-limits.md` | The 30-test suite, the real servers, the comparison with Gemini's crr, every known limit |
| `07-glossary.md` | Every term, with links between related terms |

## How to use it in NotebookLM

1. Create a notebook and add all seven files as sources (upload the `.md`
   files, or paste each one as text). Leave this README out, or add it too:
   it is only an index.
2. Things worth generating:
   - **Mind map**: the headings and "How it connects" lines give it a clear
     tree: workflow -> recorder / checkpoint / replay -> threads / signals.
   - **Audio or video overview**: ask it to focus on one journey, for example
     "follow one syscall from the moment the program makes it on machine A to
     the moment the restored copy gets the recorded answer on machine B".
   - **Study guide / FAQ**: the "Common questions" sections seed it.
   - **Briefing doc**: "what sysrr can and cannot replay, with the evidence".
3. Good questions to ask the notebook:
   - Why does sysrr record with eBPF but replay with seccomp, not ptrace?
   - What happens, step by step, between `podman container restore` and the
     first replayed syscall?
   - What is a divergence, and how is it reported?
   - Why do threads need serial mode, and what does the order search look for?
   - Which signals are replayed exactly, which approximately, and why?
   - How does sysrr differ from Gemini's crr, and what do the numbers show?

## Keeping it current

When the tool changes, update the file for that part (and the glossary if a
term changes). Keep the template: summary, key ideas, one section per concept
with a definition first and a "How it connects" line, real numbers, common
questions at the end.
