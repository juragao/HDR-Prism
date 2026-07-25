// HDR Prism — UltraHDR (JPEG_R) 无损拆分/封装
// 纯容器字节操作，不重编码。单文件，零依赖（Win32 + CRT）。
// 构建: cl /nologo /O2 /std:c++17 /utf-8 /EHsc /W3 /Fe:hdrprism.exe src/main.cpp

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#pragma comment(linker, "/SUBSYSTEM:WINDOWS")
#pragma comment(linker, "/ENTRY:wmainCRTStartup")

namespace fs = std::filesystem;

namespace {

constexpr size_t kNpos = ~(size_t)0;

// ---------- 基础工具 ----------

std::string narrowUtf8(const std::wstring& ws) {
  if (ws.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, ws.data(), (int)ws.size(), s.data(), n, nullptr, nullptr);
  return s;
}

bool loadFile(const fs::path& p, std::vector<uint8_t>& out) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  LARGE_INTEGER sz{};
  if (!GetFileSizeEx(h, &sz) || sz.QuadPart > ((LONGLONG)1 << 31)) { CloseHandle(h); return false; }
  out.resize((size_t)sz.QuadPart);
  size_t done = 0;
  while (done < out.size()) {
    DWORD want = (DWORD)std::min<size_t>(out.size() - done, 1u << 24), got = 0;
    if (!ReadFile(h, out.data() + done, want, &got, nullptr) || got == 0) {
      CloseHandle(h);
      return false;
    }
    done += got;
  }
  CloseHandle(h);
  return true;
}

bool writeFile(const fs::path& p, const void* data, size_t len) {
  HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  size_t done = 0;
  const uint8_t* ptr = (const uint8_t*)data;
  while (done < len) {
    DWORD want = (DWORD)std::min<size_t>(len - done, 1u << 24), wrote = 0;
    if (!WriteFile(h, ptr + done, want, &wrote, nullptr)) { CloseHandle(h); return false; }
    done += wrote;
  }
  CloseHandle(h);
  return true;
}

bool fileExists(const fs::path& p) {
  std::error_code ec;
  return fs::exists(p, ec) && fs::is_regular_file(p, ec);
}

// 目标已存在时追加 -1、-2 …
fs::path uniquePath(const fs::path& desired) {
  std::error_code ec;
  if (!fs::exists(desired, ec)) return desired;
  std::wstring stem = desired.stem().wstring();
  std::wstring ext = desired.extension().wstring();
  fs::path dir = desired.parent_path();
  for (int i = 1; i < 10000; i++) {
    fs::path cand = dir / (stem + L"-" + std::to_wstring(i) + ext);
    if (!fs::exists(cand, ec)) return cand;
  }
  return {};
}

// ---------- JPEG 结构 ----------

// 返回 base 处 JPEG 图像的结束偏移（FFD9 之后），失败返回 0。
size_t jpegImageEnd(const std::vector<uint8_t>& d, size_t base) {
  if (base + 4 > d.size() || d[base] != 0xFF || d[base + 1] != 0xD8) return 0;
  size_t p = base + 2;
  while (p + 1 < d.size()) {
    if (d[p] != 0xFF) return 0;
    uint8_t m = d[p + 1];
    if (m == 0xFF) { p++; continue; }
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xD8) { p += 2; continue; }
    if (m == 0xD9) return p + 2;
    if (p + 4 > d.size()) return 0;
    uint32_t len = ((uint32_t)d[p + 2] << 8) | d[p + 3];
    if (len < 2 || p + 2 + len > d.size()) return 0;
    if (m == 0xDA) {  // SOS：熵编码数据经字节填充，FFD9 只会出现在图像末尾
      size_t q = p + 2 + len;
      while (q + 1 < d.size()) {
        if (d[q] == 0xFF && d[q + 1] == 0xD9) return q + 2;
        q++;
      }
      return 0;
    }
    p += 2 + len;
  }
  return 0;
}

// 遍历 base 处 JPEG 在 SOS 之前的各个段
template <typename F>
void forEachSegment(const std::vector<uint8_t>& d, size_t base, size_t limit, F&& cb) {
  if (base + 4 > d.size() || d[base] != 0xFF || d[base + 1] != 0xD8) return;
  limit = std::min(limit, d.size());
  size_t p = base + 2;
  while (p + 4 <= limit) {
    if (d[p] != 0xFF) return;
    uint8_t m = d[p + 1];
    if (m == 0xFF) { p++; continue; }
    if (m == 0xDA || m == 0xD9) return;
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xD8) { p += 2; continue; }
    uint32_t len = ((uint32_t)d[p + 2] << 8) | d[p + 3];
    if (len < 2 || p + 2 + len > limit) return;
    cb(m, p + 4, len - 2);
    p += 2 + len;
  }
}

const char kXmpSig[] = "http://ns.adobe.com/xap/1.0/";
const char kXmpExtSig[] = "http://ns.adobe.com/xmp/extension/";
constexpr size_t kXmpSigLen = sizeof(kXmpSig);        // 29，含结尾 NUL
constexpr size_t kXmpExtSigLen = sizeof(kXmpExtSig);  // 35，含结尾 NUL

bool extractXmp(const std::vector<uint8_t>& d, size_t base, size_t end, std::string& xmp) {
  bool found = false;
  forEachSegment(d, base, end, [&](uint8_t m, size_t payload, size_t payloadLen) {
    if (!found && m == 0xE1 && payloadLen > kXmpSigLen &&
        memcmp(&d[payload], kXmpSig, kXmpSigLen) == 0) {
      xmp.assign((const char*)&d[payload + kXmpSigLen], payloadLen - kXmpSigLen);
      found = true;
    }
  });
  return found;
}

bool jpegDimensions(const std::vector<uint8_t>& d, size_t base, size_t end, uint32_t& w, uint32_t& h) {
  bool found = false;
  forEachSegment(d, base, end, [&](uint8_t m, size_t payload, size_t payloadLen) {
    if (found) return;
    if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC && payloadLen >= 5) {
      h = ((uint32_t)d[payload + 1] << 8) | d[payload + 2];
      w = ((uint32_t)d[payload + 3] << 8) | d[payload + 4];
      found = true;
    }
  });
  return found;
}

// ---------- hdrgm 参数 ----------

struct GmParams {
  double gainMapMin = 0.0;
  double gainMapMax = 3.0;
  double gamma = 1.0;
  double offsetSDR = 0.015625;
  double offsetHDR = 0.015625;
  double hdrCapacityMin = 0.0;
  double hdrCapacityMax = 3.0;
  bool hdrCapacityMaxSet = false;
  bool baseRenditionIsHDR = false;
};

std::string fmtNum(double v) {
  char b[64];
  snprintf(b, sizeof(b), "%.10g", v);
  return b;
}

// 兼容属性形式 hdrgm:Key="v" 与元素形式 <hdrgm:Key>v</hdrgm:Key>
bool extractHdrgmValue(const std::string& xmp, const char* key, std::string& out) {
  std::string pat = std::string("hdrgm:") + key;
  size_t pos = xmp.find(pat);
  if (pos == std::string::npos) return false;
  pos += pat.size();
  size_t k = xmp.find_first_not_of(" \t\r\n", pos);
  if (k == std::string::npos) return false;
  if (xmp[k] == '=') {
    k = xmp.find_first_not_of(" \t\r\n", k + 1);
    if (k == std::string::npos || (xmp[k] != '"' && xmp[k] != '\'')) return false;
    size_t e = xmp.find(xmp[k], k + 1);
    if (e == std::string::npos) return false;
    out = xmp.substr(k + 1, e - k - 1);
    return true;
  }
  if (xmp[k] == '>') {
    size_t e = xmp.find('<', k + 1);
    if (e == std::string::npos) return false;
    out = xmp.substr(k + 1, e - k - 1);
    return true;
  }
  return false;
}

void overlayFromXmp(GmParams& p, const std::string& xmp) {
  std::string v;
  if (extractHdrgmValue(xmp, "GainMapMin", v)) p.gainMapMin = strtod(v.c_str(), nullptr);
  if (extractHdrgmValue(xmp, "GainMapMax", v)) p.gainMapMax = strtod(v.c_str(), nullptr);
  if (extractHdrgmValue(xmp, "Gamma", v)) p.gamma = strtod(v.c_str(), nullptr);
  if (extractHdrgmValue(xmp, "OffsetSDR", v)) p.offsetSDR = strtod(v.c_str(), nullptr);
  if (extractHdrgmValue(xmp, "OffsetHDR", v)) p.offsetHDR = strtod(v.c_str(), nullptr);
  if (extractHdrgmValue(xmp, "HDRCapacityMin", v)) p.hdrCapacityMin = strtod(v.c_str(), nullptr);
  if (extractHdrgmValue(xmp, "HDRCapacityMax", v)) {
    p.hdrCapacityMax = strtod(v.c_str(), nullptr);
    p.hdrCapacityMaxSet = true;
  }
  if (extractHdrgmValue(xmp, "BaseRenditionIsHDR", v))
    p.baseRenditionIsHDR = (v == "True" || v == "true" || v == "1");
}

bool jsonNumber(const std::string& js, const char* key, double& out) {
  std::string pat = std::string("\"") + key + "\"";
  size_t k = js.find(pat);
  if (k == std::string::npos) return false;
  k = js.find(':', k + pat.size());
  if (k == std::string::npos) return false;
  k = js.find_first_not_of(" \t\r\n", k + 1);
  if (k == std::string::npos) return false;
  const char* s = js.c_str() + k;
  char* endp = nullptr;
  double v = strtod(s, &endp);
  if (endp == s) return false;
  out = v;
  return true;
}

bool jsonBool(const std::string& js, const char* key, bool& out) {
  std::string pat = std::string("\"") + key + "\"";
  size_t k = js.find(pat);
  if (k == std::string::npos) return false;
  k = js.find(':', k + pat.size());
  if (k == std::string::npos) return false;
  k = js.find_first_not_of(" \t\r\n", k + 1);
  if (k == std::string::npos) return false;
  if (js.compare(k, 4, "true") == 0) { out = true; return true; }
  if (js.compare(k, 5, "false") == 0) { out = false; return true; }
  return false;
}

void overlayFromJson(GmParams& p, const std::string& js) {
  double d;
  bool b;
  if (jsonNumber(js, "gainMapMin", d)) p.gainMapMin = d;
  if (jsonNumber(js, "gainMapMax", d)) p.gainMapMax = d;
  if (jsonNumber(js, "gamma", d)) p.gamma = d;
  if (jsonNumber(js, "offsetSDR", d)) p.offsetSDR = d;
  if (jsonNumber(js, "offsetHDR", d)) p.offsetHDR = d;
  if (jsonNumber(js, "hdrCapacityMin", d)) p.hdrCapacityMin = d;
  if (jsonNumber(js, "hdrCapacityMax", d)) {
    p.hdrCapacityMax = d;
    p.hdrCapacityMaxSet = true;
  }
  if (jsonBool(js, "baseRenditionIsHDR", b)) p.baseRenditionIsHDR = b;
}

std::string toJson(const GmParams& p, uint32_t gmW, uint32_t gmH) {
  std::string s = "{\r\n";
  s += "  \"format\": \"uhdr-params-v1\",\r\n";
  s += "  \"version\": \"1.0\",\r\n";
  s += "  \"gainMapMin\": " + fmtNum(p.gainMapMin) + ",\r\n";
  s += "  \"gainMapMax\": " + fmtNum(p.gainMapMax) + ",\r\n";
  s += "  \"gamma\": " + fmtNum(p.gamma) + ",\r\n";
  s += "  \"offsetSDR\": " + fmtNum(p.offsetSDR) + ",\r\n";
  s += "  \"offsetHDR\": " + fmtNum(p.offsetHDR) + ",\r\n";
  s += "  \"hdrCapacityMin\": " + fmtNum(p.hdrCapacityMin) + ",\r\n";
  s += "  \"hdrCapacityMax\": " + fmtNum(p.hdrCapacityMax) + ",\r\n";
  s += "  \"baseRenditionIsHDR\": ";
  s += p.baseRenditionIsHDR ? "true" : "false";
  if (gmW && gmH)
    s += ",\r\n  \"gainmapSize\": [" + std::to_string(gmW) + ", " + std::to_string(gmH) + "]\r\n";
  else
    s += "\r\n";
  s += "}\r\n";
  return s;
}

// primary=true：主图 XMP，含 GainMap 条目与字节长度
std::string buildXmp(const GmParams& p, uint32_t gainmapLen, bool primary) {
  std::string s;
  s += "<?xpacket begin=\"\xEF\xBB\xBF\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n";
  s += "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\" x:xmptk=\"HDR Prism\">\n";
  s += " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n";
  s += "  <rdf:Description rdf:about=\"\"\n";
  s += "   xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\"\n";
  s += "   xmlns:Container=\"http://ns.google.com/photos/1.0/container/\"\n";
  s += "   xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\"\n";
  s += "   hdrgm:Version=\"1.0\"\n";
  s += "   hdrgm:GainMapMin=\"" + fmtNum(p.gainMapMin) + "\"\n";
  s += "   hdrgm:GainMapMax=\"" + fmtNum(p.gainMapMax) + "\"\n";
  s += "   hdrgm:Gamma=\"" + fmtNum(p.gamma) + "\"\n";
  s += "   hdrgm:OffsetSDR=\"" + fmtNum(p.offsetSDR) + "\"\n";
  s += "   hdrgm:OffsetHDR=\"" + fmtNum(p.offsetHDR) + "\"\n";
  s += "   hdrgm:HDRCapacityMin=\"" + fmtNum(p.hdrCapacityMin) + "\"\n";
  s += "   hdrgm:HDRCapacityMax=\"" + fmtNum(p.hdrCapacityMax) + "\"\n";
  s += std::string("   hdrgm:BaseRenditionIsHDR=\"") + (p.baseRenditionIsHDR ? "True" : "False") + "\">\n";
  s += "   <Container:Directory>\n";
  s += "    <rdf:Seq>\n";
  s += "     <rdf:li rdf:parseType=\"Resource\">\n";
  s += "      <Item:Semantic>Primary</Item:Semantic>\n";
  s += "      <Item:Mime>image/jpeg</Item:Mime>\n";
  s += "     </rdf:li>\n";
  if (primary) {
    s += "     <rdf:li rdf:parseType=\"Resource\">\n";
    s += "      <Item:Semantic>GainMap</Item:Semantic>\n";
    s += "      <Item:Mime>image/jpeg</Item:Mime>\n";
    s += "      <Item:Length>" + std::to_string(gainmapLen) + "</Item:Length>\n";
    s += "     </rdf:li>\n";
  }
  s += "    </rdf:Seq>\n";
  s += "   </Container:Directory>\n";
  s += "  </rdf:Description>\n";
  s += " </rdf:RDF>\n";
  s += "</x:xmpmeta>\n";
  s += "<?xpacket end=\"w\"?>\n";
  return s;
}

// ---------- 容器装配 ----------

std::vector<uint8_t> makeXmpSegment(const std::string& xmp) {
  std::vector<uint8_t> s;
  uint32_t len = (uint32_t)(kXmpSigLen + xmp.size()) + 2;
  s.push_back(0xFF);
  s.push_back(0xE1);
  s.push_back((uint8_t)(len >> 8));
  s.push_back((uint8_t)len);
  s.insert(s.end(), kXmpSig, kXmpSig + kXmpSigLen);
  s.insert(s.end(), xmp.begin(), xmp.end());
  return s;
}

// 重建图像：剥离旧 XMP 与 MPF 段（可选），在头部 APP0/EXIF/APP2 序列之后插入新 XMP
bool rebuildWithXmp(const std::vector<uint8_t>& d, size_t base, size_t end,
                    const std::string& xmp, bool stripOld, std::vector<uint8_t>& out) {
  if (base + 2 > end || d[base] != 0xFF || d[base + 1] != 0xD8) return false;
  std::vector<uint8_t> seg = makeXmpSegment(xmp);
  out.assign(d.begin() + base, d.begin() + base + 2);
  bool inserted = false;
  size_t p = base + 2;
  while (p + 4 <= end) {
    if (d[p] != 0xFF) return false;
    uint8_t m = d[p + 1];
    if (m == 0xFF) { p++; continue; }
    if (m == 0xDA || m == 0xD9) break;
    if (m == 0x01 || (m >= 0xD0 && m <= 0xD7) || m == 0xD8) {
      out.push_back(0xFF);
      out.push_back(m);
      p += 2;
      continue;
    }
    uint32_t len = ((uint32_t)d[p + 2] << 8) | d[p + 3];
    if (len < 2 || p + 2 + len > end) return false;
    const uint8_t* pl = &d[p + 4];
    size_t plLen = len - 2;
    bool isXmp = m == 0xE1 && plLen > kXmpSigLen && memcmp(pl, kXmpSig, kXmpSigLen) == 0;
    bool isXmpExt = m == 0xE1 && plLen > kXmpExtSigLen && memcmp(pl, kXmpExtSig, kXmpExtSigLen) == 0;
    bool isExif = m == 0xE1 && plLen >= 6 && memcmp(pl, "Exif\0\0", 6) == 0;
    bool isMpf = (m == 0xE2 || m == 0xEB) && plLen >= 4 && memcmp(pl, "MPF\0", 4) == 0;
    if (stripOld && (isXmp || isXmpExt || isMpf)) {
      p += 2 + len;
      continue;
    }
    bool leading = m == 0xE0 || m == 0xE2 || isExif || isXmp || isXmpExt;
    if (!inserted && !leading) {
      out.insert(out.end(), seg.begin(), seg.end());
      inserted = true;
    }
    out.insert(out.end(), d.begin() + p, d.begin() + p + 2 + len);
    p += 2 + len;
  }
  if (!inserted) out.insert(out.end(), seg.begin(), seg.end());
  out.insert(out.end(), d.begin() + p, d.begin() + end);
  return true;
}

void be16(std::vector<uint8_t>& v, uint32_t x) {
  v.push_back((uint8_t)(x >> 8));
  v.push_back((uint8_t)x);
}
void be32(std::vector<uint8_t>& v, uint32_t x) { be16(v, x >> 16); be16(v, x); }

// MPF (CIPA DC-X007) APP11 段：主图 + gainmap 两幅
std::vector<uint8_t> buildMpfSegment(uint32_t primarySize, uint32_t gainmapSize) {
  std::vector<uint8_t> body;
  body.insert(body.end(), {'M', 'M', 0x00, 0x2A, 0x00, 0x00, 0x00, 0x08});  // MP Header
  be16(body, 3);                                        // IFD 条目数
  be16(body, 0xB000); be16(body, 7); be32(body, 4);     // MPFVersion UNDEFINED[4]
  body.insert(body.end(), {'0', '1', '0', '0'});
  be16(body, 0xB001); be16(body, 4); be32(body, 1); be32(body, 2);   // NumberOfImages
  be16(body, 0xB002); be16(body, 7); be32(body, 32); be32(body, 50); // MPEntry 数据偏移
  be32(body, 0);                                        // 无下一个 IFD
  // Entry[0]：Baseline MP Primary，offset 约定为 0
  be32(body, 0x00030000); be32(body, primarySize); be32(body, 0); be16(body, 0); be16(body, 0);
  // Entry[1]：gainmap 紧跟本段之后，距 MP Header 起点 82 字节
  be32(body, 0x00000000); be32(body, gainmapSize); be32(body, 82); be16(body, 0); be16(body, 0);
  std::vector<uint8_t> seg;
  seg.push_back(0xFF);
  seg.push_back(0xEB);
  be16(seg, (uint32_t)body.size() + 4 + 2);
  seg.insert(seg.end(), {'M', 'P', 'F', 0x00});
  seg.insert(seg.end(), body.begin(), body.end());
  return seg;
}

// ---------- 配置 / 日志 ----------

struct Config {
  std::wstring outputDir;
  GmParams defaults;
};

Config loadConfig(const fs::path& exeDir) {
  Config c;
  fs::path ini = exeDir / L"hdrprism.ini";
  auto getStr = [&](const wchar_t* sec, const wchar_t* key) {
    wchar_t buf[520];
    DWORD n = GetPrivateProfileStringW(sec, key, L"", buf, 520, ini.c_str());
    return std::wstring(buf, n);
  };
  auto getNum = [&](const wchar_t* key, double def, bool* set = nullptr) {
    std::wstring s = getStr(L"Defaults", key);
    if (s.empty()) return def;
    if (set) *set = true;
    return _wtof(s.c_str());
  };
  c.outputDir = getStr(L"Paths", L"OutputDir");
  GmParams& p = c.defaults;
  p.gainMapMin = getNum(L"GainMapMin", 0.0);
  p.gainMapMax = getNum(L"GainMapMax", 3.0);
  p.gamma = getNum(L"Gamma", 1.0);
  p.offsetSDR = getNum(L"OffsetSDR", 0.015625);
  p.offsetHDR = getNum(L"OffsetHDR", 0.015625);
  p.hdrCapacityMin = getNum(L"HDRCapacityMin", 0.0);
  p.hdrCapacityMax = getNum(L"HDRCapacityMax", 3.0, &p.hdrCapacityMaxSet);
  std::wstring br = getStr(L"Defaults", L"BaseRenditionIsHDR");
  p.baseRenditionIsHDR = (br == L"1" || br == L"true" || br == L"True");
  return c;
}

class Logger {
public:
  void init(const fs::path& exeDir) {
    m_candidates.push_back(exeDir / L"hdrprism.log");
    wchar_t tmp[MAX_PATH];
    if (GetTempPathW(MAX_PATH, tmp)) m_candidates.push_back(fs::path(tmp) / L"hdrprism.log");
  }
  void error(const std::wstring& msg) {
    for (const auto& p : m_candidates) {
      HANDLE h = CreateFileW(p.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
                             OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) continue;
      LARGE_INTEGER sz{};
      DWORD w;
      if (GetFileSizeEx(h, &sz) && sz.QuadPart == 0) {
        const char bom[] = "\xEF\xBB\xBF";
        WriteFile(h, bom, 3, &w, nullptr);
      }
      std::string line = "[" + timestamp() + "] " + narrowUtf8(msg) + "\r\n";
      WriteFile(h, line.data(), (DWORD)line.size(), &w, nullptr);
      CloseHandle(h);
      return;
    }
  }

private:
  static std::string timestamp() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    char b[64];
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return b;
  }
  std::vector<fs::path> m_candidates;
};

// ---------- 模式与处理 ----------

enum class Result { Ok, Skipped, Error };

bool isJpegExt(const fs::path& p) {
  std::wstring e = p.extension().wstring();
  std::transform(e.begin(), e.end(), e.begin(), [](wchar_t c) { return (wchar_t)::towlower(c); });
  return e == L".jpg" || e == L".jpeg";
}

bool stemEndsWith(const fs::path& p, wchar_t suffix) {
  std::wstring s = p.stem().wstring();
  if (s.size() < 2) return false;
  return s[s.size() - 2] == L'-' && ::towlower(s[s.size() - 1]) == suffix;
}

bool xmpHasHdrgm(const std::string& xmp) {
  return xmp.find("hdr-gain-map") != std::string::npos ||
         xmp.find("hdrgm:GainMapMax") != std::string::npos;
}

Result splitOne(const fs::path& f, Logger& log) {
  std::vector<uint8_t> d;
  if (!loadFile(f, d)) { log.error(L"读取失败: " + f.wstring()); return Result::Error; }
  size_t eoi = jpegImageEnd(d, 0);
  if (!eoi) { log.error(L"JPEG 结构损坏: " + f.wstring()); return Result::Error; }

  std::string xmp;
  bool hasXmp = extractXmp(d, 0, eoi, xmp);
  size_t gm = kNpos;
  size_t lim = std::min(d.size() - 1, eoi + (1 << 20));
  for (size_t i = eoi; i < lim; i++)
    if (d[i] == 0xFF && d[i + 1] == 0xD8) { gm = i; break; }
  bool hdr = hasXmp && xmpHasHdrgm(xmp);
  if (!hdr) return Result::Skipped;  // 非 UltraHDR，静默跳过
  if (gm == kNpos) {
    log.error(L"检测到 hdrgm 元数据但找不到 gainmap 图像: " + f.wstring());
    return Result::Error;
  }
  size_t gmEnd = jpegImageEnd(d, gm);
  if (!gmEnd) { log.error(L"gainmap 损坏: " + f.wstring()); return Result::Error; }

  GmParams params;
  std::string xmpGm;
  if (extractXmp(d, gm, gmEnd, xmpGm)) overlayFromXmp(params, xmpGm);  // 部分厂商把参数写在 gainmap XMP
  overlayFromXmp(params, xmp);  // 主图 XMP 优先
  if (!params.hdrCapacityMaxSet) params.hdrCapacityMax = params.gainMapMax;
  uint32_t gw = 0, gh = 0;
  jpegDimensions(d, gm, d.size(), gw, gh);

  fs::path dir = f.parent_path();
  std::wstring base = f.stem().wstring();
  std::wstring ext = f.extension().wstring();
  fs::path aOut = uniquePath(dir / (base + L"-a" + ext));
  fs::path bOut = uniquePath(dir / (base + L"-b" + ext));
  fs::path jOut = uniquePath(dir / (base + L".json"));
  if (aOut.empty() || bOut.empty() || jOut.empty()) {
    log.error(L"无法分配输出文件名: " + f.wstring());
    return Result::Error;
  }
  if (!writeFile(aOut, d.data(), eoi)) { log.error(L"写入失败: " + aOut.wstring()); return Result::Error; }
  if (!writeFile(bOut, d.data() + gm, gmEnd - gm)) { log.error(L"写入失败: " + bOut.wstring()); return Result::Error; }
  std::string js = toJson(params, gw, gh);
  if (!writeFile(jOut, js.data(), js.size())) { log.error(L"写入失败: " + jOut.wstring()); return Result::Error; }
  return Result::Ok;
}

Result assembleOne(const fs::path& f, const Config& cfg, Logger& log) {
  fs::path dir = f.parent_path();
  std::wstring stem = f.stem().wstring();
  std::wstring base = stem.substr(0, stem.size() - 2);  // 去掉 -a
  std::wstring ext = f.extension().wstring();

  fs::path bPath;
  const wchar_t* exts[] = {ext.c_str(), L".jpg", L".jpeg"};
  for (const wchar_t* e : exts) {
    fs::path cand = dir / (base + L"-b" + e);
    if (fileExists(cand)) { bPath = cand; break; }
  }
  if (bPath.empty()) {
    log.error(L"缺少配对 gainmap (-b): " + f.wstring());
    return Result::Error;
  }

  std::vector<uint8_t> a, b;
  if (!loadFile(f, a)) { log.error(L"读取失败: " + f.wstring()); return Result::Error; }
  if (!loadFile(bPath, b)) { log.error(L"读取失败: " + bPath.wstring()); return Result::Error; }
  size_t eoiA = jpegImageEnd(a, 0);
  if (!eoiA) { log.error(L"JPEG 结构损坏: " + f.wstring()); return Result::Error; }
  size_t eoiB = jpegImageEnd(b, 0);
  if (!eoiB) { log.error(L"JPEG 结构损坏: " + bPath.wstring()); return Result::Error; }

  // 参数优先级：同名 .json > -a 内嵌 XMP > -b 内嵌 XMP > ini 默认值（后者补前者缺）
  GmParams params = cfg.defaults;
  std::string xmpB;
  bool hasXmpB = extractXmp(b, 0, eoiB, xmpB);
  if (hasXmpB && xmpHasHdrgm(xmpB)) overlayFromXmp(params, xmpB);
  std::string xmpA;
  bool hasXmpA = extractXmp(a, 0, eoiA, xmpA);
  if (hasXmpA && xmpHasHdrgm(xmpA)) overlayFromXmp(params, xmpA);
  fs::path jPath = dir / (base + L".json");
  if (fileExists(jPath)) {
    std::vector<uint8_t> jd;
    if (loadFile(jPath, jd) && !jd.empty())
      overlayFromJson(params, std::string((const char*)jd.data(), jd.size()));
  }
  if (!params.hdrCapacityMaxSet) params.hdrCapacityMax = params.gainMapMax;

  // gainmap：保持字节原样；仅当缺少 XMP 时注入最小 XMP
  std::vector<uint8_t> gmFinal;
  if (hasXmpB) {
    gmFinal.assign(b.begin(), b.begin() + eoiB);
  } else if (!rebuildWithXmp(b, 0, eoiB, buildXmp(params, 0, false), false, gmFinal)) {
    log.error(L"gainmap 处理失败: " + bPath.wstring());
    return Result::Error;
  }

  // 主图：剥旧 XMP，插入新 XMP（EXIF/ICC 等其余段原样保留）
  std::vector<uint8_t> primary;
  if (!rebuildWithXmp(a, 0, eoiA, buildXmp(params, (uint32_t)gmFinal.size(), true), true, primary)) {
    log.error(L"主图处理失败: " + f.wstring());
    return Result::Error;
  }
  std::vector<uint8_t> mpf = buildMpfSegment((uint32_t)primary.size(), (uint32_t)gmFinal.size());

  fs::path outDir = cfg.outputDir.empty() ? dir : fs::path(cfg.outputDir);
  std::error_code ec;
  fs::create_directories(outDir, ec);
  fs::path outPath = uniquePath(outDir / (base + ext));
  if (outPath.empty()) {
    log.error(L"无法分配输出文件名: " + f.wstring());
    return Result::Error;
  }
  HANDLE h = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h == INVALID_HANDLE_VALUE) { log.error(L"写入失败: " + outPath.wstring()); return Result::Error; }
  auto writeAll = [&](const std::vector<uint8_t>& v) {
    size_t done = 0;
    while (done < v.size()) {
      DWORD want = (DWORD)std::min<size_t>(v.size() - done, 1u << 24), got = 0;
      if (!WriteFile(h, v.data() + done, want, &got, nullptr)) return false;
      done += got;
    }
    return true;
  };
  bool ok = writeAll(primary) && writeAll(mpf) && writeAll(gmFinal);
  CloseHandle(h);
  if (!ok) { log.error(L"写入失败: " + outPath.wstring()); return Result::Error; }
  return Result::Ok;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  wchar_t exeBuf[MAX_PATH];
  GetModuleFileNameW(nullptr, exeBuf, MAX_PATH);
  fs::path exeDir = fs::path(exeBuf).parent_path();

  Config cfg = loadConfig(exeDir);
  Logger log;
  log.init(exeDir);

  // 收集输入（支持文件夹，仅顶层）
  std::vector<fs::path> inputs;
  for (int i = 1; i < argc; i++) {
    std::error_code ec;
    fs::path p(argv[i]);
    if (fs::is_directory(p, ec)) {
      for (const auto& e : fs::directory_iterator(p, ec))
        if (e.is_regular_file(ec) && isJpegExt(e.path())) inputs.push_back(e.path());
    } else if (fs::is_regular_file(p, ec)) {
      inputs.push_back(p);
    }
  }

  // 模式：第一个 jpg/jpeg 的后缀决定
  enum class Mode { None, Split, Assemble } mode = Mode::None;
  for (const auto& p : inputs) {
    if (!isJpegExt(p)) continue;
    mode = (stemEndsWith(p, L'a') || stemEndsWith(p, L'b')) ? Mode::Assemble : Mode::Split;
    break;
  }
  if (mode == Mode::None) return 0;

  int errors = 0;
  for (const auto& f : inputs) {
    if (!isJpegExt(f)) continue;
    Result r;
    if (mode == Mode::Split) {
      if (stemEndsWith(f, L'a') || stemEndsWith(f, L'b')) continue;  // 跳过自己的产物
      r = splitOne(f, log);
    } else {
      if (!stemEndsWith(f, L'a')) continue;  // 只处理 -a
      r = assembleOne(f, cfg, log);
    }
    if (r == Result::Error) errors++;
  }
  return errors > 255 ? 255 : errors;
}