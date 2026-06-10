# Config 配置系统当前实现计划

> 更新: 2026-06-10

## 1. 当前架构事实

配置系统已收口为“代码默认值 + Debug/上位机会话内动态调整”。Service 启动不读取配置文件，App 不再提供本地文件 fallback，安装包不再携带外部配置资产。

| 组件 | 当前职责 | 代码依据 |
| --- | --- | --- |
| `ConfigStore` | 内存 key/value store，不含文件读写 | [ConfigStore.h:14-35](../Common/include/config/ConfigStore.h#L14-L35) |
| `ConfigBinder` | 注册代码默认值与 schema metadata | [ConfigRuntime.cpp:111-128](../EGoTouchService/source/ConfigRuntime.cpp#L111-L128) |
| `ConfigRuntime` | 初始化默认值、生成 catalog/snapshot、应用 session patch、拒绝 persist | [ConfigRuntime.cpp:361-434](../EGoTouchService/source/ConfigRuntime.cpp#L361-L434) |
| App `ConfigDraft` | connected mode 下编辑 Service 当前会话配置 | [ServiceProxy.Config.cpp:592-902](../Tools/EGoTouchApp/source/ServiceProxy.Config.cpp#L592-L902) |

## 2. IPC 当前契约

| Command | 值 | 当前行为 |
| --- | ---: | --- |
| `ReloadConfig` - `ApplyConfigTlvChunk` | `40`-`45` | legacy tombstone，返回 `UnsupportedCommand` |
| `GetConfigCatalogV3` | `46` | 分页读取 Service v3 catalog |
| `GetConfigSnapshotV3` | `47` | 分页读取 Service v3 snapshot |
| `ApplyConfigPatchV3` | `48` | 修改当前 Service 会话；重启后恢复默认值 |
| `PersistConfigV3` | `49` | 保留 wire 入口，但返回 `UnsupportedCommand`，不写文件 |

## 3. Runtime 证据

`ConfigRuntimeTest` 应覆盖：

- 无配置文件时 `ConfigRuntime::Initialize()` 成功。
- live key patch 会更新当前会话。
- restart-required key 会 staged 到 desired store，但不落盘。
- `PersistConfigV3()` 返回不支持持久化。
- fresh runtime 重新初始化后恢复代码默认值。

## 4. Packaging 规则

- build output 不再复制配置文件。
- `cmake --install` 只安装可执行文件和正常运行依赖。
- WiX 不再声明配置文件 component。
- `PackagingConfigLayoutTest` 已不再作为 gate。

## 5. 验证命令

```powershell
cmake --preset arm64-Release
cmake --build --preset arm64-Release --target ConfigRuntimeTest EGoTouchApp_ServiceProxyCatalogSchemaTest DvrCoreRuntimeConfigRoundTripTest EGoTouchApp_ServiceProxyRuntimeConfigSnapshotTest EGoTouchApp EGoTouchService
ctest --test-dir build/arm64-Release -R "ConfigRuntimeTest|EGoTouchApp\.ServiceProxyCatalogSchemaTest|DvrCoreRuntimeConfigRoundTripTest|EGoTouchApp\.ServiceProxyRuntimeConfigSnapshotTest" --output-on-failure
git diff --check
```

## 小结

- 默认值只来自代码。
- App 动态调整只影响当前 Service 会话。
- 文件配置、文件持久化、文件 fallback、文件打包均不再是产品能力。
