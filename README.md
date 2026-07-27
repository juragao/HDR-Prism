# HDR Prism

极简高效的UltraHDR 图片无损提取/封装工具。保留EXIF、ICC、色彩空间，不重编码，无 GUI。

## 用法

对于要封装的SDR底图和gainmap，请以相同文件名加-a和-b命名，例如底图：123-a.jpg，gainmap：123-b.jpg
把要处理的文件（数量不限）或文件夹拖到 `HDR prism.exe` 图标上，程序会以文件名判断工作模式，不带后缀的进行提取，带后缀的进行封装。
批量处理时每次只执行一种模式，不会同时处理两种需求。判断逻辑：按检测到的第一个 jpg 文件的命名决定模式

### 提取

对每个 UltraHDR 文件，生成：

- `[xxx]-a.jpg` — SDR 底图（字节级原样，保留完整EXIF/ICC）
- `[xxx]-b.jpg` — gainmap（字节级原样）
- `[xxx].json` — hdrgm 参数（GainMapMin/Max、Gamma、Offset、HDRCapacity 等），用于重新合成

- 默认保存在源目录\SDR output（可用hdrprism.ini 的 `SdrOutputDir`自定义）
- 非 UltraHDR 文件，或以 `-a` / `-b` 结尾的文件静默跳过。

### 封装

对每个 `[xxx]-a.jpg`，查找同目录的 `[xxx]-b.jpg` 与 `[xxx].json` 合成UltraHDR，如缺少json则使用hdrprism.ini内置的参数（可修改）：

- 参数优先级：同名 .json > -a 内嵌 hdrgm XMP > hdrprism.ini 默认值
- XMP / MPF 按 Google Ultra HDR Image Format 规范装配
- 输出的 `[xxx].jpg` 默认保存在源目录\HDR output（可用hdrprism.ini 的 `HdrOutputDir`自定义），重名时自动加 `-1`、`-2`

生成的UltraHDR图片也可用安卓应用GlowHDR可视化调整参数。

## 日志

静默运行，无窗口。仅出错时在exe旁写 `hdrprism.log`（UTF-8，追加），退出码 = 失败文件数。

## LICENSE

UltraHDRTool is licensed under the GNU General Public License v3.0 (GPL-3.0). You are free to use, modify, and redistribute this software under the terms of the GPL-3.0 license. Attribution and preservation of the license are required.
