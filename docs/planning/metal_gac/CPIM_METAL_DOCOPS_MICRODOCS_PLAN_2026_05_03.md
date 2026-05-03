---
status: active
updated: 2026-05-03T07:07:57Z
type: plan
topic: cpim-metal
slug: docops-microdocs
stage: s07
---

# DocOps Logic Small Plan And Changelog Feature Plan

## Goal

- 给 DocOps Logic 增加通用 microdoc 能力：每个小计划、每个小 changelog 都能
  通过命令生成独立 Markdown 文档。
- 让该能力同时存在于本仓库的插件源码和当前已安装的 local plugin cache。

## Scope

- 新增 `dol doc new --kind plan|changelog`。
- 新增小计划 / 小 changelog 模板。
- 新增 `docops-doc` skill，明确何时使用该能力。
- 更新插件默认提示与 AGENTS 模板规则。
- 不实现复杂 lint 强制规则；本轮先提供生成能力和工作流约束。

## Tasks

- CLI：
  - 添加 `doc new` 子命令。
  - 支持 `--topic`、`--slug`、`--title`、`--dir`、`--date`、`--force`。
  - 默认输出到 `docs/planning/<topic>/`，项目可用 `--dir` 指定目录。
- 模板：
  - `templates/small-plan.md`
  - `templates/small-changelog.md`
- Skill：
  - `skills/doc/SKILL.md`
- 同步：
  - `codex-docops-logic/`
  - `/Users/lee/.codex/plugins/cache/local/docops-logic/0.1.0/`

## Validation

- `python3 -m py_compile codex-docops-logic/scripts/dol.py /Users/lee/.codex/plugins/cache/local/docops-logic/0.1.0/scripts/dol.py`
- `python3 codex-docops-logic/scripts/dol.py doc new --help`
- `python3 /Users/lee/.codex/plugins/cache/local/docops-logic/0.1.0/scripts/dol.py doc new --help`
- 用新命令生成本计划与对应 changelog。
- `python3 codex-docops-logic/scripts/dol.py lint --soft`

## Rollback

- 删除 `doc new` 子命令、模板和 `docops-doc` skill。
- 保留已经生成的项目文档；若不再需要，可由项目显式清理。

## Links

- changelog:
  [DocOps Logic Small Plan And Changelog Feature Changelog](CPIM_METAL_DOCOPS_MICRODOCS_CHANGELOG_2026_05_03.md)
- roadmap:
  [Metal GAC Changelog Index](../METAL_GAC_CHANGELOG.md)
