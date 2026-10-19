#pragma once
// 通用 WebDAV provider（RFC 4918）。我按"服务端无关"来设计：
// Basic/Bearer 直接预发送，Digest/NTLM/Negotiate 交给 WinHTTP 的 401 协商；
// 目录列举用 PROPFIND Depth:1 递归 BFS，解析时不依赖 XML 命名空间前缀。
#include "cloud_provider.h"
#include "http_util.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class IHttpTransport;

class WebDavProvider : public ICloudProvider {
public:
    const char* Name() const override { return "WebDAV"; }

    bool Init(const std::string& configPath) override;
    void Shutdown() override;
    bool IsAuthenticated() const override;

    bool Upload(const std::string& path, const uint8_t* data, size_t len) override;
    bool Download(const std::string& path, std::vector<uint8_t>& outData) override;
    bool Remove(const std::string& path) override;
    ExistsStatus CheckExists(const std::string& path) override;
    std::vector<FileInfo> List(const std::string& prefix) override;
    bool ListChecked(const std::string& prefix, std::vector<FileInfo>& outFiles,
                     bool* outComplete = nullptr) override;

private:
    // 用 base + key_prefix + 编码后的相对路径拼出完整 URL；集合补尾斜杠。
    std::string BuildUrl(const std::string& relPath, bool collection) const;

    // 发请求并跟随同源重定向。鉴权由传输层统一处理（Basic/Bearer 预发送，
    // 其余走 401 协商），这里只负责路径与额外请求头。
    HttpUtil::HttpResp Request(const char* method, const std::string& relPath,
                               bool collection, const std::string& body,
                               const std::vector<std::string>& extraHeaders);
    // 直接对完整 URL 发请求（建目录时需要先确保 base 集合本身存在）。
    HttpUtil::HttpResp RequestUrlInternal(const char* method, const std::string& url,
                                          const std::string& body,
                                          const std::vector<std::string>& extraHeaders);
    // 对网络错误/429/5xx 做有限重试；PUT/PROPFIND 本身幂等，可安全重放。
    HttpUtil::HttpResp SendWithRetry(const char* method, const std::string& relPath,
                                     bool collection, const std::string& body,
                                     const std::vector<std::string>& extraHeaders);
    // 完整 URL 版本，建目录时用它探测 base 是否已存在。
    HttpUtil::HttpResp SendWithRetryUrl(const char* method, const std::string& url,
                                        const std::string& body,
                                        const std::vector<std::string>& extraHeaders);

    HttpUtil::HttpResp PropFindAt(const std::string& relPath, bool collection, int depth);
    HttpUtil::HttpResp PropFindUrl(const std::string& url, int depth);
    // 解析 multistatus：按本地名匹配标签，兼容任意命名空间前缀；
    // 文件路径为 base 相对路径，子目录去掉尾斜杠。
    bool ParseMultiStatus(const std::string& xml, const std::string& selfDir,
                          std::vector<FileInfo>& outFiles,
                          std::vector<std::string>& outSubdirs) const;

    bool EnsureCollections(const std::string& relDir);
    bool EnsureParentCollections(const std::string& relFile);

    // 把 PROPFIND 返回的 href 还原成 base 相对路径：
    // 依次剥离 scheme/host、base 路径和 key_prefix，并做百分号解码。
    std::string RelativeFromHref(const std::string& href) const;

    // 初始化时做一次 OPTIONS 探测：记录 DAV/Allow，401 时标记鉴权失败；
    // 网络错误不阻断初始化，等真正读写时再暴露。
    void ProbeCapabilities();

    std::unique_ptr<IHttpTransport> m_transport;

    std::string m_baseUrl;           // scheme://authority/path/（带尾斜杠）
    std::string m_basePathDecoded;   // base 路径解码值（用于匹配 href）
    std::string m_keyPrefix;         // 原样子目录前缀（带尾斜杠，可为空）
    std::string m_keyPrefixEncoded;  // 编码后的前缀（拼 URL 用）
    std::string m_keyPrefixDecoded;  // 解码后的前缀（剥 href 用）
    std::vector<std::string> m_extraHeaders;

    std::atomic<bool> m_ready{false};
    std::atomic<bool> m_authFailed{false};

    // MKCOL 结果的进程内缓存：并发 worker 不重复建同一目录。
    std::mutex m_dirMutex;
    std::set<std::string> m_knownDirs;
};
