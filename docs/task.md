# Config 重构任务清单

> 当前权威: [implementation_plan.md](implementation_plan.md) 与 [api/config_framework_api.md](api/config_framework_api.md)。
> 原则: Service 默认值只来自代码；Debug/上位机动态调整只影响当前 Service 会话；不再提供文件配置或文件持久化。

状态: `[ ]` 待开始 `[~]` 进行中 `[x]` 已完成 `[-]` 已取消

## Config v3 当前主线

- [x] P0 Catalog/KeyId/TLV/Runtime/App schema 地基
- [x] P1 Service-driven `GetConfigCatalogV3` / `GetConfigSnapshotV3` 分页 IPC
- [x] P2 legacy fixed ABI cleanup：legacy config command `40-45` 仅保留 unsupported tombstone
- [x] P3 文件配置收口：移除配置文件读取、写入、fallback、打包和 drift gate
- [x] P4 App connected mode：保留 session-only `ApplyConfigPatchV3`，不再自动 persist

## 当前 Gate

| Gate | 状态 | 当前事实 | 验收方式 |
| --- | --- | --- | --- |
| Service 默认值 | [x] 已完成 | `ConfigRuntime::Initialize()` 使用代码默认值，不依赖配置目录 | `ConfigRuntimeTest` |
| Session apply | [x] 已完成 | Config v3 patch 修改当前会话 snapshot/state | `ConfigRuntimeTest`, `EGoTouchApp.ServiceProxyCatalogSchemaTest` |
| Persist 禁用 | [x] 已完成 | `PersistConfigV3` 返回 `UnsupportedCommand`，不写文件 | `ConfigRuntimeTest` |
| App fallback | [x] 已完成 | App 未连接 Service 时不再本地应用配置文件 fallback | `EGoTouchApp.ServiceProxyCatalogSchemaTest` |
| Packaging | [x] 已完成 | 不复制、不安装、不打包配置文件 | CMake configure/build |

## 验证命令

```powershell
cmake --preset arm64-Release
cmake --build --preset arm64-Release --target ConfigRuntimeTest EGoTouchApp_ServiceProxyCatalogSchemaTest DvrCoreRuntimeConfigRoundTripTest EGoTouchApp_ServiceProxyRuntimeConfigSnapshotTest EGoTouchApp EGoTouchService
ctest --test-dir build/arm64-Release -R "ConfigRuntimeTest|EGoTouchApp\.ServiceProxyCatalogSchemaTest|DvrCoreRuntimeConfigRoundTripTest|EGoTouchApp\.ServiceProxyRuntimeConfigSnapshotTest" --output-on-failure
git diff --check
```

> 最后更新: 2026-06-10（移除 YAML/文件配置入口与持久化能力）
