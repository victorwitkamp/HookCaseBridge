# HookCaseBridge

A kernel extension that exports, under HookCase's bundle identifier `org.smichaud.HookCase`, the symbols the 2020 Tiger Lake graphics kexts (`AppleIntelTGLGraphicsFramebuffer` 16.0.0 and `AppleIntelTGLGraphics` 16.0.32) import from HookCase, and nothing else.

Exported: `AppleIntelBaseController::ReadRegister32`, `WriteRegister32`, `ReadRegister64`, `WriteRegister64`, `getPMTNow`, `hwSetupDSBMemory`; `AppleIntelPortHAL::probePortMode`; `AppleIntelDisplayPath::DSBEngineBusyStatus`; `strnstr`, `gpu_accumulate_time`, `gpu_describe`.

Build with `xcodebuild -project HookCaseBridge.xcodeproj -scheme HookCase -configuration Release`. Install `HookCase.kext` in `/Library/Extensions` beside the two graphics kexts, rebuild the auxiliary kernel collection (`kmutil install --volume-root / --update-all`) and approve it in System Settings.

MIT licence: see LICENSE.
