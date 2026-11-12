# Agent skills

Project skills for Claude Code. Claude loads one when the task matches its `description`; you can
also run one as `/<name>`. They hold the judgment calls and self-review gates for writing C/C++ here.

How the agent docs fit together:

- `.skills/SKILL.md` is the main guide: boards, constraints, layout, conventions, build/test/check,
  git and PR rules. `CLAUDE.md` imports it (`@.skills/SKILL.md`), so Claude Code always has it loaded,
  and `.github/skills/crosspoint-reader.md` is a symlink to it for other agents. Keep it compact,
  because every session pays for it.
- The skills below load on demand and add the decision procedures the main guide does not carry. Do
  not restate the main guide in them; refer to it.
- `docs/` holds the reference detail. Both of the above point into it rather than copying it.

| Skill | Loads when you are... |
|---|---|
| `heap-discipline` | allocating memory: new/malloc/vector/string, buffers, caches, tables that grow with a book |
| `hal-and-abstractions` | touching storage, files, input, display, board differences, settings, i18n, rendering |
| `control-flow-clarity` | writing branching logic, state flags, modes, if/else ladders |
| `scope-discipline` | adding a feature, activity, lib, setting, or dependency |
| `refactor-for-review` | refactoring, cleaning up, or preparing a change for PR |

Each skill ends with a self-review checklist. When reviewing a PR, the checklists double as a rubric.

## Maintaining these

- Anchor on durable names (files, types, functions, macros), not line numbers, which drift.
- When code changes a rule stated here, change the rule in the same PR. A stale rule is worse than
  none: agents follow it.
- State only what the code, a test or a device log shows.
- Trigger quality lives in the `description` field: name the situations that should pull the skill
  in, in the words a task would use.
