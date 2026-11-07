# 隐水印 · R-DCT / DCT / DWT

0.1.1 增加 **R-DCT** 原生增强路线，保留 0.1.0 的 DCT 与 DWT。三者均在本机处理，不下载模型、不需要 Python 或推理运行库；密钥不保存到配置。

## 选择算法

- **R-DCT**：优先用于需要经历 JPEG、裁剪、缩放的图片，增强同步定位和缺失数据恢复。写入至少需要 256×256 像素。
- **DCT**：旧版差分 QIM，兼容已有图片，默认写入较轻。
- **DWT**：旧版三层 Haar LH3 QIM，保持原协议和固定参数。

图片写入默认选择 R-DCT；提取“自动”依次尝试 R-DCT、DCT、DWT。旧配置不会被自动改成新算法。高级模式可显式选择 R-DCT，A / B 捕获合成复用同一个内核。

对窗口使用隐水印：在高级模式保留原内容，勾选“叠加隐水印”，选择算法并填写载荷、密钥。显性水印是另一个独立开关，可以关闭或同时叠加。显示范围由用户选择，支持屏幕与捕获均显示、或仅捕获；排除 / 透明不输出原内容，纯色 / 文字替身目前不支持隐水印。

## R-DCT 怎么工作

1. 在亮度的 8×8 DCT 系数对上写入数据；独立系数写入由密钥导出的二维同步导频。
2. 每 256×256 像素重复一个瓦片。导频分散覆盖整个瓦片，避免同步只依赖一条窄带。
3. 一个 RS 字节的八个比特放在相邻的 4×2 个块内，再做字节位置交织。裁剪后缺失的区域可以作为“擦除”，而不是把未知比特硬判成零。
4. 聚合重复载体的软证据，尝试低置信度字节擦除；RS(124,52) 在 `2×错误字节数 + 擦除字节数 ≤ 72` 的范围内具备纠错能力。图片实际能否恢复还取决于剩余载体分布、同步和噪声。
5. 完整载荷必须同时通过协议、算法标识、CRC 与密钥校验。同步得分高不等于载荷有效。
6. 对较大截图先去除未标记背景产生的常量软投票偏置，改善水印窗口只占部分画面的情况；复杂背景仍可先裁剪目标区域。

R-DCT 使用算法编号 `3`；旧 DCT / DWT 为 `1` / `2`。载荷仍最多 32 个 UTF-8 字节；密钥为 1–128 字节。

## 参数和画质

| 项目 | 说明 |
|---|---|
| 量化参数 | 默认 24；R-DCT 数据步长为参数的 8/3 倍，导频步长为 2 倍，默认 64 / 48 |
| 画质取舍 | 数值越大通常越耐压缩，同时细微纹理更明显；纯白、纯黑区域需特别检查 |
| 裁剪 | 默认已启用搜索瓦片和搜索相位；剩余面积过小或数据分布不完整时仍会失败 |
| 缩放 | 填原始宽高，并使用非 1 的缩放值；例如 `0.75,768,512,0,0,0,0` |
| 同步得分 | R-DCT：0–1000；旧算法：0–32，不能跨算法直接比较 |
| 输出 | 首次使用优先保存 PNG，并立即提取核对；JPEG 质量只影响保存编码 |

R-DCT 的参数 24 与旧 DCT 的参数 24 **不是相同的画质预算**。增强路线包含更强数据写入和额外导频，不能把全部收益都归因于纠错设计。

## 验证和边界

本版使用独立 NumPy DCT、独立 RS 实现与原生 C++ 做双向互通，并通过真实图片编解码测试。压缩、裁剪、缩放、模糊、噪声分别测试，缩放测试提供原始尺寸提示。有限样本的成功不表示任意图片和变换组合均能恢复。

本轮样本为两张明/暗界面图片和一张程序生成的纹理图。每张测试无损、JPEG 质量 90/70/50/35、Lanczos 缩至 75%/50%（提供原始尺寸）、非对齐 192×224 裁剪、裁剪后 JPEG 70、轻度模糊和噪声，共 33 项。R-DCT 恢复 **33/33**；旧 DCT 默认参数为 **18/33**，按每张图调到接近 R-DCT 的 PSNR 后为 **23/33**。同画质对照仍是有限样本结果，不是通用恢复率。

受控窗口的真实桌面 BitBlt 捕获也已恢复 R-DCT 载荷；A / B 受控合成冒烟要求实际提取成功。额外验证实际 GUI 给带标题栏演示窗口叠加显隐水印→桌面截图→GUI 自动提取，以及窗口置于较大背景截图中的提取。本轮修复了水印层被原窗口遮挡，以及嵌入后缩放非客户区导致的载体破坏。这里没有把受控窗口结果扩成 OBS / 会议全后端通过。

- 不自动估计任意缩放、旋转或透视。界面的几何参数不包含旋转控制。
- 不保证打印扫描、手机拍屏、强透视、严重裁剪、强降噪或主动去水印后的恢复。
- OBS、会议视频、独占全屏、HDR 和不同捕获后端需要逐项验证，普通图片往返不能代替它们。
- 水印不等于加密保密或身份取证。保留的 V1 包使用 32 位截断校验标签，不应作为强身份认证或法律归属证明。

## English summary

R-DCT is a native, model-free enhanced carrier, not a claim of universal state-of-the-art performance. It combines differential luminance DCT QIM, an independent distributed keyed pilot, byte-local spatial interleaving, and reliability-guided Reed–Solomon errors/erasures decoding. Legacy DCT and DWT remain available.

Embedding requires at least 256×256 pixels; payload/key limits are 32/128 bytes. Default strength 24 maps to data/pilot steps 64/48, so it uses more image distortion than legacy DCT at strength 24. Tile and pixel-phase search are enabled by default. For resized images, supply original dimensions and a non-1 scale under Advanced parameters. Visible and hidden watermarks are independent additions and may be combined for either audience. Desktop/screen capture includes the composed surface; direct capture of the original target may omit external overlays. Pilot scores use 0–1000; legacy scores use 0–32. Recovery is reported only after packet validation.

Camera recapture, print/scan, arbitrary geometry, severe editing, and every external capture backend are not guaranteed. The V1 packet's truncated 32-bit authentication tag is not a strong identity or ownership proof.
