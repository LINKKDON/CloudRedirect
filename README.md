> **维护声明**：原作者已删除上游仓库（`Selectively11/CloudRedirect`）。本仓库最初作为个人留档保存了上游代码，现由本人基于该存档**自行二次维护**——持续适配 Windows 版本 Steam 更新、修复问题并迭代功能，主分支不再保持上游原样。
>
> **Maintenance Notice**: The original author has deleted the upstream repository (`Selectively11/CloudRedirect`). This repository was initially kept as a personal archive of the upstream code, and is now independently maintained by me based on that archive—continuously adapting to Windows version Steam updates, fixing issues, and iterating on features. The main branch no longer remains identical to the original upstream.

# CloudRedirect

""Steam Cloud"" for 'lua' games.

**This the only official source for CloudRedirect. Any other websites are not operated by me or are otherwise endorsed by this project. At least one of those fake sites was actively distributing malware!**

> ****This software is experimental and under active development.**** The underlying techniques are fairly insane. What this software tries to do is nuts to attempt. This software could damage your save files and probably will! It could overwrite your saves, cause weird conflicts, make your saves disappear, make you cry. Back up any saves you care about before using this software.

But it probably won't. It's been very solid for a long time.

>****Do not use this if you do not actively want cloud saves for "lua" games. If all you care about is the Steam Cloud error, disable Steam Cloud in properties for that game.****

## What it does

First, some back story:

Once upon a time, there was a tool called SteamTools. SteamTools supported cloud saving, kind of.

Later, Valve patched the (sinful) thing SteamTools did to sync saves. Specifically, SteamTools rewrote requests to AppID 760, which is Steam Screenshots. It sent all Steam Cloud requests for non-owned AppIDs there. It did not create prefixes for each individual game, which means that each lua app shared the saves with all others. This can cause saves to conflict if multiple games use the same save file name. This also means that your saves are replicated in the `Steam/Userdata/<steamid>/<appid for lua game>` folder for each lua app.

It also did not support Steam AutoCloud games at all. It would simply show a fake success message for those games.

What _this_ tool does is redirect Steam Cloud requests for games that are injected to Google Drive/OneDrive/a local folder, including AutoCloud games. Everything is native inside the Steam Client, but the actual data is read/written to and from your cloud account. This was much harder to do than just redirecting read/write to an AppID that your account owns, but it was fun to make. It also is less likely to piss off Valve.

## How it works

CloudRedirect for Windows consists of a C++ DLL and a WPF companion app:

1. The companion app is used to copy the DLL to your Steam folder and let the user login to their cloud provider.
2. The DLL hooks Steam's internal cloud save RPC handlers via ~~vtable interception~~ black magic.
3. When a lua game attempts to read or write cloud save data, the DLL intercepts the calls and redirects it. If the game is owned, the game uses normal Steam Cloud as expected. If a lua is present that only unlocks DLC, the game will use normal Steam Cloud.
4. More dark magic occurs. Saves sync. Bytes flow. This all is visible in the Steam UI and looks identical to normal Steam Cloud functionality.

## Supported cloud providers

- **Google Drive**
- **OneDrive**
- **Cloudflare R2**
- **S3-Compatible** - AWS S3, MinIO, Backblaze B2, Wasabi, DigitalOcean Spaces, and any self-hosted S3 server.
- **Local folder / mapped drive** - by request of literally one user.
- **WebDAV** - 通用 WebDAV 服务端（Nextcloud/ownCloud、Alist/OpenList、坚果云、群晖、IIS、Apache mod_dav 等），支持 Basic/Digest/NTLM/Negotiate/Bearer/匿名与自签证书。

With more to come over time. 

### WebDAV

通用 WebDAV 支持，不做特定服务端适配；认证覆盖 Basic、Digest、NTLM、Negotiate、Bearer、匿名，传输覆盖 HTTPS、自签证书、内网 HTTP、自定义端口与系统代理。

本仓库只维护 Windows DLL，WebDAV 的配置入口由宿主（Fluent-Steam-Lua）的云存档页写入 `%APPDATA%\CloudRedirect\`：

- `config.json`：`"provider": "webdav"`，`token_path` 指向凭据文件（留空时回退到 `webdav_credentials.json`）。
- `webdav_credentials.json` 示例：

```json
{
  "url": "https://dav.example.com/remote.php/dav/files/user/",
  "username": "user",
  "password": "app-password",
  "auth": "basic",
  "key_prefix": "",
  "allow_insecure_http": false,
  "allow_insecure_tls": false,
  "timeout_sec": 0,
  "extra_headers": []
}
```

字段说明：

- `url`：目标集合的完整地址（WebDAV 服务器或用户目录）；工具会自动在其下创建 `CloudRedirect` 目录，若 URL 已以 `/CloudRedirect` 结尾则直接使用。
- `auth`：`basic`（默认，预发送）、`auto`（401 协商，自动选择 Digest/NTLM/Negotiate）、`digest`、`ntlm`、`negotiate`、`bearer`（配合 `bearer_token`）、`none`（匿名）。
- `key_prefix`：base 下的可选子目录，含空格/中文会自动编码。
- `allow_insecure_tls`：跳过证书校验（自签服务端）。URL 显式写成 `http://` 时即视为允许明文并记录警告；`allow_insecure_http` 仅为与其它 provider 的配置字段保持一致而保留。
- `timeout_sec`：单请求超时秒数，0 表示传输层默认值（最大 300）。
- `extra_headers`：附加请求头，例如 IIS 的 `Translate: f`。

凭据文件可直接写明文 JSON，DLL 首次读取时会用 DPAPI 加密回写；修改配置后需重启 Steam。部分服务端（如 IIS）需要自行启用 WebDAV 发布并调整请求体大小限制。

## Usage (Windows)

Grab the latest release from the [Releases page](https://github.com/huanyuejue/CloudRedirect/releases).

Run the EXE. Pick your mode - STfixer mode for fixes to ST bugs, CloudRedirect mode for the good stuff. In Setup, hit 'Run All Patches'. Go to the Cloud Provider tab, select your provider. If it is a cloud provider, sign in to it.

That's it. Go launch Steam and watch the magic.

## Building from source (Windows)

### Prerequisites

- Visual Studio 2022 (or Build Tools) with the C++ and .NET 8 workloads
- CMake 3.20+

### Build

```bash
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

This builds both the C++ DLL (`build/Release/cloud_redirect.dll`) and publishes the WPF app (`ui/bin/publish/CloudRedirect.exe`). The DLL is automatically embedded into the executable.

Or don't build it? Building Windows apps is pain.

## Contibuting

CloudRedirect is open source. I welcome PRs. I love PRs.

I don't _love_ downstream forks. You have a right to fork and do what you want, but if you make something useful, if you fix issues...I'd much prefer they be sent upstream! Help me help you! If you identify a problem/add a provider/improve the tool in any way, please consider submitting your changes as a PR, rather than keeping them in a downstream fork.
