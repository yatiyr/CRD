# Dependency and license manifest (GENERATED -- do not edit)

> Emitted by `scripts/gen_license_manifest.py` from [cmake/pins.json](../../cmake/pins.json), the registry
> that names every external input once with its version, source, SHA-256 and license
> ([pinned inputs](../design/pinned-inputs.md)). Regenerate with `python scripts/gen_license_manifest.py`;
> `--check` fails on drift. A manifest supplements review, it does not replace it: a new dependency is a
> registry edit, a license reading and a reviewed commit ([contribution routes](../CONTRIBUTING.md)).

- packages: **44** · tools: **50** · workflow actions: **6** · runner images: **6** · unpinned apt packages: **24**
- Demo assets keep their own terms: [assets/source/LICENSES.md](../../assets/source/LICENSES.md).
- `external/` is git-ignored and never shipped: locally built peer oracles for benchmarks, each under its
  upstream license, outside this registry and outside every build of the engine.

## Packages (CPM, commit-addressed archives, verified before extraction)

| Package | Version | License | Source | Archive |
|---|---|---|---|---|
| Catch2 | 3.7.1 | BSL-1.0 | catchorg/Catch2 @ `v3.7.1` (fa43b77429ba) | `Catch2-fa43b774.tar.gz` |
| glfw | 3.4 | Zlib | glfw/glfw @ `3.4` (7b6aead9fb88) | `glfw-7b6aead9.tar.gz` |
| zstd | 1.5.7 | BSD-3-Clause OR GPL-2.0-only, taken under BSD-3-Clause | facebook/zstd @ `v1.5.7` (f8745da6ff1a) | `zstd-f8745da6.tar.gz` |
| aws-lc | 5.12.0 | Apache-2.0 AND ISC AND MIT AND BSD-3-Clause | aws/aws-lc @ `v5.12.0` (cd481c207e38) | `aws-lc-cd481c20.tar.gz` |
| openssl | 4.0.3 | Apache-2.0 | openssl/openssl @ `openssl-4.0.3` (af1775b60dfa) | `openssl-4.0.3.tar.gz` |
| boringssl | 0.20261020.0 | Apache-2.0 | google/boringssl @ `0.20261020.0` (d3bf4795ad5b) | `boringssl-d3bf4795.tar.gz` |
| nlohmann_json | 3.12.0 | MIT | nlohmann/json @ `v3.12.0` (55f93686c015) | `json-55f93686.tar.gz` |
| tpm2-tss | 4.0.1 | BSD-2-Clause | tpm2-software/tpm2-tss @ `4.0.1` (8b404ee7e588) | `tpm2-tss-8b404ee7.tar.gz` |
| pkcs11 | 3.2 | OASIS-IPR (RF on RAND) | oasis-tcs/pkcs11 @ `master` (c55388c02702) | `pkcs11-c55388c0.tar.gz` |
| ngtcp2 | 1.26.0 | MIT | ngtcp2/ngtcp2 @ `v1.26.0` (6a22a4d7a111) | `ngtcp2-6a22a4d7.tar.gz` |
| nghttp3 | 1.19.0 | MIT | ngtcp2/nghttp3 @ `v1.19.0` (dc83309ca5fd) | `nghttp3-1.19.0.tar.gz` |
| qifs | da52cd93 | MIT | qpackers/qifs (da52cd936b3e) | `qifs-da52cd93.tar.gz` |
| mlspp | 0.1.0-fc724c31 | BSD-2-Clause AND BSL-1.0 | cisco/mlspp @ `main` (fc724c3100ce) | `mlspp-fc724c31.tar.gz` |
| nasm | 2.16.03 | BSD-2-Clause | https://www.nasm.us/pub/nasm/releasebuilds/2.16.03/win64/nasm-2.16.03-win64.zip | `nasm-2.16.03-win64.zip` |
| wycheproof | d17cc89 | Apache-2.0 | C2SP/wycheproof @ `d17cc89fa5c8f4f4d378b5385ebf75da5d0926c9` (d17cc89fa5c8) | `wycheproof-d17cc89f.tar.gz` |
| mls-vectors | 2034fe7 | IETF Trust (BCP 78) | mlswg/mls-implementations @ `2034fe72e0c805a683ba76165660c834f2a8142a` (2034fe72e0c8) | `mls-implementations-2034fe72.tar.gz` |
| hpke-pq-vectors | 6433c8f | IETF Trust (BCP 78) | hpkewg/hpke-pq @ `6433c8fce0b8b749dfc86c1095081a88698ccfab` (6433c8fce0b8) | `hpke-pq-6433c8fc.tar.gz` |
| stb | 2c980bb | MIT OR Unlicense | nothings/stb @ `2c980bb59875b0d32144a71867fbdebb2f77cd20` (2c980bb59875) | `stb-2c980bb5.tar.gz` |
| cgltf | 1.14 | MIT | jkuhlmann/cgltf @ `v1.14` (52c23814dbb6) | `cgltf-52c23814.tar.gz` |
| MikkTSpace | 3e895b4 | Zlib | mmikk/MikkTSpace @ `3e895b49d05ea07e4c2133156cfa94369e19e409` (3e895b49d05e) | `MikkTSpace-3e895b49.tar.gz` |
| imgui | 1.92.0-docking | MIT | ocornut/imgui @ `v1.92.0-docking` (adfa5364cd84) | `imgui-adfa5364.tar.gz` |
| eigen | 3.4.0 | MPL-2.0 | gitlab.com/libeigen/eigen @ `3.4.0` | `eigen-3.4.0.tar.gz` |
| OpenBLAS | 0.3.27 | BSD-3-Clause | OpenMathLib/OpenBLAS @ `v0.3.27` (ce3f668c992c) | `OpenBLAS-ce3f668c.tar.gz` |
| boost | 1.92.0 | BSL-1.0 | boostorg/boost @ `boost-1.92.0` | `boost_1_92_0.tar.bz2` |
| hptt | 9425386 | BSD-3-Clause | springer13/hptt @ `942538649b51ff14403a0c73a35d9825eab2d7de` (942538649b51) | `hptt-94253864.tar.gz` |
| pocketfft | 4d865cb | BSD-3-Clause | mreineck/pocketfft @ `4d865cba24359b05b37da4e75aee24498c3b78e7` (4d865cba2435) | `pocketfft-4d865cba.tar.gz` |
| isa-l | 2.32.1 | BSD-3-Clause | intel/isa-l @ `v2.32.1` (7c3479e0a9da) | `isa-l-7c3479e0.tar.gz` |
| ntp | 4.2.8p18 | NTP AND BSD-2-Clause | https://downloads.nwtime.org/ntp/4.2.8/ntp-4.2.8p18.tar.gz | `ntp-4.2.8p18.tar.gz` |
| nanorq | 0-7d52fbeb | MIT | sleepybishop/nanorq (7d52fbebdc89) | `nanorq-7d52fbeb.tar.gz` |
| serialize | 2.1.3 | BSD-3-Clause | mas-bandwidth/serialize @ `v2.1.3` (7bf1877b38cf) | `serialize-7bf1877b.tar.gz` |
| bitsery | 5.2.5 | MIT | fraillt/bitsery @ `v5.2.5` (9bebfd4911b8) | `bitsery-9bebfd49.tar.gz` |
| zpp_bits | 4.7.6 | MIT | eyalz800/zpp_bits @ `v4.7.6` (4a57ecd68a48) | `zpp_bits-4a57ecd6.tar.gz` |
| protozero | 1.8.2 | BSD-2-Clause | mapbox/protozero @ `v1.8.2` (af2f6e274e0a) | `protozero-af2f6e27.tar.gz` |
| flatbuffers | 25.12.19 | Apache-2.0 | google/flatbuffers @ `v25.12.19` (7e163021e59c) | `flatbuffers-7e163021.tar.gz` |
| capnproto | 1.5.0 | MIT | capnproto/capnproto @ `v1.5.0` (373e61ec89e2) | `capnproto-373e61ec.tar.gz` |
| networked-physics-snapshot | fe19b74b | BSD-3-Clause | mas-bandwidth/networked-physics-gdc-2015 (fe19b74bf78a) | `Snapshot.h` |
| vectorial-quat4f | fe19b74b | BSD-2-Clause | mas-bandwidth/networked-physics-gdc-2015 (fe19b74bf78a) | `quat4f.h` |
| kdis | 97f8e86a | BSD-2-Clause | karljj1/kdis (97f8e86a4c68) | `kdis-97f8e86a.tar.gz` |
| openfec | 1.4.2.12 | CeCILL-C | roc-streaming/openfec @ `v1.4.2.12` (e549f3b8ba54) | `openfec-e549f3b8.tar.gz` |
| highway | 1.4.0 | Apache-2.0 OR BSD-3-Clause | google/highway @ `1.4.0` (2607d3b5b011) | `highway-2607d3b5.tar.gz` |
| tracy | 0.14.1 | BSD-3-Clause | wolfpld/tracy @ `v0.14.1` (30997d5ca6bb) | `tracy-30997d5c.tar.gz` |
| nlopt | 2.11.0 | MIT | stevengj/nlopt @ `v2.11.0` (88c424d4f458) | `nlopt-88c424d4.tar.gz` |
| ggml | 0.28.0 | MIT | ggml-org/ggml @ `v0.28.0` (facdc44a67da) | `ggml-facdc44a.tar.gz` |
| vkfft | 1.3.4 | MIT | DTolm/VkFFT @ `v1.3.4` (066a17c17068) | `VkFFT-066a17c1.tar.gz` |

## Tools (installers, SDK members and helpers, verified by digest)

| Tool | Version | License | Source | File |
|---|---|---|---|---|
| cpm | 0.40.2 | MIT | github.com | `CPM_0.40.2.cmake` |
| vulkan-sdk-windows | 1.4.341.1 | LunarG Vulkan SDK License Agreement | sdk.lunarg.com | `vulkansdk-windows-X64-1.4.341.1.exe` |
| vulkan-sdk-linux | 1.4.341.1 | LunarG Vulkan SDK License Agreement | sdk.lunarg.com | `vulkansdk-linux-x86_64-1.4.341.1.tar.xz` |
| vulkan-headers | vulkan-sdk-1.4.341.0 | Apache-2.0 OR MIT | KhronosGroup/Vulkan-Headers (b5c8f996196b) | `Vulkan-Headers-b5c8f996.tar.gz` |
| spirv-reflect | vulkan-sdk-1.4.341.0 | Apache-2.0 | KhronosGroup/SPIRV-Reflect (62ca825c6923) | `spirv_reflect.h`, `spirv_reflect.c` |
| spirv-headers | vulkan-sdk-1.4.341.0 | MIT | KhronosGroup/SPIRV-Headers (04f10f650d51) | `include/spirv/unified1/spirv.h` |
| warp | 1.0.20 | Microsoft Software License Terms (Microsoft.Direct3D.WARP) | api.nuget.org | `microsoft.direct3d.warp.1.0.20.nupkg` |
| actionlint | 1.7.12 | MIT | github.com | `actionlint_1.7.12_linux_amd64.tar.gz` |
| sccache-windows | 0.17.0 | Apache-2.0 | github.com | `sccache-v0.17.0-x86_64-pc-windows-msvc.zip` |
| sccache-linux | 0.17.0 | Apache-2.0 | github.com | `sccache-v0.17.0-x86_64-unknown-linux-musl.tar.gz` |
| sde-windows | 10.13.1 | Intel Simplified Software License (Licenses/; third-party programs in Licenses/third-party-programs.txt) | downloadmirror.intel.com | `sde-external-10.13.1-2026-07-28-win.tar.xz` |
| sde-linux | 10.13.1 | Intel Simplified Software License (Licenses/; third-party programs in Licenses/third-party-programs.txt) | downloadmirror.intel.com | `sde-external-10.13.1-2026-07-28-lin.tar.xz` |
| mold-linux | 3.0.0 | MIT | github.com | `mold-3.0.0-x86_64-linux.tar.gz` |
| clang-build-analyzer | 1.6.0 | Unlicense | aras-p/ClangBuildAnalyzer (bae0cb488cce) | `ClangBuildAnalyzer-bae0cb48.tar.gz` |
| clang-tidy-linux | 20.1.0 | Apache-2.0 WITH LLVM-exception AND Apache-2.0 | files.pythonhosted.org | `clang_tidy-20.1.0-py2.py3-none-manylinux_2_17_x86_64.manylinux2014_x86_64.whl` |
| tracy-capture-windows | 0.14.1 | BSD-3-Clause | github.com | `windows-0.14.1.zip` |
| tracy-csvexport-windows | 0.14.1 | BSD-3-Clause | github.com | `windows-0.14.1.zip` |
| ninja-windows | 1.11.1 | Apache-2.0 | github.com | `ninja-win.zip` |
| ninja-linux | 1.11.1 | Apache-2.0 | github.com | `ninja-linux.zip` |
| ninja-macos | 1.11.1 | Apache-2.0 | github.com | `ninja-mac.zip` |
| llvm | 20.1.8 | Apache-2.0 WITH LLVM-exception | github.com | `clang+llvm-20.1.8-x86_64-pc-windows-msvc.tar.xz` |
| genmc | 0.19.0 | Apache-2.0 OR MIT | MPI-SWS/genmc (9f6c4c0772d0) | `genmc-9f6c4c07.tar.gz` |
| node-linux-x64 | 24.22.0 | MIT | nodejs.org | `node-v24.22.0-linux-x64.tar.xz` |
| rust-linux-x64 | 1.99.1 | MIT OR Apache-2.0 | static.rust-lang.org | `rustc-1.99.1-x86_64-unknown-linux-gnu.tar.xz`, `rust-std-1.99.1-x86_64-unknown-linux-gnu.tar.xz`, `cargo-1.99.1-x86_64-unknown-linux-gnu.tar.xz`, `clippy-1.99.1-x86_64-unknown-linux-gnu.tar.xz` |
| gnome-keyring-linux-x64 | 46.1-2ubuntu0.2 | GPL-2.0-or-later AND LGPL-2.1-or-later | launchpad.net | `gnome-keyring_46.1-2ubuntu0.2_amd64.deb`, `libgck-1-0_3.41.2-1build3_amd64.deb`, `libgcr-base-3-1_3.41.2-1build3_amd64.deb` |
| swtpm-linux-x64 | 0.7.3-0ubuntu5.24.04.1 | BSD-3-Clause AND BSD-2-Clause | launchpad.net | `swtpm_0.7.3-0ubuntu5.24.04.1_amd64.deb`, `libtpms0_0.9.3-0ubuntu4.24.04.1_amd64.deb`, `libtss2-esys-3.0.2-0t64_4.0.1-7.1ubuntu5.1_amd64.deb`, `libtss2-sys1t64_4.0.1-7.1ubuntu5.1_amd64.deb`, `libtss2-mu-4.0.1-0t64_4.0.1-7.1ubuntu5.1_amd64.deb`, `libtss2-tctildr0t64_4.0.1-7.1ubuntu5.1_amd64.deb`, `libtss2-tcti-swtpm0t64_4.0.1-7.1ubuntu5.1_amd64.deb`, `libtss2-tcti-pcap0t64_4.0.1-7.1ubuntu5.1_amd64.deb` |
| softhsm-linux-x64 | 2.6.1-2.2ubuntu3 | BSD-2-Clause AND LGPL-2.1-or-later | launchpad.net | `softhsm2_2.6.1-2.2ubuntu3_amd64.deb`, `softhsm2-common_2.6.1-2.2ubuntu3_amd64.deb`, `libsofthsm2_2.6.1-2.2ubuntu3_amd64.deb`, `opensc-pkcs11_0.25.0~rc1-1ubuntu0.2_amd64.deb` |
| nasm-linux-x64 | 2.16.01-1build1 | BSD-2-Clause | launchpad.net | `nasm_2.16.01-1build1_amd64.deb` |
| tamarin-linux-x64 | 1.12.0 | GPL-3.0-only AND GPL-2.0-or-later | github.com | `tamarin-prover-1.12.0-linux64-ubuntu.tar.gz`, `Maude-3.5.1-linux-x86_64.zip` |
| android-ndk-linux-x64 | 30.0.16248370 | LicenseRef-Android-SDK-License | dl.google.com | `android-ndk-r30-linux.zip` |
| android-platform-tools-linux | 37.0.1 | LicenseRef-Android-SDK-License | dl.google.com | `platform-tools_r37.0.1-linux.zip` |
| android-emulator-linux-x64 | 37.2.13 | LicenseRef-Android-SDK-License | dl.google.com | `emulator-linux_x64-16740729.zip` |
| android-system-image-31-x86_64 | 31-r05 | LicenseRef-Android-SDK-License | dl.google.com | `x86_64-31_r05.zip` |
| android-build-tools-linux | 37.0.0 | LicenseRef-Android-SDK-License | dl.google.com | `build-tools_r37_linux.zip` |
| android-platform-31 | 31-r01 | LicenseRef-Android-SDK-License | dl.google.com | `platform-31_r01.zip` |
| temurin-jdk-21-linux-x64 | 21.0.13+9 | GPL-2.0-only WITH Classpath-exception-2.0 | github.com | `OpenJDK21U-jdk_x64_linux_hotspot_21.0.13_9.tar.gz` |
| raptorq-crates | 2.0.1 | Apache-2.0 | cberner/raptorq (29253866ed07) | the crates `tests/numerics/hesap-net/raptorq_crate_peer/Cargo.lock` names |
| openmls-crates | 0.9.2 | MIT | openmls/openmls (8a8f83b4b61f) | the crates `tests/foundation/crypto/interop/openmls_peer/Cargo.lock` names |
| quiche-crates | 0.30.0 | BSD-2-Clause | cloudflare/quiche (be47c5011215) | the crates `tests/foundation/net/interop/quiche_peer/Cargo.lock` names |
| web-transport-quinn-crates | 0.13.6 | MIT OR Apache-2.0 | moq-dev/web-transport (c878557faabd) | the crates `tests/foundation/net/interop/webtransport_peer/Cargo.lock` names |
| qmux-crates | 0.6.6 | MIT OR Apache-2.0 | moq-dev/web-transport (ef8abdccdc0a) | the crates `tests/foundation/net/interop/qmux_crate_peer/Cargo.lock` names |
| cuda-linux-x64 | 13.3.1 | LicenseRef-NVIDIA-CUDA-Toolkit-EULA | developer.download.nvidia.com | `cuda_nvcc-linux-x86_64-13.3.73-archive.tar.xz`, `cuda_crt-linux-x86_64-13.3.73-archive.tar.xz`, `libnvvm-linux-x86_64-13.3.73-archive.tar.xz`, `cuda_cudart-linux-x86_64-13.3.29-archive.tar.xz`, `cccl-linux-x86_64-13.3.3.4.1-archive.tar.xz`, `cuda_nvrtc-linux-x86_64-13.3.33-archive.tar.xz`, `libnvjitlink-linux-x86_64-13.3.33-archive.tar.xz`, `libcufft-linux-x86_64-12.3.0.29-archive.tar.xz`, `libcublas-linux-x86_64-13.6.0.2-archive.tar.xz` |
| bake-python-windows | 3.12.10 | PSF-2.0 | www.python.org | `python-3.12.10-amd64.exe` |
| bake-git-windows | 2.56.0.windows.4 | GPL-2.0-only | github.com | `Git-2.56.0.4-64-bit.exe` |
| bake-cmake-windows | 4.4.5 | BSD-3-Clause | github.com | `cmake-4.4.5-windows-x86_64.msi` |
| bake-cmake-linux-x64 | 4.4.5 | BSD-3-Clause | github.com | `cmake-4.4.5-linux-x86_64.tar.gz` |
| bake-cmake-linux-arm64 | 4.4.5 | BSD-3-Clause | github.com | `cmake-4.4.5-linux-aarch64.tar.gz` |
| bake-vs-buildtools-windows | 18 (stable channel bootstrapper) | Microsoft Visual Studio Build Tools license terms | download.visualstudio.microsoft.com | `vs_BuildTools.exe` |
| bake-gpu-driver-installer-windows | 9a05b735a56c45d87788720ed18ccca7205d224b | Apache-2.0 | raw.githubusercontent.com | `install_gpu_driver.ps1` |
| webrtc-bwe-sources | branch-heads/8147 | BSD-3-Clause | webrtc.googlesource.com | `LICENSE`, `PATENTS`, `modules/congestion_controller/goog_cc/trendline_estimator.cc`, `modules/congestion_controller/goog_cc/trendline_estimator.h`, `modules/congestion_controller/goog_cc/bitrate_estimator.cc`, `modules/congestion_controller/goog_cc/bitrate_estimator.h`, `modules/congestion_controller/goog_cc/inter_arrival_delta.cc`, `modules/congestion_controller/goog_cc/inter_arrival_delta.h`, `modules/congestion_controller/goog_cc/delay_increase_detector_interface.h`, `modules/remote_bitrate_estimator/overuse_estimator.cc`, `modules/remote_bitrate_estimator/overuse_estimator.h`, `modules/remote_bitrate_estimator/overuse_detector.cc`, `modules/remote_bitrate_estimator/overuse_detector.h`, `api/network_state_predictor.h`, `api/transport/bandwidth_usage.h`, `api/units/data_rate.h`, `api/units/data_size.h`, `api/units/frequency.h`, `api/units/time_delta.h`, `api/units/timestamp.h`, `rtc_base/units/unit_base.h`, `rtc_base/numerics/divide_round.h`, `rtc_base/numerics/safe_compare.h`, `rtc_base/numerics/safe_conversions.h`, `rtc_base/numerics/safe_conversions_impl.h`, `rtc_base/numerics/safe_minmax.h`, `rtc_base/type_traits.h`, `rtc_base/system/rtc_export.h`, `rtc_base/rtp_to_ntp_estimator.h`, `rtc_base/rtp_to_ntp_estimator.cc`, `rtc_base/timestamp_aligner.h`, `rtc_base/timestamp_aligner.cc`, `rtc_base/time_utils.h`, `rtc_base/numerics/sequence_number_unwrapper.h`, `api/numerics/sequence_number_unwrapper.h`, `rtc_base/numerics/sequence_number_util.h`, `rtc_base/numerics/mod_ops.h`, `system_wrappers/include/ntp_time.h`, `api/make_ref_counted.h`, `api/ref_count.h`, `api/scoped_refptr.h`, `modules/audio_coding/neteq/packet.cc`, `modules/audio_coding/neteq/packet.h`, `modules/audio_coding/neteq/red_payload_splitter.cc`, `modules/audio_coding/neteq/red_payload_splitter.h`, `modules/include/module_common_types_public.h`, `modules/include/module_fec_types.h`, `modules/rtp_rtcp/source/byte_io.h`, `modules/rtp_rtcp/source/fec_private_tables_bursty.cc`, `modules/rtp_rtcp/source/fec_private_tables_bursty.h`, `modules/rtp_rtcp/source/fec_private_tables_random.cc`, `modules/rtp_rtcp/source/fec_private_tables_random.h`, `modules/rtp_rtcp/source/flexfec_03_header_reader_writer.cc`, `modules/rtp_rtcp/source/flexfec_03_header_reader_writer.h`, `modules/rtp_rtcp/source/flexfec_header_reader_writer.cc`, `modules/rtp_rtcp/source/flexfec_header_reader_writer.h`, `modules/rtp_rtcp/source/forward_error_correction.cc`, `modules/rtp_rtcp/source/forward_error_correction.h`, `modules/rtp_rtcp/source/forward_error_correction_internal.cc`, `modules/rtp_rtcp/source/forward_error_correction_internal.h`, `modules/rtp_rtcp/source/ulpfec_header_reader_writer.cc`, `modules/rtp_rtcp/source/ulpfec_header_reader_writer.h`, `rtc_base/buffer.h`, `rtc_base/byte_order.h`, `rtc_base/copy_on_write_buffer.cc`, `rtc_base/copy_on_write_buffer.h`, `rtc_base/ref_counted_object.h`, `rtc_base/ref_counter.h`, `rtc_base/span_helpers.h`, `rtc_base/system/arch.h`, `rtc_base/zero_memory.h`, `rtc_base/ref_counter.cc`, `rtc_base/zero_memory.cc` |

## Workflow actions (pinned to a reviewed commit)

| Action | Tag | Commit |
|---|---|---|
| actions/checkout | `v7.0.1` | `3d3c42e5aac5ba805825da76410c181273ba90b1` |
| actions/setup-python | `v7.1.0` | `6d4fd22b20465ccc0f7354476588e8df7c9ca0e0` |
| actions/cache | `v6.1.0` | `55cc8345863c7cc4c66a329aec7e433d2d1c52a9` |
| actions/upload-artifact | `v7.0.2` | `cf430e030ddbb5b0abf93d22962f4752f3646cd9` |
| actions/download-artifact | `v8.0.2` | `9000827ccba6bdab643e8b6fd33ac0654aef8333` |
| actions/github-script | `v8` | `ed597411d8f924073f98dfc5c65a23a2325f34cd` |

## Runner images

| Lane | Image |
|---|---|
| windows | `windows-2025` |
| linux | `ubuntu-24.04` |
| windows-native | `windows-2025-vs2026` |
| linux-arm64 | `ubuntu-24.04-arm` |
| windows-arm64 | `windows-11-arm` |
| macos | `macos-15-intel` |

## Unpinned inputs

OS packages of the Linux image, recorded with their installed versions in every Linux lane census: `libvulkan-dev`, `glslang-tools`, `libshaderc-dev`, `libwayland-dev`, `wayland-protocols`, `libxkbcommon-dev`, `libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libxi-dev`, `mesa-common-dev`, `mesa-vulkan-drivers`, `gdb`, `zlib1g-dev`, `dbus-daemon`, `dbus-bin`, `llvm-20-dev`, `clang-20`, `g++-14`, `libedit-dev`, `ninja-build`, `clang`, `libclang-rt-18-dev`.

OS-provided by the pinned runner image; the Linux lanes record the installed versions with dpkg-query into the lane census (llvm-20-dev, clang-20, g++-14 and libedit-dev only on the linux-clang-tsan entry, for the GenMC build; ninja-build, clang and libclang-rt-18-dev only on the linux-arm64 lanes, whose image runs no x86-64 pinned Ninja). The MSVC toolset comes from the pinned Windows image and is recorded by the evidence bundle. The own-CI images (ci/executors/gce_vm.py) take their apt packages at bake time, and their Windows image takes the Build Tools from the stable channel of the pinned bake-vs-buildtools-windows bootstrapper; each records the installed versions in its image census.
