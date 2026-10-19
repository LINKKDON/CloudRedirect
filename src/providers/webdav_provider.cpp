// 通用 WebDAV provider。我刻意不做特定服务端适配，而是按协议行为兼容：
// - PROPFIND 结果按标签本地名扫描，命名空间前缀任意（D:/d:/lp1:/无前缀）
// - href 兼容绝对 URL、绝对路径、相对路径与百分号编码
// - 认证走共同路径：Basic/Bearer 预发送，Digest/NTLM/Negotiate 交给 WinHTTP
#include "webdav_provider.h"

#include "cloud_provider_base.h"
#include "http_util.h"
#include "json.h"
#include "log.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <set>
#include <thread>

namespace {

// 列举保护阈值：防止异常服务端返回无限层级/条目把内存打满。
constexpr size_t kMaxDirsPerList = 100000;
constexpr size_t kMaxFilesPerList = 500000;

// 只请求真正需要的属性，显式列出比 allprop 更兼容（部分服务端 allprop 很慢）。
const char* kPropFindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<D:propfind xmlns:D=\"DAV:\"><D:prop>"
    "<D:resourcetype/><D:getcontentlength/><D:getlastmodified/>"
    "</D:prop></D:propfind>";

std::string ToLower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char)s[a] <= ' ') ++a;
    while (b > a && (unsigned char)s[b - 1] <= ' ') --b;
    return s.substr(a, b - a);
}

// 只接受 http(s)://authority[/path]；URL 里带用户信息直接拒绝，避免密码进日志。
bool SplitUrl(const std::string& url, std::string& scheme, std::string& authority,
              std::string& path) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    scheme = ToLower(url.substr(0, p));
    if (scheme != "http" && scheme != "https") return false;
    size_t hostStart = p + 3;
    size_t pathStart = url.find('/', hostStart);
    if (pathStart == std::string::npos) {
        authority = url.substr(hostStart);
        path = "/";
    } else {
        authority = url.substr(hostStart, pathStart - hostStart);
        path = url.substr(pathStart);
    }
    size_t aq = authority.find_first_of("?#");
    if (aq != std::string::npos) authority = authority.substr(0, aq);
    if (authority.empty()) return false;
    if (authority.find('@') != std::string::npos) return false;
    size_t q = path.find_first_of("?#");
    if (q != std::string::npos) path = path.substr(0, q);
    return true;
}

// 把 Location 解析成绝对 URL。支持绝对地址、绝对路径和相对路径，
// 并折叠 "."/".." 段；只用于同源跳转，跨源由调用方拒绝。
std::string ResolveUrl(const std::string& baseUrl, const std::string& location) {
    std::string loc = Trim(location);
    if (loc.empty()) return {};
    if (loc.find("://") != std::string::npos) return loc;

    std::string scheme, authority, path;
    if (!SplitUrl(baseUrl, scheme, authority, path)) return {};

    std::string merged;
    if (loc[0] == '/') {
        merged = loc;
    } else {
        size_t slash = path.rfind('/');
        merged = (slash == std::string::npos) ? "/" : path.substr(0, slash + 1);
        merged += loc;
    }

    size_t q = merged.find_first_of("?#");
    std::string suffix = (q == std::string::npos) ? "" : merged.substr(q);
    if (q != std::string::npos) merged = merged.substr(0, q);

    std::vector<std::string> parts;
    size_t i = 0;
    while (i < merged.size()) {
        size_t j = merged.find('/', i);
        std::string seg = merged.substr(i, (j == std::string::npos ? merged.size() : j) - i);
        if (seg == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!seg.empty() && seg != ".") {
            parts.push_back(seg);
        }
        if (j == std::string::npos) break;
        i = j + 1;
    }
    std::string out;
    for (auto& s : parts) {
        out += "/";
        out += s;
    }
    if (out.empty()) out = "/";
    return scheme + "://" + authority + out + suffix;
}

std::string XmlUnescape(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '&') {
            if (s.compare(i, 5, "&amp;") == 0)  { out.push_back('&');  i += 4; continue; }
            if (s.compare(i, 4, "&lt;") == 0)   { out.push_back('<');  i += 3; continue; }
            if (s.compare(i, 4, "&gt;") == 0)   { out.push_back('>');  i += 3; continue; }
            if (s.compare(i, 6, "&quot;") == 0) { out.push_back('"');  i += 5; continue; }
            if (s.compare(i, 6, "&apos;") == 0) { out.push_back('\''); i += 5; continue; }
        }
        out.push_back(s[i]);
    }
    return out;
}

// 民用日期转 1970 起的整数天数，避开 timegm/mktime 的时区与平台差异。
int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// RFC 1123 / IMF-fixdate（"Sun, 06 Nov 1994 08:49:37 GMT"）。
// 解析失败返回 0：修改时间只用于展示，不影响同步正确性。
int64_t Rfc1123ToUnix(const std::string& in) {
    std::string s = Trim(in);
    size_t comma = s.find(',');
    if (comma != std::string::npos) s = Trim(s.substr(comma + 1));
    size_t digit = s.find_first_of("0123456789");
    if (digit == std::string::npos) return 0;
    s = s.substr(digit);

    char mon[8] = {};
    int day = 0, year = 0, hh = 0, mm = 0, ss = 0;
    if (sscanf(s.c_str(), "%d %7s %d %d:%d:%d", &day, mon, &year, &hh, &mm, &ss) != 6)
        return 0;
    static const char* months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int m = 0;
    for (int i = 0; i < 12; ++i) {
        if (strncmp(mon, months[i], 3) == 0) { m = i + 1; break; }
    }
    if (!m || year < 1970 || day < 1 || day > 31) return 0;
    int64_t days = DaysFromCivil(year, (unsigned)m, (unsigned)day);
    return days * 86400 + hh * 3600 + mm * 60 + ss;
}

struct XmlElement {
    size_t innerStart = 0;
    size_t innerEnd = 0;
    bool selfClosing = false;
};

// 找到下一个本地名匹配的开始标签。这里不做完整 XML 解析，
// 只按标签名扫描；prefix 任意、属性任意，够解析 multistatus 用。
bool FindElement(const std::string& xml, size_t from, const char* localName, XmlElement& out) {
    size_t p = from;
    while (p < xml.size()) {
        size_t lt = xml.find('<', p);
        if (lt == std::string::npos || lt + 1 >= xml.size()) return false;
        size_t i = lt + 1;
        if (xml[i] == '!' || xml[i] == '?') { // 注释 / 声明 / DOCTYPE
            size_t gt = xml.find('>', i);
            if (gt == std::string::npos) return false;
            p = gt + 1;
            continue;
        }
        if (xml[i] == '/') { // 闭合标签
            size_t gt = xml.find('>', i);
            if (gt == std::string::npos) return false;
            p = gt + 1;
            continue;
        }
        size_t nameStart = i;
        while (i < xml.size() && xml[i] != '>' && xml[i] != '/' &&
               !isspace((unsigned char)xml[i])) ++i;
        std::string fullName = xml.substr(nameStart, i - nameStart);
        std::string local = fullName;
        size_t colon = local.find(':');
        if (colon != std::string::npos) local = local.substr(colon + 1);

        size_t gt = xml.find('>', i);
        if (gt == std::string::npos) return false;
        bool selfClosing = (gt > 0 && xml[gt - 1] == '/');

        if (local != localName) {
            p = gt + 1;
            continue;
        }
        if (selfClosing) {
            out.innerStart = out.innerEnd = gt + 1;
            out.selfClosing = true;
            return true;
        }
        // 用完整标签名找闭合标签，避免把同名前缀的其他元素误配。
        std::string closeTag = "</" + fullName + ">";
        size_t close = xml.find(closeTag, gt + 1);
        if (close == std::string::npos) return false;
        out.innerStart = gt + 1;
        out.innerEnd = close;
        out.selfClosing = false;
        return true;
    }
    return false;
}

bool ElementInner(const std::string& xml, size_t from, const char* localName, std::string& out) {
    XmlElement e;
    if (!FindElement(xml, from, localName, e)) return false;
    out = xml.substr(e.innerStart, e.innerEnd - e.innerStart);
    return true;
}

bool ContainsLocalTag(const std::string& xml, const char* localName) {
    XmlElement e;
    return FindElement(xml, 0, localName, e);
}

std::string TrimTrailingSlashes(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

std::string TrimLeadingSlashes(std::string s) {
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    return s;
}

// 与其他 provider 的远端根约定一致：URL 未显式指向 CloudRedirect 时自动补一层，
// 避免账号目录直接散落在 WebDAV 根目录。
bool IsCloudRedirectSegment(const std::string& s) {
    static const char kRoot[] = "CloudRedirect";
    const size_t n = sizeof(kRoot) - 1;
    if (s.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        if (tolower((unsigned char)s[i]) != tolower((unsigned char)kRoot[i])) return false;
    }
    return true;
}

// 这些状态说明请求可能只是暂时失败，重试一次往往就能过。
bool RetryableStatus(int status) {
    return status == 0 || status == 408 || status == 429 ||
           status == 500 || status == 502 || status == 503 || status == 504;
}

const char* AuthName(ServerAuthScheme s) {
    switch (s) {
    case ServerAuthScheme::Basic:      return "basic";
    case ServerAuthScheme::Bearer:     return "bearer";
    case ServerAuthScheme::Negotiated: return "auto";
    case ServerAuthScheme::Digest:     return "digest";
    case ServerAuthScheme::Ntlm:       return "ntlm";
    case ServerAuthScheme::Negotiate:  return "negotiate";
    default:                           return "none";
    }
}

} // namespace

bool WebDavProvider::Init(const std::string& configPath) {
    if (configPath.empty()) {
        LOG("[WebDAV] Init failed: empty credentials path");
        return false;
    }
    auto store = CreateTokenStore();
    if (!store) return false;
    std::string json = store->Read(configPath);
    if (json.empty()) {
        LOG("[WebDAV] Init failed: cannot read '%s'", configPath.c_str());
        return false;
    }

    Json::Value root = Json::Parse(json);
    if (root.type != Json::Type::Object) {
        LOG("[WebDAV] Init failed: credentials file is not a JSON object");
        return false;
    }

    std::string url = Trim(root["url"].str());
    std::string urlScheme, authority, rawPath;
    if (!SplitUrl(url, urlScheme, authority, rawPath)) {
        LOG("[WebDAV] invalid url '%s' (expected http(s)://host[/path])", url.c_str());
        return false;
    }

    // 路径归一化：折叠重复斜杠，补齐首尾斜杠，集合 URL 始终以 '/' 结尾。
    std::string normPath;
    bool lastSlash = false;
    for (char c : rawPath) {
        if (c == '/') {
            if (!lastSlash) normPath.push_back('/');
            lastSlash = true;
        } else {
            normPath.push_back(c);
            lastSlash = false;
        }
    }
    if (normPath.empty() || normPath.front() != '/') normPath.insert(normPath.begin(), '/');
    if (normPath.back() != '/') normPath.push_back('/');

    // 远端根统一落在 CloudRedirect 子目录下；URL 已指向 .../CloudRedirect 时保持不变，
    // 避免出现 CloudRedirect/CloudRedirect 的重复嵌套。
    {
        std::string decoded = HttpUtil::UrlDecode(normPath);
        std::string trimmed = decoded;
        while (trimmed.size() > 1 && trimmed.back() == '/') trimmed.pop_back();
        size_t lastSlash = trimmed.find_last_of('/');
        std::string lastSegment = (lastSlash == std::string::npos)
            ? trimmed : trimmed.substr(lastSlash + 1);
        if (!IsCloudRedirectSegment(lastSegment))
            normPath += "CloudRedirect/";
    }

    m_baseUrl = urlScheme + "://" + authority + normPath;
    m_basePathDecoded = HttpUtil::UrlDecode(normPath);

    std::string keyPrefix = Trim(root["key_prefix"].str());
    keyPrefix = TrimLeadingSlashes(keyPrefix);
    while (!keyPrefix.empty() && keyPrefix.back() == '/') keyPrefix.pop_back();
    if (!keyPrefix.empty()) keyPrefix.push_back('/');
    m_keyPrefix = keyPrefix;
    m_keyPrefixEncoded = HttpUtil::UrlEncode(keyPrefix, true);
    m_keyPrefixDecoded = HttpUtil::UrlDecode(keyPrefix);

    m_extraHeaders.clear();
    const Json::Value& extra = root["extra_headers"];
    if (extra.type == Json::Type::Array) {
        for (const auto& v : extra.arrVal) {
            if (v.type != Json::Type::String) continue;
            std::string h = Trim(v.str());
            // 头值里的换行会把一行拆成多行，直接丢弃而不是透传给 WinHTTP。
            if (h.empty() || h.find('\r') != std::string::npos ||
                h.find('\n') != std::string::npos) continue;
            m_extraHeaders.push_back(h);
        }
    }

    std::string username = root["username"].str();
    std::string password = root["password"].str();
    std::string bearer = Trim(root["bearer_token"].str());
    std::string auth = ToLower(Trim(root["auth"].str()));

    // 默认策略：填了 bearer 用 Bearer，填了用户名用预发送 Basic（兼容面最大、
    // 少一次 401 往返）；需要 Digest/NTLM 等服务端协商时显式写 auto 或具体方案。
    ServerAuthScheme scheme;
    if (auth.empty()) {
        if (!bearer.empty())        scheme = ServerAuthScheme::Bearer;
        else if (!username.empty()) scheme = ServerAuthScheme::Basic;
        else                        scheme = ServerAuthScheme::None;
    } else if (auth == "basic") {
        scheme = ServerAuthScheme::Basic;
    } else if (auth == "bearer") {
        scheme = ServerAuthScheme::Bearer;
    } else if (auth == "auto" || auth == "negotiated") {
        scheme = ServerAuthScheme::Negotiated;
    } else if (auth == "digest") {
        scheme = ServerAuthScheme::Digest;
    } else if (auth == "ntlm") {
        scheme = ServerAuthScheme::Ntlm;
    } else if (auth == "negotiate") {
        scheme = ServerAuthScheme::Negotiate;
    } else if (auth == "none") {
        scheme = ServerAuthScheme::None;
    } else {
        LOG("[WebDAV] unknown auth mode '%s', using basic", auth.c_str());
        scheme = ServerAuthScheme::Basic;
    }

    if (scheme == ServerAuthScheme::Bearer && bearer.empty()) {
        LOG("[WebDAV] auth=bearer but bearer_token is empty; continuing anonymously");
        scheme = ServerAuthScheme::None;
    }
    if (scheme != ServerAuthScheme::None && scheme != ServerAuthScheme::Bearer &&
        username.empty()) {
        LOG("[WebDAV] auth mode %s needs a username; continuing anonymously", AuthName(scheme));
        scheme = ServerAuthScheme::None;
    }

    int64_t timeout = 0;
    if (root["timeout_sec"].type == Json::Type::Number)
        timeout = root["timeout_sec"].integer();
    else if (root["timeout_sec"].type == Json::Type::String)
        timeout = atoll(root["timeout_sec"].str().c_str());
    timeout = (std::max)((int64_t)0, (std::min)((int64_t)300, timeout));

    m_transport = CreateHttpTransport("[WebDAV]");
    if (!m_transport || !m_transport->Init()) {
        LOG("[WebDAV] transport init failed");
        return false;
    }
    TransportOptions opts;
    opts.allowInsecureHttp = (urlScheme == "http") ||
                             root["allow_insecure_http"].boolean();
    opts.allowInsecureTls = root["allow_insecure_tls"].boolean();
    // 重定向必须由我控制：跟随 3xx 时要保证不把凭据带到别的源。
    opts.disableRedirects = true;
    opts.timeoutSec = (int)timeout;
    opts.authScheme = scheme;
    opts.username = username;
    opts.password = password;
    opts.bearerToken = bearer;
    m_transport->SetOptions(opts);

    m_ready.store(true);
    if (urlScheme == "http")
        LOG("[WebDAV] WARNING: plaintext HTTP endpoint (%s)", m_baseUrl.c_str());
    LOG("[WebDAV] initialized: %s auth=%s prefix='%s' timeout=%llds",
        m_baseUrl.c_str(), AuthName(scheme), m_keyPrefix.c_str(),
        (long long)timeout);
    ProbeCapabilities();
    return true;
}

void WebDavProvider::Shutdown() {
    m_ready.store(false);
    if (m_transport) m_transport->Shutdown();
    m_transport.reset();
}

bool WebDavProvider::IsAuthenticated() const {
    return m_ready.load() && !m_authFailed.load();
}

std::string WebDavProvider::BuildUrl(const std::string& relPath, bool collection) const {
    std::string rel = TrimLeadingSlashes(relPath);
    std::string url = m_baseUrl + m_keyPrefixEncoded + HttpUtil::UrlEncode(rel, true);
    if (collection && (url.empty() || url.back() != '/')) url.push_back('/');
    return url;
}

std::string WebDavProvider::RelativeFromHref(const std::string& hrefIn) const {
    std::string href = XmlUnescape(Trim(hrefIn));
    if (href.empty()) return {};
    size_t q = href.find_first_of("?#");
    if (q != std::string::npos) href = href.substr(0, q);
    size_t schemePos = href.find("://");
    if (schemePos != std::string::npos) {
        size_t slash = href.find('/', schemePos + 3);
        href = (slash == std::string::npos) ? "/" : href.substr(slash);
    }
    std::string decoded = HttpUtil::UrlDecode(href);

    // 服务端可能把 base 路径写成不带尾斜杠的形式，先按原样跳过自身。
    std::string baseNoSlash = TrimTrailingSlashes(m_basePathDecoded);
    if (!baseNoSlash.empty() && decoded == baseNoSlash) return {};

    size_t bpos = decoded.find(m_basePathDecoded);
    if (bpos != std::string::npos)
        decoded = decoded.substr(bpos + m_basePathDecoded.size());

    decoded = TrimLeadingSlashes(decoded);
    // 有些服务端返回集合自身的 href 时省略末尾斜杠；key_prefix 非空时先按
    // 无斜杠形式判断，避免把前缀目录本身当成子目录再递归一遍。
    if (!m_keyPrefixDecoded.empty() &&
        TrimTrailingSlashes(decoded) == TrimTrailingSlashes(m_keyPrefixDecoded))
        return {};
    if (!m_keyPrefixDecoded.empty() &&
        decoded.compare(0, m_keyPrefixDecoded.size(), m_keyPrefixDecoded) == 0)
        decoded = decoded.substr(m_keyPrefixDecoded.size());
    return decoded;
}

HttpUtil::HttpResp WebDavProvider::Request(const char* method, const std::string& relPath,
                                           bool collection, const std::string& body,
                                           const std::vector<std::string>& extraHeaders) {
    return RequestUrlInternal(method, BuildUrl(relPath, collection), body, extraHeaders);
}

HttpUtil::HttpResp WebDavProvider::RequestUrlInternal(const char* method, const std::string& urlIn,
                                                      const std::string& body,
                                                      const std::vector<std::string>& extraHeaders) {
    HttpUtil::HttpResp fail;
    if (!m_ready.load() || !m_transport) return fail;

    std::string url = urlIn;
    std::vector<std::string> hdrs = m_extraHeaders;
    hdrs.insert(hdrs.end(), extraHeaders.begin(), extraHeaders.end());

    std::string curMethod = method;
    std::string curBody = body;

    // 传输层已关闭自动重定向，这里只跟随同源跳转（最多 3 次），
    // 跨源一律拒绝，避免 Authorization 头泄露到第三方主机。
    for (int hop = 0; hop <= 3; ++hop) {
        HttpUtil::HttpResp r = m_transport->RequestUrl(curMethod.c_str(), url, curBody, hdrs);
        if (r.status < 300 || r.status >= 400 || r.location.empty()) return r;

        std::string next = ResolveUrl(url, r.location);
        if (next.empty()) return r;

        std::string s1, a1, p1, s2, a2, p2;
        if (!SplitUrl(url, s1, a1, p1) || !SplitUrl(next, s2, a2, p2) ||
            ToLower(s1) != ToLower(s2) || ToLower(a1) != ToLower(a2)) {
            LOG("[WebDAV] refusing cross-origin redirect to %s", next.c_str());
            return r;
        }
        LOG("[WebDAV] %s -> %d redirect to %s", curMethod.c_str(), r.status, next.c_str());
        if (r.status == 303) {
            curMethod = "GET";
            curBody.clear();
        }
        url = next;
    }
    LOG("[WebDAV] too many redirects for %s", urlIn.c_str());
    fail.status = 0;
    return fail;
}

HttpUtil::HttpResp WebDavProvider::SendWithRetry(const char* method, const std::string& relPath,
                                                 bool collection, const std::string& body,
                                                 const std::vector<std::string>& extraHeaders) {
    return SendWithRetryUrl(method, BuildUrl(relPath, collection), body, extraHeaders);
}

HttpUtil::HttpResp WebDavProvider::SendWithRetryUrl(const char* method, const std::string& url,
                                                    const std::string& body,
                                                    const std::vector<std::string>& extraHeaders) {
    HttpUtil::HttpResp r;
    for (int attempt = 0; attempt < 3; ++attempt) {
        r = RequestUrlInternal(method, url, body, extraHeaders);
        if (!RetryableStatus(r.status) || attempt == 2) return r;

        int delayMs = 250 * (1 << attempt);
        auto it = r.headers.find("retry-after");
        if (r.status == 429 || r.status == 503) {
            // 限流状态没有 Retry-After 时继续重试只会把配额打得更空，直接返回让上层稍后再试；
            // 只有服务端给出明确的短等待才补一次，且最多补一次，避免放大请求量。
            if (it == r.headers.end()) return r;
            int sec = atoi(it->second.c_str());
            if (sec <= 0 || sec > 10 || attempt >= 1) return r;
            delayMs = sec * 1000;
        } else if (it != r.headers.end()) {
            int sec = atoi(it->second.c_str());
            if (sec > 0 && sec <= 10) delayMs = sec * 1000;
        }
        LOG("[WebDAV] %s '%s' status %d, retry in %d ms", method,
            url.c_str(), r.status, delayMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
    }
    return r;
}

HttpUtil::HttpResp WebDavProvider::PropFindAt(const std::string& relPath, bool collection,
                                              int depth) {
    return PropFindUrl(BuildUrl(relPath, collection), depth);
}

HttpUtil::HttpResp WebDavProvider::PropFindUrl(const std::string& url, int depth) {
    std::vector<std::string> hdrs;
    hdrs.push_back(depth == 0 ? "Depth: 0" : "Depth: 1");
    hdrs.push_back("Content-Type: application/xml; charset=utf-8");
    return SendWithRetryUrl("PROPFIND", url, kPropFindBody, hdrs);
}

bool WebDavProvider::EnsureCollections(const std::string& relDirIn) {
    if (!m_ready.load()) return false;

    std::string rel = TrimLeadingSlashes(relDirIn);
    while (!rel.empty() && rel.back() == '/') rel.pop_back();

    // 建目录本身是幂等操作；锁住是为了避免并发 worker 重复 MKCOL 同一路径。
    std::lock_guard<std::mutex> lock(m_dirMutex);
    auto ensureUrl = [&](const std::string& cacheKey, const std::string& url) -> bool {
        if (m_knownDirs.count(cacheKey)) return true;
        HttpUtil::HttpResp r = RequestUrlInternal("MKCOL", url, "", {});
        // RFC 4918：新建返回 201，已存在通常返回 405；200/204 也一并接受。
        if (r.status == 200 || r.status == 201 || r.status == 204 || r.status == 405) {
            m_knownDirs.insert(cacheKey);
            return true;
        }
        // 坚果云等对已存在的集合返回 403/409 而不是 405；用 PROPFIND 确认后按已存在处理，
        // 只有确认不存在的才视为真正的失败（权限不足时 PROPFIND 也不会是 2xx）。
        if (r.status == 403 || r.status == 409) {
            HttpUtil::HttpResp p = PropFindUrl(url, 0);
            if (p.status == 207 || p.status == 200) {
                m_knownDirs.insert(cacheKey);
                return true;
            }
        }
        LOG("[WebDAV] MKCOL '%s' failed: status %d", url.c_str(), r.status);
        return false;
    };

    // 先确保配置的 base 集合本身存在（key_prefix 非空时它是父级），
    // 再确保 base+key_prefix。Nextcloud 这类固定端点对 MKCOL 返回 405，
    // 按"已存在"处理即可。
    if (!ensureUrl("@base", m_baseUrl)) return false;
    if (m_keyPrefix.empty()) {
        m_knownDirs.insert("");
    } else if (!ensureUrl("", BuildUrl("", true))) {
        return false;
    }

    // 自顶向下逐级创建，保证父集合先存在。
    size_t start = 0;
    while (start <= rel.size()) {
        size_t slash = rel.find('/', start);
        size_t end = (slash == std::string::npos) ? rel.size() : slash;
        if (end > start) {
            std::string prefix = rel.substr(0, end);
            prefix.push_back('/');
            if (!ensureUrl(prefix, BuildUrl(prefix, true))) return false;
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool WebDavProvider::EnsureParentCollections(const std::string& relFile) {
    size_t slash = relFile.rfind('/');
    std::string dir = (slash == std::string::npos) ? "" : relFile.substr(0, slash);
    return EnsureCollections(dir);
}

bool WebDavProvider::Upload(const std::string& path, const uint8_t* data, size_t len) {
    if (!m_ready.load()) return false;
    std::string body(reinterpret_cast<const char*>(data), len);

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (!EnsureParentCollections(path)) return false;
        HttpUtil::HttpResp r = SendWithRetry("PUT", path, false, body,
            {"Content-Type: application/octet-stream"});
        if (r.status == 200 || r.status == 201 || r.status == 204) return true;
        if (r.status == 409 && attempt == 0) {
            // 服务端找不到父集合：清掉缓存后从头重建一次。
            std::lock_guard<std::mutex> lock(m_dirMutex);
            m_knownDirs.clear();
            continue;
        }
        if (r.status == 423 && attempt == 0) {
            // 资源被其他客户端锁定，短暂等待后重试一次。
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        LOG("[WebDAV] PUT '%s' failed: status %d", path.c_str(), r.status);
        return false;
    }
    return false;
}

bool WebDavProvider::Download(const std::string& path, std::vector<uint8_t>& outData) {
    if (!m_ready.load()) return false;
    HttpUtil::HttpResp r = SendWithRetry("GET", path, false, "", {});
    if (r.status != 200) {
        // 409：父目录不存在（坚果云等），等价于文件不存在；不必按错误刷日志
        if (r.status != 404 && r.status != 409)
            LOG("[WebDAV] GET '%s' failed: status %d", path.c_str(), r.status);
        return false;
    }
    outData.assign(r.body.begin(), r.body.end());
    return true;
}

bool WebDavProvider::Remove(const std::string& path) {
    if (!m_ready.load()) return false;
    HttpUtil::HttpResp r = SendWithRetry("DELETE", path, false, "", {});
    // 409：父目录不存在，目标本来就不存在，按幂等成功处理
    if (r.status == 200 || r.status == 204 || r.status == 404 || r.status == 409) return true;
    LOG("[WebDAV] DELETE '%s' failed: status %d", path.c_str(), r.status);
    return false;
}

ICloudProvider::ExistsStatus WebDavProvider::CheckExists(const std::string& path) {
    if (!m_ready.load()) return ExistsStatus::Error;
    HttpUtil::HttpResp r = SendWithRetry("HEAD", path, false, "", {});
    if (r.status == 200 || r.status == 204) return ExistsStatus::Exists;
    if (r.status == 404 || r.status == 409) return ExistsStatus::Missing;
    // 有些服务端禁用 HEAD，退化成 Depth:0 的 PROPFIND 判断存在性。
    if (r.status == 405 || r.status == 501) {
        HttpUtil::HttpResp p = PropFindAt(path, false, 0);
        if (p.status == 207 || p.status == 200) return ExistsStatus::Exists;
        if (p.status == 404 || p.status == 409) return ExistsStatus::Missing;
    }
    if (r.status != 0 && r.status != 405 && r.status != 501)
        LOG("[WebDAV] HEAD '%s' failed: status %d", path.c_str(), r.status);
    return ExistsStatus::Error;
}

bool WebDavProvider::ParseMultiStatus(const std::string& xml, const std::string& selfDir,
                                      std::vector<FileInfo>& outFiles,
                                      std::vector<std::string>& outSubdirs) const {
    bool sawResponse = false;
    const std::string selfNoSlash = TrimTrailingSlashes(selfDir);
    size_t pos = 0;
    XmlElement resp;
    while (FindElement(xml, pos, "response", resp)) {
        sawResponse = true;
        std::string block = xml.substr(resp.innerStart, resp.innerEnd - resp.innerStart);
        pos = resp.innerEnd + 1;

        std::string href;
        if (!ElementInner(block, 0, "href", href)) continue;
        std::string rel = RelativeFromHref(href);
        if (rel.empty()) continue;

        std::string relNoSlash = TrimTrailingSlashes(rel);
        if (relNoSlash.empty() || relNoSlash == selfNoSlash) continue;
        // 防御性过滤：只接受当前子树内的条目（按路径边界比较）。
        if (!selfNoSlash.empty()) {
            if (relNoSlash.compare(0, selfNoSlash.size(), selfNoSlash) != 0) continue;
            if (relNoSlash.size() > selfNoSlash.size() &&
                relNoSlash[selfNoSlash.size()] != '/') continue;
        }

        bool isDir = false;
        std::string resType;
        if (ElementInner(block, 0, "resourcetype", resType))
            isDir = ContainsLocalTag(resType, "collection");
        if (!isDir && !rel.empty() && rel.back() == '/') isDir = true;

        if (isDir) {
            outSubdirs.push_back(relNoSlash);
            continue;
        }

        FileInfo fi;
        fi.path = relNoSlash;
        fi.size = 0;
        std::string sizeStr;
        if (ElementInner(block, 0, "getcontentlength", sizeStr))
            fi.size = strtoull(Trim(sizeStr).c_str(), nullptr, 10);
        fi.modifiedTime = 0;
        std::string modStr;
        if (ElementInner(block, 0, "getlastmodified", modStr))
            fi.modifiedTime = (uint64_t)Rfc1123ToUnix(Trim(modStr));
        outFiles.push_back(std::move(fi));
    }
    return sawResponse;
}

std::vector<ICloudProvider::FileInfo> WebDavProvider::List(const std::string& prefix) {
    std::vector<FileInfo> files;
    ListChecked(prefix, files, nullptr);
    return files;
}

bool WebDavProvider::ListChecked(const std::string& prefixIn, std::vector<FileInfo>& outFiles,
                                 bool* outComplete) {
    if (outComplete) *outComplete = false;
    outFiles.clear();
    if (!m_ready.load()) return false;

    std::string prefix = TrimLeadingSlashes(prefixIn);
    if (!prefix.empty() && prefix.back() != '/') prefix.push_back('/');

    // 递归列举：不用 Depth:infinity（多数服务端默认禁用），逐层 Depth:1。
    std::deque<std::string> queue;
    queue.push_back(prefix);
    std::set<std::string> visited;
    bool complete = true;
    size_t filesSeen = 0;

    while (!queue.empty()) {
        std::string dir = queue.front();
        queue.pop_front();
        if (!visited.insert(dir).second) continue;
        if (visited.size() > kMaxDirsPerList || filesSeen > kMaxFilesPerList) {
            LOG("[WebDAV] listing guard hit (dirs=%zu, files=%zu); marking incomplete",
                visited.size(), filesSeen);
            complete = false;
            break;
        }

        HttpUtil::HttpResp r = PropFindAt(dir, true, 1);
        if (r.status == 404 || r.status == 409) {
            if (visited.size() == 1) {
                // ǰ׺Ŀ¼�������ڣ����б�����׼ȷ��������ܱ��Ϊ��������
                if (outComplete) *outComplete = true;
                return true;
            }
            // 子目录被父级列出后突然消失：可能是被删了，也可能是权限/服务端异常。
            // 不能当作完整列举，否则上层 prune 会拿着缺项的清单做删除判断。
            LOG("[WebDAV] PROPFIND '%s' status %d; marking listing incomplete",
                dir.c_str(), r.status);
            complete = false;
            continue;
        }
        if (r.status != 207 && r.status != 200) {
            LOG("[WebDAV] PROPFIND '%s' failed: status %d", dir.c_str(), r.status);
            if (visited.size() == 1) return false;
            complete = false;
            continue;
        }

        std::vector<FileInfo> files;
        std::vector<std::string> subdirs;
        if (!ParseMultiStatus(r.body, dir, files, subdirs)) {
            LOG("[WebDAV] PROPFIND '%s': no <response> entries (body %zu bytes)",
                dir.c_str(), r.body.size());
            if (visited.size() == 1) return false;
            complete = false;
            continue;
        }

        filesSeen += files.size();
        for (auto& f : files) outFiles.push_back(std::move(f));
        for (auto& d : subdirs) queue.push_back(d);
    }

    if (outComplete) *outComplete = complete;
    return true;
}

void WebDavProvider::ProbeCapabilities() {
    HttpUtil::HttpResp r = SendWithRetry("OPTIONS", "", true, "", {});
    if (r.status == 200 || r.status == 204) {
        auto dav = r.headers.find("dav");
        auto allow = r.headers.find("allow");
        LOG("[WebDAV] OPTIONS ok (DAV=%s, Allow=%s)",
            dav != r.headers.end() ? dav->second.c_str() : "-",
            allow != r.headers.end() ? allow->second.c_str() : "-");
        if (allow != r.headers.end() && allow->second.find("PUT") == std::string::npos)
            LOG("[WebDAV] WARNING: server does not advertise PUT; uploads may fail");
        // OPTIONS 只能说明服务端可用，不能证明集合存在（部分服务端对任意路径都回 2xx）；
        // 用 PROPFIND Depth:0 确认，存在才写缓存，否则等首次上传时按需 MKCOL 创建。
        HttpUtil::HttpResp probe = PropFindUrl(m_baseUrl, 0);
        if (probe.status == 207 || probe.status == 200) {
            std::lock_guard<std::mutex> lock(m_dirMutex);
            m_knownDirs.insert("@base");
            if (m_keyPrefix.empty()) m_knownDirs.insert("");
            LOG("[WebDAV] root collection verified: %s", m_baseUrl.c_str());
        } else if (probe.status == 401) {
            m_authFailed.store(true);
            LOG("[WebDAV] root collection PROPFIND returned 401: credentials rejected");
        } else {
            LOG("[WebDAV] root collection not verified (PROPFIND status %d); "
                "will verify/create on first upload", probe.status);
        }
    } else if (r.status == 401) {
        m_authFailed.store(true);
        LOG("[WebDAV] OPTIONS returned 401: credentials rejected");
    } else if (r.status == 404) {
        LOG("[WebDAV] base collection missing; creating it now");
        EnsureCollections("");
    } else if (r.status == 0) {
        LOG("[WebDAV] OPTIONS failed (network error); retrying on demand");
    } else {
        LOG("[WebDAV] OPTIONS returned status %d", r.status);
    }
}
