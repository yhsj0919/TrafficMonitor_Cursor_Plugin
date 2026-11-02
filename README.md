# CursorUsage TrafficMonitor 插件

这是一个 TrafficMonitor 插件，用于读取本机 Cursor 登录会话，并从 Cursor 用量接口获取并显示剩余用量。

## 显示项目

- `Cursor User`：当前登录账号邮箱（来自本地 `cursorAuth/cachedEmail`）。
- `Cursor Models Remaining`：Cursor Models（含 Grok / Composer 等）用量窗口的剩余百分比。
- `Other Models Remaining`：Other Models 用量窗口的剩余百分比。

鼠标提示会显示用户、剩余百分比、已用百分比、重置倒计时、套餐信息，或最近一次获取失败的错误信息。用量接口失败时，仍会尽量显示本地读到的用户邮箱。

## 数据来源

插件选项中的访问令牌路径留空时，按顺序自动查找（找到可用令牌即停止；自定义路径不回退）：

1. `%APPDATA%\Cursor\User\globalStorage\state.vscdb`（Cursor IDE）
2. `%APPDATA%\Cursor\auth.json`（Cursor CLI / agent login）
3. `%USERPROFILE%\.cursor\auth.json`

`state.vscdb` 读取 SQLite `ItemTable` 键：

- `cursorAuth/accessToken`
- `cursorAuth/refreshToken`（过期时尝试刷新，不写回数据库）
- `cursorAuth/cachedEmail`
- `cursorAuth/stripeMembershipType`

`auth.json` 读取 `accessToken` / `refreshToken`（也兼容嵌套的 `tokens.access_token`）。也可在选项中手动指定上述任一文件的完整路径。

随后调用：

`POST https://api2.cursor.sh/aiserver.v1.DashboardService/GetCurrentPeriodUsage`

解析其中的 `planUsage.autoPercentUsed`（Cursor Models）与 `planUsage.apiPercentUsed`（Other Models）。该接口为未文档化接口，可能随 Cursor 更新变化。

## 插件选项

在 TrafficMonitor 的插件管理中选中 **Cursor Usage**，打开插件选项：

- **访问令牌路径**：填写 `state.vscdb` 或令牌 JSON 的完整文件路径，也可点击“浏览”选择文件；留空恢复默认路径。
- **系统代理（自动读取）**：选择后自动使用 Windows 的系统及当前用户代理设置，支持自动检测和 PAC 自动配置。修改系统代理后，后续刷新会读取新设置。
- **直连（不使用代理）**：默认模式，忽略系统代理，直接连接用量接口。
- **手动代理**：填写 HTTP 代理地址，如 `127.0.0.1:7890`、`http://127.0.0.1:7890` 或 `[::1]:7890`。HTTPS 用量请求通过 HTTP CONNECT 隧道传输。不支持 SOCKS、HTTPS 代理端点或带账号密码的代理地址。

点击“确定”后，设置保存到 `CursorUsage.dll` 同目录的 `CursorUsage.ini`，自动重新获取用量；如果旧请求仍在进行，会在旧请求结束后的下一次轮询使用新设置。点击“取消”不保存修改。插件正常情况下每分钟刷新一次。

配置文件只保存令牌文件的路径，不保存令牌内容。请确保插件目录可写；保存失败时会显示错误，并保留原配置。自定义路径不展开 `%变量%` 或 `~`，建议使用完整路径。

## 安装

将生成的 `CursorUsage.dll` 复制到 TrafficMonitor 的 `plugins` 目录，然后重启 TrafficMonitor，并在插件管理或显示项目设置中启用这两个显示项目。

插件与 TrafficMonitor 的架构需要匹配。令牌过期或缺少 `cursorAuth/accessToken` 时，需要先在 Cursor 中重新登录。构建、测试和排错方法见 [开发说明](./Development.md)。

## LICENSE

[MIT LICENSE](./LICENSE)
