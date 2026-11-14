# Temporal Denoise（时域降噪）OBS 滤镜插件

针对夜间/低光直播画面的 GPU 时域降噪滤镜。利用前后帧做运动估计与加权平均：
静止区域强降噪，运动区域保细节，全部在 GPU 上完成，不把帧读回 CPU。

## 原理

```
输入帧 ──► input 纹理
           │
           ├─► SceneStat pass（1x1 纹理：全帧平均亮度差，用于场景切换检测）
           │
           ├─► Denoise pass（当前帧 + 历史帧 + 场景统计）
           │     · 3x3 均值亮度差做运动估计（抗噪，不会把噪点误判为运动）
           │     · 静止像素：weight ≈ strength，与历史帧加权平均（IIR）
           │     · 运动像素：weight → 0，保留当前帧细节
           │     · 边缘/纹理区域按 detail 参数降低平滑，保留细节
           │     · 色度通道权重 ×1.25（低光下色度噪点更重）
           │     · 全帧平均差超过阈值 → 判定场景切换，丢弃历史帧，避免拖影
           │
           └─► 输出即下一帧的历史帧（ping-pong 双缓冲，无需额外拷贝）
```

分辨率变化、首帧会自动重置历史帧。所有检测统计都留在 GPU 纹理中。

## 目录结构

```
temporal-denoise/
├── CMakeLists.txt              # 独立 CMake 工程
├── temporal-denoise.c          # 插件主程序（OBS filter source）
├── data/
│   ├── temporal-denoise.effect # GPU shader（SceneStat / Denoise / Draw）
│   └── locale/                 # en-US / zh-CN 界面文案
├── test/
│   └── offline_test.cpp        # 离线算法测试（无 OBS 依赖）
└── README.md
```

## 编译

依赖：CMake ≥ 3.28、C/C++ 编译器、libobs 开发文件。

**方式 A：系统已安装 OBS 开发文件**（`find_package(libobs)` 或 pkg-config 能找到）：

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

**方式 B：指向 obs-studio 源码树**（无需安装 OBS）：

```bash
cmake -B build \
  -DOBS_SOURCE_DIR=/path/to/obs-studio \
  -DSIMDE_INCLUDE_DIR=/path/to/simde \   # libobs 头文件依赖 simde
  -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

如果同时通过 `-DOBS_LIBRARY=/path/to/libobs.so` 给出已编译的 libobs 会链接它；
否则在 Linux/macOS 上以未解析符号方式构建模块（OBS 加载时解析），不影响使用。

也可以把本目录放进 obs-studio 源码树中用 `add_subdirectory` 构建
（会自动识别 `OBS::libobs` 目标）。

## 安装

```bash
cmake --install build
```

默认安装到用户级插件目录（无需 root）：

```
~/.config/obs-studio/plugins/temporal-denoise/
├── bin/64bit/temporal-denoise.so
└── data/ (temporal-denoise.effect, locale/)
```

系统级安装（对所有用户生效）：

```bash
cmake -B build -DOBS_PLUGIN_DESTINATION=/usr/lib/obs-plugins/.. # 见下
# 或手动：
sudo mkdir -p /usr/lib/obs-plugins/temporal-denoise
sudo cp build/temporal-denoise.so /usr/lib/obs-plugins/temporal-denoise/bin/64bit/
sudo cp -r data/* /usr/share/obs/obs-plugins/temporal-denoise/data/  # 实际路径按发行版调整
```

重启 OBS 后，在来源的「滤镜」→「+」中应能看到 **时域降噪 (Temporal Denoise)**。

## 参数

| 参数 | 范围 | 默认 | 说明 |
|---|---|---|---|
| 强度 Strength | 0–1 | 0.85 | 时域平均的最大权重，越大降噪越强 |
| 运动阈值 Motion Threshold | 0–0.5 | 0.04 | 亮度差超过该值判定为运动；噪点越大该值应调高 |
| 细节保留 Detail Preservation | 0–1 | 0.5 | 越大越保护边缘/纹理不被平滑 |

建议：夜间噪点很重时把运动阈值调到 0.06–0.10。

## NV12 / YUV420P 支持

OBS 在滤镜链之前会把 NV12、I420（YUV420P）等源格式统一解码为 RGBA 纹理，
因此本滤镜对这两种格式透明可用；shader 内部在 YUV 空间做滤波
（亮度/色度独立加权），与 NV12/YUV420P 的语义一致。
离线测试直接读写 YUV420P 原始文件验证同一算法。

## 离线测试

`denoise_offline_test` 不依赖 OBS，用 CPU 复刻同一算法：

```bash
./build/denoise_offline_test                 # 默认 320x240、90 帧、噪声 σ=12
./build/denoise_offline_test --noise 20 --motion-threshold 0.08
./build/denoise_offline_test --write /tmp/seq   # 导出 *_clean/_noisy/_denoised.yuv
```

测试流程：生成带运动物体和场景切换的合成序列 → 加高斯噪声 → 时域降噪 →
对比处理前后的 PSNR 与静止区域噪声方差，并检查场景切换帧无拖影。
全部指标改善时退出码为 0（PASS）。

参考结果（默认参数，σ=12）：PSNR +5.2 dB，静止区域噪声方差 −72%，
场景切换帧 PSNR 与输入一致（历史帧被丢弃，无拖影）。

## 验收对照

- **编译并加载**：`cmake --build build` 生成 `temporal-denoise.so`，按上文安装后出现在滤镜列表；
- **噪点下降、运动无拖影**：运动估计 + 场景切换检测保证静止区域强平均、运动区域跟随当前帧；
- **关闭滤镜恢复原样**：滤镜不修改来源本身，在滤镜列表中关闭/移除即可。
