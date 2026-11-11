#!/bin/bash
# 打成一个 .bin 固件包：从 0x0 整片刷入，同时也能直接用于设备网页里的 OTA 升级。
# 用法: package.sh <编译输出目录> <版本号> <目标目录>
#   APP_MAX 单槽容量，默认 1835008（对应 code/partitions.csv 的 app0/app1）
# 产物: <目标目录>/sms_forwarding.bin（只有一个文件，没有别的分片）
set -euo pipefail

BUILD_DIR=${1:?编译输出目录}
VER=${2:?版本号}
OUT=${3:-dist}
APP_MAX=${APP_MAX:-1835008}

APP="$BUILD_DIR/code.ino.bin"
BOOT="$BUILD_DIR/code.ino.bootloader.bin"
PART="$BUILD_DIR/code.ino.partitions.bin"

# boot_app0.bin 的位置随 arduino-cli 版本不同：新版拷到 build 目录，老版只在 core 里
BOOT_APP0=""
for c in "$BUILD_DIR/boot_app0.bin" \
         "$HOME/.arduino15/packages/esp32/hardware/esp32"/*/tools/partitions/boot_app0.bin; do
  [ -f "$c" ] && { BOOT_APP0="$c"; break; }
done

for f in "$APP" "$BOOT" "$PART"; do
  [ -f "$f" ] || { echo "缺少编译产物: $f" >&2; exit 1; }
done
[ -n "$BOOT_APP0" ] || { echo "找不到 boot_app0.bin" >&2; exit 1; }

SIZE=$(stat -c%s "$APP")
if [ "$SIZE" -gt "$APP_MAX" ]; then
  echo "app $SIZE 字节超过单槽容量 $APP_MAX，OTA 会失败" >&2
  exit 1
fi

# 找 esptool：优先 PATH，其次 arduino-esp32 的 tools 目录
ESPTOOL=$(command -v esptool || command -v esptool.py || true)
if [ -z "$ESPTOOL" ]; then
  ESPTOOL=$(ls "$HOME"/.arduino15/packages/esp32/tools/esptool_py/*/esptool 2>/dev/null | tail -1 || true)
fi
[ -n "$ESPTOOL" ] || { echo "找不到 esptool" >&2; exit 1; }

mkdir -p "$OUT"
# 关键：不填充到 4MB，成品字节数 = 0x10000 + app 大小。
# 这样设备端 OTA 可以靠「大小超过单槽容量」判定这是整包，跳过前 64KB 直接写应用区。
"$ESPTOOL" --chip esp32c3 merge-bin \
  -o "$OUT/sms_forwarding.bin" \
  --flash-mode dio --flash-freq 80m \
  0x0 "$BOOT" 0x8000 "$PART" 0xe000 "$BOOT_APP0" 0x10000 "$APP" >/dev/null

OUT_SIZE=$(stat -c%s "$OUT/sms_forwarding.bin")
if [ "$OUT_SIZE" -ne $((0x10000 + SIZE)) ]; then
  echo "合并结果 $OUT_SIZE 字节，预期 $((0x10000 + SIZE))，布局不对" >&2
  exit 1
fi

# 在线刷写页用的清单（放在 Pages 的 docs/fw/ 下）
BASE=${BASE_URL:-.}
cat > "$OUT/manifest.json" <<JSON
{
  "name": "短信转发器 (ESP32-C3 + ML307R)",
  "version": "${VER}",
  "builds": [
    {
      "chipFamily": "ESP32-C3",
      "parts": [
        { "path": "${BASE}/sms_forwarding.bin", "offset": 0 }
      ]
    }
  ]
}
JSON

echo "==> $OUT/sms_forwarding.bin  版本 $VER  app $SIZE/$APP_MAX 字节 ($((SIZE * 100 / APP_MAX))%)  整包 $OUT_SIZE 字节"
