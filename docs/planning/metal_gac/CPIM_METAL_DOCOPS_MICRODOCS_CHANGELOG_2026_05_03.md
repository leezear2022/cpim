---
status: active
updated: 2026-05-03T07:07:57Z
type: changelog
topic: cpim-metal
slug: docops-microdocs
stage: s07
---

# DocOps Logic Small Plan And Changelog Feature Changelog

## Summary

- DocOps Logic 新增生成独立小计划/小 changelog 文档的通用能力。
- 当前项目的 Metal GAC 文档规则同步升级：索引页只做索引，具体小计划和小
  changelog 均单独成文。

## Changes

- `dol.py` 新增 `doc new` 子命令：
  - `--kind plan|changelog`
  - `--topic`
  - `--slug`
  - `--title`
  - `--dir`
  - `--date`
  - `--force`
- 新增模板：
  - `templates/small-plan.md`
  - `templates/small-changelog.md`
- 新增 skill：
  - `skills/doc/SKILL.md`
- `templates/AGENTS.md` 增加规则：每个 small plan / small changelog 必须独立成文。
- `.codex-plugin/plugin.json` 默认提示增加“Create a standalone small plan or changelog”。
- 同步修改：
  - 本仓库 `codex-docops-logic/`
  - 已安装 local plugin cache `/Users/lee/.codex/plugins/cache/local/docops-logic/0.1.0/`

## Validation

- `python3 -m py_compile codex-docops-logic/scripts/dol.py /Users/lee/.codex/plugins/cache/local/docops-logic/0.1.0/scripts/dol.py`
- `python3 codex-docops-logic/scripts/dol.py doc new --help`
- `python3 /Users/lee/.codex/plugins/cache/local/docops-logic/0.1.0/scripts/dol.py doc new --help`
- `python3 codex-docops-logic/scripts/dol.py doc new --kind plan --topic cpim-metal --dir docs/planning/metal_gac --slug docops-microdocs --title "DocOps Logic Small Plan And Changelog Feature Plan" --date 2026-05-03`
- `python3 codex-docops-logic/scripts/dol.py doc new --kind changelog --topic cpim-metal --dir docs/planning/metal_gac --slug docops-microdocs --title "DocOps Logic Small Plan And Changelog Feature Changelog" --date 2026-05-03`

## Decisions

- 生成能力放在 `dol doc new`，不复用 `ch add`，避免“事件日志”和“独立文档”
  的职责混在一起。
- 默认目录使用 `docs/planning/<topic>/`，同时提供 `--dir` 适配项目已有目录规范。
- 聚合页只做索引；多个小计划或小 changelog 不再写进同一个正文文件。

## Follow-Ups

- 后续可给 `dol lint` 增加规则：当 change/plan 文档约定开启时，缺少 standalone
  microdoc 则提示。
- 后续可给 `doc new` 增加自动更新索引页能力。

## Links

- plan:
  [DocOps Logic Small Plan And Changelog Feature Plan](CPIM_METAL_DOCOPS_MICRODOCS_PLAN_2026_05_03.md)
- roadmap:
  [Metal GAC Changelog Index](../METAL_GAC_CHANGELOG.md)
