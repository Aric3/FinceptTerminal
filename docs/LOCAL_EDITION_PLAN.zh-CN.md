# 本地维护版改造评估与实施决策

评估日期：2026-10-06。源码基线：Aric3/FinceptTerminal，main，3f444cb7ab712d85a57d270d5edd6e7a2ef50d55。
前半部分保留首次评估与实施路线；各轮已完成内容和验证结果记录在文末。评估时已验证原版编译、首次 Python 初始化，以及默认分析环境中的 AAPL/MSFT 公开行情脚本请求。

## 产品目标

正常启动 macOS 应用即可进入本地工作区。公开数据、用户自己的数据源和 AI 服务均可联网；“本地模式”指不依赖 Fincept 的账号和订阅，不等于禁止联网。默认不请求官方云服务，不显示商业推广。用户主动配置某个服务后才启用对应集成。

保留现有 Qt/C++ 桌面架构、Python 分析脚本、SQLite 与工作区功能。通过独立提交逐步改造，保持能同步上游技术修复。

## 已确认的耦合

- `fincept-qt/src/app/WindowFrame_Auth.cpp`：登录、PIN、本地窗口可见性及 `has_paid_plan()` 相互绑定；未满足套餐判断会进入 PricingScreen。
- `fincept-qt/src/app/main.cpp` 与 `ui/widgets/EnterprisePromo.*`：首次初始化后和普通启动都安排升级推广弹窗。
- `ui/navigation/ToolBar.cpp`、`NavigationBar.cpp` 与 WindowFrame：存在 UPGRADE、官方积分和套餐入口。不能只隐藏一次弹窗。
- `network/http/HttpClient.cpp`：只是网络入口之一；源码中 46 个 .cpp 文件引用 QNetworkAccessManager，8 个引用 QWebSocket。这不是已确认的实例数。
- `python/PythonRunner.cpp::build_python_env()`、`PythonSetupManager.cpp::run_process()`：继承进程环境，但没有把 macOS 系统代理统一转换并传给子进程。
- `main.cpp` 启动组：注册官方云同步、宏观日历、海运及计费/代币相关 producer。
- `core/config/AppConfig.cpp`：API 和 cloud 默认地址均为官方域名。
- `core/config/AppPaths.cpp`、main.cpp 和 CMake：数据根目录、应用名和 bundle identifier 使用官方标识。
- `services/updater/UpdateService.h`：清单指向官方仓库；当前签名公钥为空，更新校验会拒绝更新，不能描述为已经在自动覆盖应用。维护版仍须建立自己的更新来源和信任配置。
- `storage/secure/SecureStorage.cpp`：Windows 使用 DPAPI；macOS 当前为基于机器信息的密钥派生和 AES 加密，源码明确说明 Keychain 尚未接入。

## 1. 统一代理（P0，复杂度中高）

新增应用级 ProxyManager 和设置页“网络”：

1. 跟随系统（默认）。
2. 应用内手动代理：HTTP/HTTPS 目标的 HTTP CONNECT 代理，以及客户端支持的 SOCKS5。
3. 直连。

应用内设置只影响本应用，不改写 macOS 的全局代理。设置页可以显示系统当前值和打开系统网络设置的入口。

ProxyManager 管理配置、例外地址、协议能力和变更信号；在任何网络服务启动前初始化。Qt 使用应用级代理工厂；另行检查显式设置代理的客户端。Python、UV、curl 与其他子进程使用统一的环境构造器；PythonRunner、PythonWorker、PythonSetupManager 都必须接入。

优先级：用户显式选择的手动/直连设置最高；跟随系统模式使用系统设置。开发或自动化传入的显式代理环境保留为有文档说明的覆盖机制，不允许应用静默覆盖。直连模式必须清除子进程继承的代理变量。localhost、127.0.0.1 和 ::1 固定绕过；其他例外遵循设置。代理密码进入安全存储，日志只记录脱敏地址。

应用代理变更后，新请求使用新配置；长连接有控制地重连，Python worker 重启。不自动重发具有副作用的交易请求。

第一批覆盖本机现有的静态 HTTP/HTTPS 系统代理及应用内手动配置。PAC 是按 URL 决策的脚本，不能简单转换成一个 HTTP_PROXY；需独立验收 Qt 和 Python 路径。第一批对 PAC-only 配置显示明确状态与手动配置入口，不宣称支持。完整 PAC 与 SOCKS-only 跨客户端一致性随后做能力测试，必要时引入本机代理桥。

验收：Finder 启动时没有 shell 代理变量，Qt HTTPS、实际 yfinance 脚本、UV 下载及一个 WebSocket 客户端仍按设置联网；覆盖手动、系统、直连、例外地址、错误代理、设置切换与重启。测试代理失效时，不擅自切换到直连。

参考：https://doc.qt.io/qt-6.8/qnetworkproxyfactory.html

## 2. 本地模式与商业界面清理（P0，复杂度高）

引入明确的应用模式/服务能力配置，并在本地构建中默认启用。工作区是否可进入，与云端是否已认证是两种状态；不伪造官方 session、API key 或“已付费”状态。

- 启动直接进入本地工作区，使用已有 profile / SQLite / layout 基础设施。
- 本地 PIN 可选，独立于官方登录；注销云账号不能把本地工作区锁回登录页。
- 官方登录移至可选服务连接设置，第一版默认不启用。云端 401 只让对应连接失效。
- 移除升级弹窗、Enterprise banner、UPGRADE 按钮、套餐/支付/充值导航和官方积分展示；不依赖用户勾选“不再提示”。从构建和注册入口禁用相关商业组件，检查菜单、响应式布局、翻译和快捷键。
- 不删除量化分析里的“定价”计算器，也不隐藏用户自己的第三方 API 费用/配额错误。
- 保留原作者署名、许可证及必要的来源说明；这些与商业广告不同。

验收：全新 profile、未登录、无网络及官方 API 返回 401 时，本地工作区仍可进入；重启和缩放窗口后无商业入口；本地文件、组合、笔记、回测和用户自带 AI 提供商可正常使用。

## 3. 官方服务解耦（P0 基础隔离；P1/P2 逐项替换，复杂度高）

第一批停止默认注册/启动官方云同步、官方账号/订阅刷新和计费相关 producer，清理官方代币推广/费用优惠/质押入口。保留通用钱包、行情及交易功能，按实际依赖区分。

所有服务标识其提供方、所需凭据、连接状态及本地可用能力。未配置的服务显示“未配置数据源”或从默认工作区移除，不持续轮询官方接口，不显示购买引导。

HTTP / Cloud 边界再增加一层官方服务请求策略，避免遗漏后台路径。显式配置连接才允许对应源。凭据按提供方和源隔离，配置自建服务不能复用或外送官方令牌。

数据能力分组：

| 类别 | 第一批行为 | 后续方向 |
| --- | --- | --- |
| Yahoo/yfinance、AKShare、RSS、公开宏观接口 | 保留并验证 | 重试、缓存、限流、备用源 |
| Databento、新闻及其他自带 key 提供商 | 按用户配置启用 | 独立配额及连接诊断 |
| 海运、新闻事件、官方宏观日历 | 默认停用官方后端 | 逐项接公共源或自建服务 |
| 官方 AI / 研究接口 | 默认未配置 | OpenAI-compatible、其他支持提供商或本地模型 |
| 云同步 | 默认停用 | 本地备份优先，再实现独立自建服务协议 |

移除客户端套餐门槛不等于拥有服务端数据、历史归档或付费供应商权限。

验收：全新本地启动、默认工作区与定时刷新都没有 api.fincept.in 请求；测试涵盖非 HttpClient 的入口。用户主动启用自有服务后只使用对应凭据。

## 4. 原生应用、数据目录和更新（P0，复杂度中）

- 生成真正可通过 Finder、Dock 和 Spotlight 启动的 .app；运行时自行加载代理配置，不依赖 .command、Terminal 或开发者 PATH。
- 将 Qt frameworks/plugins、WebEngine helper/resources、OpenSSL 和资源打入 app；开发构建可复用 SDK，但安装版不得依赖 build/_deps 或 Homebrew 路径。
- 使用独立 bundle identifier、应用偏好域和数据根目录。名称暂用工作名称，发布前再统一品牌。
- 引入数据根目录配置及开发模式覆盖。旧数据采取显式、可回滚导入；数据库备份后再迁移，不自动删除旧数据。
- 固定迁移后的 Python 运行时绝对路径；venv 不应直接移动，因为入口及路径可能引用旧目录。按包清单重建，复用缓存。
- 默认禁用上游安装器更新通道；后续使用 fork release、自己的清单与签名密钥，保留哈希及签名验证。
- 本机 OpenSSL 的最低系统版本较高。独立分发前确定 macOS 支持范围并匹配构建库，不宣称当前开发产物可在旧系统运行。

验收：从 Finder 启动，无 Terminal；应用目录可移动；运行不依赖源码目录；偏好和数据库位于独立路径；升级保留数据；更新不能拉取官方安装器。

## 5. 凭据、依赖和诊断（P1）

macOS 接入 Keychain 保存或包装安装密钥，保持已有加密数据可迁移。用假凭据验证迁移、锁屏、拒绝访问与 profile 隔离，不输出实际密钥。保留日志脱敏。

Python 依赖分为核心行情、回测、ML/AI、特殊交易平台等功能组。核心组足以进入工作区；其他功能按需安装，使用锁定版本与缓存。首装界面显示正在下载的包、大小、进度与具体错误。当前 PySide6 由 vnpy 等 Python 依赖引入，只有确认不使用对应功能后才可排除；C++ Qt SDK 与 Python Qt 不能直接互换。

添加统一诊断：实际代理路径、数据源状态、最近成功时间、限流、缺失依赖与离线缓存。保留 API 配额和错误事实，不把服务缺失包装成“功能解锁”。

## 实施顺序与交付

1. 独立工作分支和本地构建开关；分离本地工作区、登录及 PIN；关闭商业界面与默认官方服务。建立无官方请求的验收基线。
2. ProxyManager、设置页、子进程环境；验证各网络路径，不再使用 shell 启动代理。
3. 原生 .app 打包、独立标识和数据目录、迁移入口、停用上游更新。至此交付第一版日常可用的本地应用。
4. Keychain 与数据源/AI 配置体验；补齐已发现的凭据管理限制。
5. 精简依赖与首装体验，再按实际需要替换海运、宏观日历、云同步等服务。

每个步骤拆成可独立评审/回滚的提交。第一批不重写 UI 框架、不一次性替换全部数据后端、不批量删除所有包含 fincept / pricing 字样的代码。

验证以新 profile 和离线启动、实际 Qt/Python/UV 网络路径、官方域名请求记录、原生启动和数据迁移为主；增加代理与服务能力的关键行为测试，并运行相关现有测试。行情供应商出现临时限流时，用本机测试服务区分代理路由问题和上游服务问题。


## 首轮实施记录（2026-10-06）

已实施 P0 三项：

- 本地版默认直接进入工作区；认证状态仍为未登录，已有 PIN 仍须验证。PIN 达到失败次数上限后按一小时冷却继续验证，不通过清除 PIN 或云端登录绕过。新工作区不强制设置 PIN。
- 构建开关 `FINCEPT_LOCAL_EDITION=ON` 默认开启；官方 HTTP/Cloud 请求在发送前异步拒绝。默认云同步、官方更新、新闻 WebSocket、健康探测和推广弹窗停用。账户、论坛、客服、官方代币模块从常用导航及组件目录过滤；旧布局中的相应模块显示不可用说明。
- CHAT 进入可自配提供商的 AI Chat；旧官方 LLM 配置不会被自动选中，也不会出现在本地提供商列表。尚未配置 AI 时明确保持未配置状态。
- `Settings → Network & Proxy` 提供系统、手动 HTTP、直连及旁路主机配置。Qt HTTP/WebSocket 的应用代理工厂、WebEngine 的 Chromium 代理参数，以及 PythonRunner、常驻 PythonWorker、uv/curl 安装进程共用启动配置。保存后重启，避免旧连接和新进程使用不同策略。直连清理继承代理变量；应用内配置优先于终端继承的 HTTP_PROXY。
- 本地身份为 `dev.aric.fincept.local`，偏好设置使用 `Aric/FinceptLocal`，数据位于 `~/Library/Application Support/dev.aric.fincept.local`。旧目录保留；没有自动迁移账户或密钥。Python 虚拟环境在新目录重新创建，使用旧下载缓存硬链接复用，不移动旧 venv。
- 原生应用安装到 `~/Applications/Fincept Local.app`。`fincept-qt/packaging/macos/package-local.sh` 接受构建目录、Qt 前缀、全新目标路径三个参数，打包 Qt/WebEngine/OpenSSL 并进行本地签名。重复打包须使用新目标路径，脚本不覆盖现有应用。

验证：完整编译通过；Qt Test 6/6 通过。新增集成测试覆盖六种 HTTP 操作的官方服务拦截、异步回调、接收对象销毁、独立 HTTP 服务、本地模拟代理真实转发、旁路和直连环境变量处理。原生无终端启动直接进入仪表盘；Qt 和行情 Python 进程均实际连接系统代理 `127.0.0.1:7897`，27/27 行情返回，新闻获取 1,556 条。代理界面打开和保存验证通过，应用签名校验通过。没有推送远端。

限制与后续：系统模式先支持静态 HTTP/HTTPS；PAC 与仅 SOCKS 的设置请在手动模式填写代理软件的 HTTP 端口。本机 Homebrew OpenSSL 最低要求 macOS 26，因此此包用于当前机器，尚非跨版本发行包。官方 QuantLib/宏观日历/海事等云服务尚未实现本地替代；不得将“本地可进入”理解为这些数据自动可用。P1 的 Keychain、数据提供商能力标记、依赖分组和自建服务接入继续按本计划实施。


退出回归补充：原生 Command-Q 暴露了上游 `lastWindowClosed` 同步调用 `quit()` 的 macOS 重入问题。已将该连接改为 `Qt::QueuedConnection`；修复版实际 Command-Q 后写入 clean-shutdown 标记，主进程退出，未产生新的崩溃报告。宏观日历保留精确 topic 的生产者注册，官方请求仍在 HttpClient 发送前拒绝，使组件立即收到不可用错误而非等待通用生产者超时。


## P1：macOS Keychain（2026-10-06）

安装密钥现存放在 macOS login Keychain；每个数据库使用独立的随机账户 ID，SQLite 仅保存引用和 AES-GCM 密文。旧 PBKDF2 包装先解密、写入 Keychain 并读回校验，然后在同一 SQLite 事务中写引用、移除弱包装；已有凭据密文不变。更早的直接机器密钥存储在事务中重新加密；任何无法解密的旧行都会中止迁移。

Keychain 拒绝访问、缺失密钥、旧包装损坏、SQL 写入失败均保留原有数据，不降级或生成替代密钥覆盖旧内容。启动时先初始化凭据保护，再加载 PIN/账户；失败时显示错误并退出，避免将读不到的 PIN 当作未设置。失败可重试，缓存按数据库路径隔离。

本机签名仍为 ad-hoc；首次使用或重新编译后，macOS 可能要求允许应用访问 Keychain。应按系统提示授权本应用自己的条目，不设置任何应用均可访问。OS 锁屏不会清除应用已经读入内存的密钥；应用 PIN/自动锁仍由原有模块负责。数据库文件本身不包含 Keychain 密钥，单独复制 DB 不能迁移凭据；数据备份与密钥迁移需另做显式流程。迁移后不要直接启动此前不识别 Keychain 的旧构建；代码降级需要配套的密钥迁移方案。

验证：独立假凭据测试覆盖 12 项行为（Qt Test 共 14 项含初始化/清理），包括旧包装与旧行迁移、空值、拒绝访问与重试、Keychain 读回失败、SQL 插入/删除回滚、损坏密钥、缺失 Keychain 及 profile 隔离。真实 Keychain 接口另用随机 UUID 和公开假值验证新增、读取、拒绝覆盖、删除；测试条目已清理，未读取用户已有条目。完整应用构建通过。数据源能力标记、依赖分组和自建服务替换仍待后续实施。

接口依据：[Apple Keychain 实现说明](https://developer.apple.com/documentation/technotes/tn3137-on-mac-keychains)、[SecItemCopyMatching](https://developer.apple.com/documentation/security/secitemcopymatching(_:_:))。


本轮交付：更新后的原生应用已安装至 `~/Applications/Fincept Local.app`，签名校验通过。全套 8/8 测试通过，其中真实 Keychain 集成测试为显式启用项，默认关闭；它只访问临时数据库与自身新建的假条目，包含重新加载、解密、拒绝覆盖及缺失密钥拒绝访问，测试条目已删除。用户既有工作区未自动启动；真实凭据将于下次正常打开应用时按上述事务流程迁移。

可重复验证：开发环境执行 `dev configure -DFINCEPT_BUILD_TESTS=ON`、`dev build`，再用 SDK 中的 `ctest --test-dir <build> --output-on-failure`。只有主动测试真实 Keychain 时才增加 `-DFINCEPT_TEST_REAL_KEYCHAIN=ON`；完成后恢复 OFF，日常测试不操作系统 Keychain。原生包启动的 profile 测试因会改写真实 `profiles.json` 未执行，采用真实 Keychain + 纯临时 SQLite 的集成测试替代。


### 2026-10-06：持续加载与 macOS 辅助功能崩溃修复

- DataHub 取消尚未派发的合并请求时同步清除 `in_flight`；页面迅速隐藏、重新订阅不会再被取消请求留下的标记挡住。已派发请求保留去重标记。
- 请求超时通过 `publish_error()` 通知直接和通配符错误订阅者，面板结束加载并显示错误。超时当次调度不启动替代请求，避免错误通知清除新请求标记。
- 本地模式也注册宏观日历的精确生产者；官方服务被禁用时立即显示不可用提示，不再落入通用经济数据生产者后一直等待。
- 实际 Command-Q 复现了 `QAccessibleCache::removeAccessibleElement` 崩溃。Qt 6.8.3 Cocoa 表格占位单元格提升后，行数组和缓存共享一个所有权引用，缓存失效会释放数组仍引用的对象。只在提升时增加缓存所有权引用，不关闭辅助功能，也不改系统 Qt。
- `fincept-qt/packaging/macos/cocoa` 提供固定版本、SHA256 校验的官方源码构建脚本、最小补丁及独立复现程序。原版测试退出 SIGSEGV；修补版及打包后的插件均完成 20 次 8×5 表格读取、清空、销毁循环并退出 0。应用测试 8/8 通过。
- Qt 6.8.3 原生打包要求 `FINCEPT_COCOA_PLUGIN`，在 macdeployqt 后复制修补插件并移除 SDK 绝对 rpath，避免重新打包丢失修复。包内保留补丁、源码地址及原始许可证。

复现构建（在隔离 SDK 的 bash 环境中）：

```bash
source /Users/ac/workspace/projects/FinceptTerminal-dev/env.sh
/Users/ac/workspace/projects/FinceptTerminal/fincept-qt/packaging/macos/cocoa/build-cocoa-patch.sh "$QT_DIR" "$FINCEPT_DEV_ROOT/packages/cocoa-build"
FINCEPT_COCOA_PLUGIN="$FINCEPT_DEV_ROOT/packages/cocoa-build/plugins/platforms/libqcocoa.dylib" \
  /Users/ac/workspace/projects/FinceptTerminal/fincept-qt/packaging/macos/package-local.sh \
  "$FINCEPT_DEV_ROOT/build" "$QT_DIR" /private/tmp/FinceptLocal-new.app
```

本机最终验证：已安装到 `/Users/ac/Applications/Fincept Local.app`。实际行情页 9 个面板均有数据，切换页面后正常显示；第二次启动后读取到 270 个单元格、无 `LOADING`。两次 Command-Q 均无新崩溃报告，第二次受控进程退出码 0。原生应用已重新打开。少量 Yahoo 无行情的代码（如 `LBS=F`、`UNI-USD`）仍会返回上游数据错误，不代表整个应用离线。


## 功能验收第一轮（2026-10-06）

本轮从 `WindowFrame_Setup.cpp` 的注册表提取全部 52 个页面，记录在
`docs/LOCAL_FUNCTION_ACCEPTANCE.csv`。状态区分部分通过、待验收、待配置、官方后端缺失及本地版停用。
没有把页面构建成功当作业务功能正常，也没有把单元测试通过当作整页通过。
尚未逐个运行所有页面；待验收项明确保留。CSV 是本次检查快照，后续实现变更须更新。

实际界面：新闻有 824 篇文章、694 个簇、48 个来源、53 feeds；这些数量不证明所有文章新鲜或所有来源联网成功。
组合页正确显示新建/导入 JSON/加载演示的空工作区，没有向用户数据库写演示持仓。
本轮后续界面检查被系统锁屏暂停；已请求用户解锁。
此前行情加载、快速切换和实际退出的证据见上一节；本轮未重测退出。

新增 `tst_feed_parsing` 复用已有 FeedSelfTest，直接测试真实 RSS/Atom/JSON/CSV 解析与时间处理，
无需启动应用、联网、用户数据库或 Keychain。构建成功，CTest **9/9** 通过。
测试涵盖代理路径、本地政策、凭据迁移、DataHub 刷新、指标、订单验证及解析等关键边界；
不覆盖组合收益正确性、历史K线、策略回测、独立LLM及交易平台连接。

### 自动化接口核实与实施决定

当前有 `--dump-tools`、`--selftest-*` 和 `--smoke-test`；它们是应用诊断/自测入口，
在常规启动初始化之后处理，不是稳定的业务 CLI。`--smoke-test` 主要测试页面构建。
Python `yfinance_data.py` 有 quote/search/historical 等 JSON 命令，但直接运行绕过应用的服务及代理配置，
不能作为本机应用联网通过的证据。

内部 `TerminalMcpBridge` 提供 loopback HTTP `GET /tools` 和 `POST /tool`，随机端口，
每次运行令牌及额外的破坏性操作许可。现有 AgentService 用它调用工具。
这不是可直接配置给外部客户端的标准 MCP 传输；也尚无外部客户端的端口发现、令牌交接和生命周期接口。
外部 MCP 服务器不是分析工作的前置条件。当前可通过界面、受控脚本或现有数据源完成读取和分析；
要让 Codex 稳定调用应用，应复用内部服务工具，并先建设小型 JSON CLI，再加标准 MCP 适配层。

接入前缺陷：`ThreadHelper.h::run_async_wait` 有超时，部分调用者仍把结果写入栈变量。
`MarketsTools.cpp` 的 quote/history 回调捕获 `&ok`、`&q` / `&points`；若回调晚于超时返回，存在失效引用风险。
这来自源码检查，尚未做故障注入复现；不能仅因超时为120秒而接受。
应为所有异步工具审核生命周期、线程同步和取消机制，并增加迟到回调的回归测试后再开放接口。

下一批按依赖排序：
1. 核心闭环：有效代码报价→历史K线→临时组合CRUD/导入导出→确定性收益与回测→独立LLM。
2. 数据能力提示：把官方后端缺失、缺API key、缺Python包、上游限流区分显示；优先替代宏观日历/QuantLib。
3. 修复异步工具生命周期，再实现只读 CLI 首批命令：doctor、quote、history、news、portfolio list/holdings。
   命令名为拟定接口，尚未实现；要求 JSON 输出、来源及时间、错误分类、进程超时，不打印凭据。
4. 标准 MCP 适配器调用同一命令/服务；不重复建立数据抓取与计算逻辑。写操作和交易不纳入首批。


### 解锁后的核心功能验收（2026-10-06 20:49–20:56）

- 新闻关键词 Nobel 得到14条，使用唯一无匹配关键词得到0条，清空后恢复833条。
  发现无匹配状态误显示“请检查网络连接，然后点击刷新”，列为界面缺陷，尚未修改。
  OFFLINE 标签属于官方实时WebSocket状态，不能说明RSS不可用，后续需分别标记。
- AI Chat 实际显示“无提供商、无模型”，等待用户配置独立提供商后才可验收真实聊天。
- 股票交易页默认 RELIANCE/NSE 且报价为空；源码 hub_subscribe_streaming 在无聚焦账户时直接返回。
  未接入券商行情，不将空报价判为全局代理故障；需要明确缺账户的界面提示。
- 股票研究 AAPL 报价、公司信息、1Y/1M日K以及利润表加载成功。
  这证明显示链路工作；服务可读取缓存，本轮没有逐项确认每次网络新请求或外部价格准确性。
- 默认 VectorBT 回测：SPY，2025-10-06 到2026-10-05，日线，SMA10/20，初始100000，
  佣金0.1%、滑点0.05%、无风险利率4%。返回 completed，250条权益数据，8笔交易，
  期末103983.00081829363，收益3.983%，最大回撤5.5565%，胜率50%，手续费1624.2082214875982。
  JSON标记 dataSource=yahoo、usingSyntheticData=false；日志记载8入场信号、7出场信号。
  权益曲线渲染正常，选择 /private/tmp 后JSON导出成功。
- 原生导出默认 Macintosh HD 根目录，直接保存报只读文件系统错误，应改为用户可写默认目录。
  首次把绝对路径放入文件名也被macOS改为冒号，属于错误的自动化操作，不能作为导出缺陷证据。
  后续使用展开目录面板、Go to Folder选择 /private/tmp，再填写普通文件名完成导出。
- 对导出JSON独立重算：曲线末值=期末资金；期末/初始−1=收益；逐日峰值回撤=摘要最大回撤；
  交易数组长度=交易数；逐笔commission总和=手续费；正pnl比例=胜率；逐笔pnl总和=资金增量。
  全部一致。此检查验证结果内部一致性，不证明策略无前视偏差或每个指标的算法正确。

导出证据保留于当前任务的 `acceptance/spy-vectorbt-20261006.json`，
绝对路径 `/Users/ac/Documents/Codex/2026-10-05/https-github-com-fincept-corporation-finceptterminal/acceptance/spy-vectorbt-20261006.json`。
本轮仅读取行情/执行回测及写验收输出，没有添加持仓、连接账户或执行订单；
组合CRUD/导入导出仍待临时数据库隔离验收。应用程序未重新安装；本轮更新验收记录。


### 组合与自选隔离验收、界面修复（2026-10-06）

新增 `tst_portfolio_storage`，直接链接真实 PortfolioRepository、WatchlistRepository、SettingsRepository、
SyncOutbox/CloudSyncSettings、Database，以及实体所需的真实 v001/v006/v019/v022 迁移。
每个测试使用独立 QTemporaryDir SQLite文件；明确关闭同步；没有启动应用、联网、操作Keychain或真实持仓。
这是仓储和相关迁移集成测试，不是全应用迁移或业务服务端到端测试。

已验收：
- 10股@100再加5股@160得到15股、加权成本120；归一大小写，保留原行业及券商代码/交易所/首购日。
- 改名后关闭并重新打开同一数据库，组合名称、券商关联和资产成本均持久化。
- 交易更新到3股@120时总金额为360；数据库拒绝非法交易类型。
- 同一日快照替换，不产生重复；删除组合后资产、交易、快照均清理，拒绝孤立资产。
- 自选重复代码只保留一条，改名/关闭重读正常；删除自选清理代码，拒绝孤立代码。

完整应用编译成功，CTest **10/10**通过；格式及diff检查通过。
基本计算验证覆盖加权成本和交易金额，市值/盈亏、JSON导入导出与失败事务回滚仍需补充。

已修复源码：
- 新闻有原始文章但筛选为空时，显示“没有匹配的文章/清空搜索或调整筛选条件”，不误报断网。
  保留无原始文章时的连接提示；语言切换保持空状态原因，新增简体/繁体翻译。
- 回测JSON导出默认使用 QStandardPaths DocumentsLocation，空值回退用户主目录，避免默认根目录。

更新前另观察到 ^GSPC 研究页部分请求报 curl TLS connect error，而报价及仪表盘行情有数值。
未将该局部错误当作已修复，也未据此判定全局断网；后续须单独诊断与验证重试恢复。

安装状态：已打包并签名验证，安装到 `/Users/ac/Applications/Fincept Local.app`，保留旧应用
`FinceptTerminal-dev/backups/FinceptLocal-before-acceptance.bundle-backup`（非应用后缀，避免标识冲突）。
新进程已启动；原生界面自动化超时后，系统清单明确报告Mac锁屏。
已请求用户解锁，新闻新提示与Documents默认目录的最终界面验收尚未完成；
不将编译/签名/测试通过替代这两项界面验收。

解锁后最终界面验证（2026-10-06 22:47）：无匹配关键词显示“没有匹配的文章/清空搜索或调整筛选条件以查看更多文章”，清空后恢复797条文章。回测已有优化命令执行完成后，导出面板默认 Where=Documents、文件名backtest_results.json，确认后取消保存，未写新的用户文件。两项界面修复均已实际验证；^GSPC局部取数错误仍保留，未标为修复。


### 组合 JSON 与估值闭环（2026-10-06）

本轮抽取 PortfolioTransfer（真实仓储/SQLite 事务）和 PortfolioValuation（纯计算），应用服务与测试共用同一实现。
新 leaf .cpp 显式跳过 unity 编译，保持原有 batch 排列；测试只增加所需的 leaf，不链接整个应用。

已修改并验证：
- JSON 2.0 同时保存当前持仓和交易历史。支持空组合、无交易记录持仓、手动修改过数量/成本的持仓；
  保留行业、首购日、券商代码/交易所、描述和交易备注；BUY/SELL/DIVIDEND/SPLIT 历史独立于持仓恢复。
  不迁移 broker_account_id 或凭据。导出不再截断到10000条，读错误直接失败，JSON 文件使用 QSaveFile 原子替换。
- JSON 1.0（及省略版本号的旧文件）仍按日期重放交易，同日 BUY 优先；保留行业提示，兼容省略 owner。
  旧文件的 SPLIT 明确拒绝，不再静默丢弃。旧文件缺少的真实持仓状态无法靠交易回放补回。
- 在创建/合并前校验版本、结构、日期、有限数值、非负持仓数量/成本、交易类型和重复持仓代码；
  合并拒绝不同币种。写入全程一个事务；用 SQLite trigger 在持仓已写入后制造交易插入失败，
  验证新建无残留、合并原数量与交易数不变。另验证 BUY 成功后超额 SELL 整体回滚。
- 合并仍为累加持仓、追加交易；重复导入会重复添加，界面提供明确中文说明及提示。
  新导入不自动生成历史 NAV；从当前持仓回填不能反映真实历史持仓，保留显式回填入口待单独审计。
- 纯估值验证：10股@100、报价120及5股@200、报价180时，总市值2100、成本2000、盈亏100（5%），
  权重合计100%。第二项缺报价时按成本显示，总市值2200、报价覆盖1/2；日涨跌百分比只以有报价部分为基数。
  全部缺报价、零/负/NaN报价、空组合与零成本均有回归检查；无报价持仓不计入盈利持仓数。
- 不完整报价不缓存、不写每日快照及自定义指数观察值，避免覆盖当天有效净值。
  列表缺报价显示“不可用”，状态提示报价覆盖及成本回退；有效 broker tick 清除缺报价 tooltip。
  这是纯计算/策略判断测试及服务分支检查，没有在真实持仓上注入网络故障验证。
- 卖出使用按数量缩放的浮点容差；2e-10持仓卖出1e-10后保留1e-10，额外卖出1.1e-10拒绝且回滚。

验证命令：隔离 SDK `dev build` 完整构建；`ctest --test-dir FinceptTerminal-dev/build --output-on-failure`
**11/11**套件通过；`git diff --check`通过；简体/香港繁体/台湾繁体 Qt 翻译资源编译成功。
仓储测试使用 QTemporaryDir，关闭云同步；只测试实体所需迁移，非全应用数据库/服务端到端测试。
SQL connection 重新打开时存在原有非致命 duplicate connection 警告，本轮未改数据库全局生命周期。

最终打包保留已验证 Cocoa 修补插件，签名及复制前后哈希验证通过，已安装至
`/Users/ac/Applications/Fincept Local.app`。回滚包：
`/Users/ac/workspace/projects/FinceptTerminal-dev/backups/FinceptLocal-before-portfolio.bundle-backup`。
主程序 SHA256：`1b72fb2243ba8ea823ebca55b5b9c60eceab45455cfad44f8e1251760dff2c2d`。
Cocoa 插件 SHA256：`3205d0df9f5c4cecbb8416652d7f01e7a2a7fb19c6c00062322ecf21a82577fb`。

实际界面验证：启动初期 AX 读取超时，随后仪表盘已加载12项全球指数；组合空工作区正常。
打开导入窗口，确认“向现有组合添加持仓和交易历史”及重复导入中文提示，随后取消；未添加真实持仓。
Command-Q 后主进程已退出，对比本轮前后 Fincept 崩溃报告无新增；随后重新打开应用。
GUI 实际持仓/故障报价/JSON往返尚未在独立应用 profile 上验证；CSV文件流程及真实历史NAV重建也未在本轮验收。
^GSPC 局部 TLS 错误、异步工具超时、只读 CLI 与标准 MCP 仍是后续工作，不标为已完成。


### Yahoo 指数取数容错与异步生命周期（2026-10-07）

- 研究页单标的报价改从价格历史取数，不再把公司资料接口作为报价成功的前置条件。过滤非有限/非正收盘价；只有一根有效日线且没有有效前收盘价时，涨跌明确显示不可用，不编造零涨跌。
- 单标的报价、公司资料与周期历史遇到特定临时连接错误时最多重试一次，并重新创建 Ticker；无效代码、限流和证书校验失败不重试。保留现有代理与 TLS 校验；单次 HTTP timeout 配置拒绝非有限和非正值。研究服务暴露失败输出，便于定位局部取数错误。
- 旧异步等待明确返回完成状态。排队启动和回调访问调用者存储前必须取得生命周期锁；超时/取消/目标销毁后迟到回调失效，防止栈释放后的访问。现有调用点检查失败并返回工具错误；修复原先未转发参数的 typed callback bridge。
- 真异步工具的结果、超时、取消、异常共用一次性完成门；结果完成后清理 watchdog/取消轮询定时器。AsyncDispatch 在服务目标销毁或目标线程抛异常时完成失败；已结束调用不再启动排队工作。
- 这些机制结束等待并拦截迟到回调，不保证强制终止底层服务工作或抢占正在执行的同步函数；全工具业务正确性仍需逐项验收。

验证：完整 `dev build` 成功；CTest **12/12** 套件通过（新增 Qt Core 生命周期叶子测试，不启动真实应用数据库）；Python Yahoo 故障回归 **8/8** 通过，覆盖临时 TLS 恢复、重试耗尽、非重试错误、缺失价格与前收盘、配置边界。`git diff --check` 通过。

使用现有本地 HTTP 代理实际请求 ^GSPC，改动后 quote/info/1M history 均成功（约1.66/0.94/0.29秒，历史21根）。本轮改动前请求也成功，未重现原始 TLS 错误；因此记录为容错改善，未认定根因已消除。

已签名打包并安装 `/Users/ac/Applications/Fincept Local.app`；安装前后主程序与 Cocoa 插件哈希一致，打包的 yfinance_data.py 与仓库一致。保留此前退出崩溃修补插件。回滚包：
`/Users/ac/workspace/projects/FinceptTerminal-dev/backups/FinceptLocal-before-network.bundle-backup`。
主程序 SHA256：`7cf859c1b61648bb44e8367ccda9fee398267a5f1fa75583af43a5a82aa94467`。

实际界面验收（12:18）：^GSPC 报价/成交量/52周区间显示，切换1M周期后日K图绘出，页面没有持续Loading或错误弹窗。指数不适用的公司估值项目仍显示“无”；未核对外部价格精度，不将该观察当作全模块网络验收。Command-Q 后主进程消失，没有新增Fincept崩溃报告；随后重新启动。未添加持仓、下单或修改凭据。

下一阶段：只读 CLI（数据源状态、报价、历史、组合估值查询、结构化 JSON 输出），之后再评估标准 MCP 接入。
