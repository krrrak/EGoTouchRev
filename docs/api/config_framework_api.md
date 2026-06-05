# Config 框架 API 文档

> EGoTouchRev 配置系统重构目标 API | 基于 [implementation_plan.md](../implementation_plan.md) | 日期: 2026-06-05

---

## 1. 概述

Config 框架提供统一的配置管理基础设施: YAML 文件加载、Schema 校验、类型安全的 C++ 成员绑定、运行时读写、IPC 快照/补丁。

### 1.1 设计原则

- **配置文件是唯一权威数据源** — default.yaml + overrides.yaml
- **零代码生成** — 无 Python 脚本; 每个键一行 C++ 绑定声明
- **启动时校验** — 类型错误/越界/缺键在启动阶段精确报告
- **热路径零开销** — Apply 阶段一次性读取到成员字段, 运行时直接成员访问

### 1.2 命名空间

所有 API 位于 `Config` 命名空间下。

---

## 2. ConfigValue

类型安全的配置值, 替代当前 `void*` + `const_cast`。

```cpp
#include "config/ConfigValue.h"

namespace Config {

using ConfigValue = std::variant<
    bool,
    int32_t,
    float,
    std::string
>;

// ── 类型安全访问 ──

template<typename T>
T getValue(const ConfigValue& v);

template<typename T>
std::optional<T> tryGetValue(const ConfigValue& v);

// ── 序列化 ──

std::string toString(const ConfigValue& v);           // → "true", "42", "3.14", "\"hello\""
ConfigValue fromString(const std::string& s, Type t); // ← 反序列化

// ── 类型枚举 ──

enum class Type : uint8_t {
    Bool,
    Int32,
    Float,
    String,
    Enum,   // 枚举值存储为 string, 通过 mapping 校验
};

Type typeOf(const ConfigValue& v);

} // namespace Config
```

### 使用示例

```cpp
ConfigValue v = 42;                               // 隐式 int32_t
int32_t ival = getValue<int32_t>(v);             // 42
auto opt = tryGetValue<float>(v);                // std::nullopt (类型不匹配)
std::string s = toString(v);                     // "42"
```

---

## 3. ConfigPaths

配置目录路径解析。

```cpp
#include "config/ConfigPath.h"

namespace Config {

struct ConfigPaths {
    std::string defaultConfig;   // config/default.yaml 的完整路径
    std::string overrideConfig;  // config/overrides.yaml 的完整路径
    std::string baseDir;         // config/ 目录路径
};

// 按优先级解析配置目录
// 1. --config <dir> CLI 参数
// 2. EGOTOUCH_CONFIG_DIR 环境变量
// 3. ./config/ 可执行文件同目录
ConfigPaths resolve(std::optional<std::string> cliOverride = {});

} // namespace Config
```

### 使用示例

```cpp
auto paths = Config::resolve();
// paths.defaultConfig = "C:/Program Files/EGoTouchRev/config/default.yaml"
// paths.overrideConfig = "C:/Program Files/EGoTouchRev/config/overrides.yaml"

auto paths2 = Config::resolve("D:/dev/test_config");
// paths2.defaultConfig = "D:/dev/test_config/default.yaml"
```

---

## 4. ConfigStore

运行时配置存储, 提供统一的 get/set/snapshot/applyPatch 接口。

```cpp
#include "config/ConfigStore.h"

namespace Config {

class ConfigStore {
public:
    // ── 生命周期 ──
    ConfigStore() = default;

    // ── 加载 ──
    // 从 YAML 文件加载。可多次调用以合并多个文件 (后加载的覆盖先加载的)。
    void loadFromYaml(const std::string& path);

    // ── 类型安全的读写 ──
    // 按点号分隔路径读取 (如 "touch.signal_cond.baseline_alpha")
    // 若键不存在或类型不匹配, 抛出 ConfigException
    template<typename T>
    T get(std::string_view path) const;

    // 带默认值的读取 (缺键返回 fallback)
    template<typename T>
    T getOr(std::string_view path, T fallback) const;

    // 写入值
    template<typename T>
    void set(std::string_view path, T value);

    // ── 查找 ──
    bool has(std::string_view path) const;
    std::vector<std::string> allPaths() const;

    // ── UI / IPC ──
    ConfigSnapshot snapshot() const;
    ConfigMutationResult applyPatch(const ConfigPatch& patch);

    // ── 持久化 ──
    void saveToYaml(const std::string& path) const;
    void saveOverrides(const std::string& path); // 仅写与 default 不同的键

    // ── 差分 ──
    // 计算与另一个 ConfigStore 的差异
    std::vector<std::string> diffKeys(const ConfigStore& base) const;
};

// ── 异常 ──
class ConfigException : public std::runtime_error {
public:
    explicit ConfigException(std::string_view message);
    const std::string& path() const;   // 出错的配置路径
    const std::string& detail() const; // 详细信息
};

} // namespace Config
```

### 使用示例

```cpp
ConfigStore store;
store.loadFromYaml("config/default.yaml");
store.loadFromYaml("config/overrides.yaml"); // 覆盖 default

int alpha = store.get<int>("touch.signal_cond.baseline_bg_alpha_shift");
bool enabled = store.getOr<bool>("touch.frame_parser.enabled", true);

store.set<int>("touch.signal_cond.baseline_bg_alpha_shift", 5);
store.saveOverrides("config/overrides.yaml");
```

---

## 5. ConfigBinder

YAML path → C++ member 的编译时绑定 — 替代 codegen 的核心机制。

```cpp
#include "config/ConfigBinder.h"

namespace Config {

struct ConfigRange {
    float min;
    float max;

    constexpr ConfigRange() : min(0), max(0) {}               // 无限制
    constexpr ConfigRange(float lo, float hi) : min(lo), max(hi) {}
    bool hasRange() const { return min != 0 || max != 0; }
};

class ConfigBinder {
public:
    // ── 值类型绑定 (int, float, bool) ──
    // 参数: (YAML路径, 成员指针, 结构体实例, 默认值, 范围, 描述)
    template<typename Struct, typename T>
    void bind(std::string_view yamlPath,
              T Struct::*memberPtr,
              Struct& instance,
              T defaultValue,
              ConfigRange range = {},
              std::string_view description = "");

    // ── 枚举类型绑定 ──
    // enumMapping: {枚举值, YAML 字符串} 的映射表
    template<typename Struct, typename Enum>
    void bindEnum(std::string_view yamlPath,
                  Enum Struct::*memberPtr,
                  Struct& instance,
                  Enum defaultValue,
                  std::span<const std::pair<Enum, std::string_view>> enumMapping,
                  std::string_view description = "");

    // ── 执行操作 ──
    // 从 ConfigStore 读取值写入所有绑定成员
    void apply(const ConfigStore& store);

    // 将默认值写入 ConfigStore (用于首次初始化)
    void writeDefaults(ConfigStore& store) const;

    // 将当前成员值写回 ConfigStore (用于 SaveConfig)
    void readMembers(ConfigStore& store) const;

    // ── 元数据 ──
    Schema toSchema() const;

    // 注册绑定数量
    size_t bindingCount() const;

private:
    std::vector<BindingEntry> m_bindings;
};

} // namespace Config
```

### 完整使用示例 (模块侧)

```cpp
// TouchPipeline.h
class TouchPipeline {
public:
    void registerBindings(Config::ConfigBinder& binder);
    void applyConfig(const Config::ConfigStore& store);

private:
    // 成员字段 — 热路径直接访问
    int m_baselineBgAlphaShift = 3;
    int m_baselineNoFingerAlphaShift = 3;
    bool m_frameParserEnabled = true;
    // ...
};

// TouchPipeline.cpp
void TouchPipeline::registerBindings(Config::ConfigBinder& binder) {
    using R = Config::ConfigRange;

    binder.bind("touch.signal_cond.baseline_bg_alpha_shift",
                &TouchPipeline::m_baselineBgAlphaShift, *this,
                3, R{0, 15}, "Background alpha shift");

    binder.bind("touch.signal_cond.baseline_no_finger_alpha_shift",
                &TouchPipeline::m_baselineNoFingerAlphaShift, *this,
                3, R{0, 15}, "No-finger alpha shift");

    binder.bind("touch.frame_parser.enabled",
                &TouchPipeline::m_frameParserEnabled, *this,
                true, {}, "Frame parser enable");
}

void TouchPipeline::applyConfig(const Config::ConfigStore& store) {
    // 可选: 在此做"键 X 变更 → 重置模块 Y"的联动逻辑
    if (store.has("touch.frame_parser.enabled")) {
        bool wasEnabled = m_frameParserEnabled;
        m_frameParserEnabled = store.get<bool>("touch.frame_parser.enabled");
        if (!m_frameParserEnabled && wasEnabled) {
            resetParserState(); // 关闭时重置状态
        }
    }
    // 其余键由 ConfigBinder::apply() 自动处理
}

// 每帧处理 — 直接读成员, 零开销:
void TouchPipeline::processFrame(Frame& frame) {
    int alpha = m_baselineBgAlphaShift;      // 编译为直接成员读取
    if (!m_frameParserEnabled) return;
    // ...
}
```

---

## 6. SchemaValidator

启动时配置校验。

```cpp
#include "config/SchemaValidator.h"

namespace Config {

// Schema 定义
struct SchemaField {
    std::string path;
    Type type;
    bool required = false;
    ConfigValue defaultValue;
    ConfigValue minValue;
    ConfigValue maxValue;
    std::vector<std::string> enumValues; // 仅 Enum 类型
};

struct Schema {
    uint16_t version = 1;
    std::vector<SchemaField> fields;

    // 从 YAML 加载
    static Schema fromYaml(const std::string& path);
};

// 校验问题
struct ValidationIssue {
    enum Severity : uint8_t { Error, Warning, Info };

    Severity severity;
    std::string path;
    std::string message;
};

struct ValidationResult {
    std::vector<ValidationIssue> issues;

    bool ok() const;                        // 无 Error
    bool hasErrors() const;
    bool hasWarnings() const;
    void logAll() const;                    // LOG_ERROR / LOG_WARN
};

// 校验器
class SchemaValidator {
public:
    // 校验 ConfigStore 是否符合 Schema
    ValidationResult validate(const ConfigStore& store, const Schema& schema);

    // 从 ConfigBinder 自动生成 Schema (无需 schema.yaml)
    static Schema fromBinder(const ConfigBinder& binder);
};

} // namespace Config
```

### 校验行为

| 场景 | 严重级别 | 处理 |
|------|---------|------|
| 类型不匹配 (expected int, got string) | Error | 拒绝启动 |
| 必填键缺失 | Error | 拒绝启动 |
| 值超出范围 (150, max=100) | Warning | clamp 到边界 |
| ConfigStore 中有未知键 (schema 未定义) | Info | 忽略, 记录日志 |
| schema 中定义但 YAML 未提供 | Info | 使用 defaultValue |

### 使用示例

```cpp
auto schema = Schema::fromYaml("config/schema.yaml");
// 或从 Binder 自动生成:
// auto schema = SchemaValidator::fromBinder(binder);

ConfigStore store;
store.loadFromYaml("config/default.yaml");

SchemaValidator validator;
auto result = validator.validate(store, schema);
if (!result.ok()) {
    result.logAll();
    throw std::runtime_error("Config validation failed");
}
```

---

## 7. YamlParser

yaml-cpp 封装, 提供类型安全的 YAML 读写。

```cpp
#include "config/YamlParser.h"

namespace Config {

class YamlParser {
public:
    // 从文件加载 YAML::Node
    static YAML::Node load(const std::string& path);

    // 将 YAML::Node 的内容灌入 ConfigStore
    // prefix: YAML 路径前缀 (如 "touch")
    static void populateStore(const YAML::Node& root,
                              ConfigStore& store,
                              std::string_view prefix = "");

    // 将 ConfigStore 的内容序列化为 YAML::Node
    static YAML::Node fromStore(const ConfigStore& store);

    // 保存 YAML::Node 到文件
    static void save(const YAML::Node& root, const std::string& path);

    // 差分保存: 仅输出 base 与 override 之间不同的键
    // 保持 YAML 结构和注释
    static void saveDiff(const ConfigStore& base,
                         const ConfigStore& override,
                         const std::string& path);
};

} // namespace Config
```

---

## 8. 完整启动流程

```cpp
// ── ServiceHost 启动时 ──

// 1. 解析配置路径
auto paths = Config::resolve(cmdLineConfigOverride);

// 2. 加载 YAML 文件
ConfigStore store;
store.loadFromYaml(paths.defaultConfig);
if (fs::exists(paths.overrideConfig)) {
    store.loadFromYaml(paths.overrideConfig);
}

// 3. 注册绑定 + 校验
ConfigBinder binder;
touchPipeline.registerBindings(binder);
stylusPipeline.registerBindings(binder);
serviceConfig.registerBindings(binder);

auto schema = SchemaValidator::fromBinder(binder);
SchemaValidator validator;
auto result = validator.validate(store, schema);
if (!result.ok()) {
    result.logAll();
    return EXIT_FAILURE;
}

// 4. 应用到模块
binder.apply(store);
touchPipeline.applyConfig(store);
stylusPipeline.applyConfig(store);

// 5. Service 就绪
```

---

## 9. 与当前架构的对应关系

| 当前组件 | 目标组件 | 说明 |
|---------|---------|------|
| `ConfigParam::void* valuePtr` | `ConfigBinder::bind(path, memberPtr, instance, ...)` | 编译时类型安全 |
| `IConfigProvider::GetConfigSchema()` | `ConfigBinder::toSchema()` | 从绑定自动推导 |
| `IConfigProvider::LoadConfig(key, value)` | `ConfigBinder::apply(store)` | 一次性批量应用 |
| `IConfigProvider::SaveConfig(ostream&)` | `ConfigStore::saveToYaml(path)` | YAML 代替 INI |
| `ParseConfigInt/Float/Bool()` | `ConfigValue` variant 类型系统 | 不需要手写转换 |
| `IsFrozenCurrentTouchConfigKey()` | 不存在 — frozen 键在 YAML 中注释掉 | 运行时无效的键不参与绑定 |
| `config_key_sync.py` | 不存在 — 无代码生成 | 直接编辑 YAML + binder.bind() |
| `ServiceConfigCore::ParseServiceConfig()` | `ConfigBinder::apply(store)` | 自动分发 |
| `ServiceConfigCore::DiffServiceConfig()` | `ConfigStore::diffKeys(base)` | 通用差分 |

---

## 10. 线程安全

| 组件 | 线程安全 | 说明 |
|------|---------|------|
| `ConfigStore` | 外部锁 | 不支持并发读写; 调用方负责同步 (使用 `DeviceRuntime::m_pipelineMu`) |
| `ConfigBinder` | 外部锁 | 注册阶段单线程; `apply()` 需与成员字段的读取者互斥 |
| `SchemaValidator` | 不可变 | `validate()` 只读, 线程安全 |
| 成员字段 | 外部锁 | 热路径直接读取; 重载时写入需持有 Pipeline 锁 |

**推荐锁策略**: 当前 `DeviceRuntime::m_pipelineMu` 已提供 Pipeline 级别的读写互斥。ConfigBinder::apply() 在持有此锁的前提下调用即可。

---

## 11. 错误处理策略

| 场景 | 行为 |
|------|------|
| `default.yaml` 不存在 | 致命错误 — 拒绝启动, LOG_ERROR |
| `overrides.yaml` 不存在 | 非错误 — 仅使用 default |
| YAML 格式错误 | 致命 — yaml-cpp 抛出异常, 打印行号和上下文 |
| 类型不匹配 | 致命 — SchemaValidator 报告精确路径和期望类型 |
| 值越界 | 警告 — clamp 到合法范围, 日志记录 |
| 未知键 (ConfigStore 中有但未绑定) | 信息 — 记录日志, 不阻止启动 |
| `binder.bind()` 中的路径拼写错误 | 编译通过但运行时缺键 → SchemaValidator 检测到 default.yaml 中未使用键 |

> :warning: 路径拼写不会在编译时捕获。建议在 CI 中运行 `diffKeys` 检查确保每个 binder.bind() 的路径都在 default.yaml 中出现。
