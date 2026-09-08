#!/usr/bin/env bash

set -Eeuo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_root="$(cd "$script_dir/../.." && pwd)"
log_dir="${DARTPLANT_EMULATOR_LOG_DIR:-build/ci/emulator}"
boot_timeout="${DARTPLANT_EMULATOR_BOOT_TIMEOUT:-1200}"
api_level="${DARTPLANT_ANDROID_API:-31}"
system_image_tag="${DARTPLANT_ANDROID_TAG:-aosp_atd}"
avd_name="${DARTPLANT_AVD_NAME:-dartplant-arm64-ci}"
require_kvm="${DARTPLANT_REQUIRE_KVM:-0}"
system_image="system-images;android-${api_level};${system_image_tag};arm64-v8a"
aarch64_emulator_url="${DARTPLANT_AARCH64_EMULATOR_URL:-https://github.com/Changqing-JING/android-emulator-aarch64-linux/releases/download/v2.12.0-19097-g85fa07f04ef/sdk-repo-linux_aarch64-emulator-standalone-0.zip}"
aarch64_emulator_sha256="${DARTPLANT_AARCH64_EMULATOR_SHA256:-67ce4f576687b067d9b36668405b86b9749745d67d31037616aac12c49c63a65}"
aarch64_emulator_revision="${DARTPLANT_AARCH64_EMULATOR_REVISION:-35.6.3}"
aarch64_metrics_sha256="${DARTPLANT_AARCH64_METRICS_SHA256:-7eb64f1740dc5800f66b2422e79251a9de60abcd4cff1567e725f3a3c9ac4068}"
cmdline_tools_revision="${DARTPLANT_ANDROID_CMDLINE_TOOLS_REVISION:-15859902}"
cmdline_tools_sha256="${DARTPLANT_ANDROID_CMDLINE_TOOLS_SHA256:-4e4c464f145a7512b57d088ac6c278c03c9eea610886b35a5e0804e74eedf583}"

mkdir -p "$log_dir"
exec > >(tee "$log_dir/probe.log") 2>&1

echo "== DartPlant ARM64 Android emulator probe =="
date -u '+utc=%Y-%m-%dT%H:%M:%SZ'
echo "host_uname=$(uname -a)"
echo "host_machine=$(uname -m)"
echo "kvm=$(if [[ -c /dev/kvm ]]; then echo present; else echo absent; fi)"
if [[ -e /dev/kvm ]]; then
  ls -l /dev/kvm
fi
lscpu || true

if [[ "$(uname -m)" != "aarch64" ]]; then
  echo "error: this probe is authoritative only on an AArch64 Linux host" >&2
  exit 2
fi

if [[ "$require_kvm" == "1" && ! ( -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ) ]]; then
  echo "infrastructure unavailable: authoritative CI requires usable /dev/kvm on the ARM64 host" >&2
  exit 11
fi

android_home="${DARTPLANT_ANDROID_HOME:-${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/android-sdk}}}"
export ANDROID_HOME="$android_home"
export ANDROID_SDK_ROOT="$android_home"

cmdline_marker="$android_home/cmdline-tools/latest/.dartplant-revision"
if [[ -x "$android_home/cmdline-tools/latest/bin/sdkmanager" ]] && \
   [[ -f "$cmdline_marker" ]] && \
   [[ "$(cat "$cmdline_marker")" == "$cmdline_tools_revision" ]]; then
  export PATH="$android_home/cmdline-tools/latest/bin:$PATH"
fi

if ! command -v sdkmanager >/dev/null 2>&1 || \
   [[ ! -f "$cmdline_marker" ]] || \
   [[ "$(cat "$cmdline_marker" 2>/dev/null || true)" != "$cmdline_tools_revision" ]]; then
  echo "sdkmanager is unavailable; bootstrapping pinned Android Command-Line Tools"
  mkdir -p "$android_home/cmdline-tools"
  archive="$(mktemp -t dartplant-commandlinetools-XXXXXX.zip)"
  tools_dir="$(mktemp -d -t dartplant-commandlinetools-XXXXXX)"
  trap 'rm -f "$archive"; rm -rf "$tools_dir"' EXIT
  curl -fL --retry 3 --retry-delay 2 \
    "https://dl.google.com/android/repository/commandlinetools-linux-${cmdline_tools_revision}_latest.zip" \
    -o "$archive"
  printf '%s  %s\n' "$cmdline_tools_sha256" "$archive" | sha256sum -c -
  python3 - "$archive" "$tools_dir" <<'PY'
import sys
import zipfile

archive, destination = sys.argv[1:]
with zipfile.ZipFile(archive) as package:
    package.extractall(destination)
PY
  rm -rf "$android_home/cmdline-tools/latest"
  mkdir -p "$android_home/cmdline-tools/latest"
  cp -a "$tools_dir/cmdline-tools/." "$android_home/cmdline-tools/latest/"
  # Python's ZipFile extractor does not preserve the executable mode bits
  # carried by Google's archive. Restore them explicitly before invoking the
  # command-line tools on a minimal hosted runner.
  chmod +x "$android_home/cmdline-tools/latest/bin/"*
  printf '%s\n' "$cmdline_tools_revision" >"$cmdline_marker"
  export PATH="$android_home/cmdline-tools/latest/bin:$PATH"
fi

if ! command -v sdkmanager >/dev/null 2>&1; then
  echo "error: sdkmanager remains unavailable after command-line-tools bootstrap" >&2
  exit 3
fi
echo "sdkmanager=$(command -v sdkmanager)"
sdkmanager --version

is_aarch64_elf() {
  python3 - "$1" <<'PYELF'
import pathlib
import struct
import sys

path = pathlib.Path(sys.argv[1])
data = path.read_bytes()[:64]
if len(data) < 20 or data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
    raise SystemExit(1)
machine = struct.unpack_from("<H", data, 18)[0]
raise SystemExit(0 if machine == 183 else 1)
PYELF
}

install_pinned_aarch64_emulator() {
  echo "installing pinned Linux-AArch64 Android Emulator"
  local archive extract_dir
  archive="$(mktemp -t dartplant-aarch64-emulator-XXXXXX.zip)"
  extract_dir="$(mktemp -d -t dartplant-aarch64-emulator-XXXXXX)"
  curl -fL --retry 3 --retry-delay 2 "$aarch64_emulator_url" -o "$archive"
  printf '%s  %s\n' "$aarch64_emulator_sha256" "$archive" | sha256sum -c -
  python3 - "$archive" "$extract_dir" <<'PYZIP'
import os
import pathlib
import stat
import sys
import zipfile

archive, destination = sys.argv[1:]
root = pathlib.Path(destination).resolve()
with zipfile.ZipFile(archive) as package:
    for info in package.infolist():
        target = (root / info.filename).resolve()
        if root not in target.parents and target != root:
            raise SystemExit(f"unsafe archive member: {info.filename}")
        if info.is_dir():
            target.mkdir(parents=True, exist_ok=True)
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        with package.open(info) as src, target.open("wb") as dst:
            dst.write(src.read())
        mode = (info.external_attr >> 16) & 0xFFFF
        if stat.S_ISREG(mode) and mode & 0o777:
            os.chmod(target, mode & 0o777)
PYZIP
  rm -rf "$android_home/emulator"
  cp -a "$extract_dir/emulator" "$android_home/emulator"
  rm -f "$archive"
  rm -rf "$extract_dir"

  local revision_major revision_minor revision_micro
  IFS=. read -r revision_major revision_minor revision_micro <<<"$aarch64_emulator_revision"
  cat >"$android_home/emulator/package.xml" <<EOF
<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<ns2:repository xmlns:ns2="http://schemas.android.com/repository/android/common/01" xmlns:ns5="http://schemas.android.com/repository/android/generic/01">
  <localPackage path="emulator" obsolete="false">
    <type-details xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:type="ns5:genericDetailsType"/>
    <revision><major>${revision_major}</major><minor>${revision_minor}</minor><micro>${revision_micro}</micro></revision>
    <display-name>Android Emulator (pinned Linux AArch64 CI build)</display-name>
  </localPackage>
</ns2:repository>
EOF
  printf '%s\n' "$aarch64_emulator_sha256" >"$android_home/emulator/.dartplant-sha256"
}

emulator_bin="$android_home/emulator/emulator"
qemu_aarch64_bin="$android_home/emulator/qemu/linux-aarch64/qemu-system-aarch64"
emulator_marker="$android_home/emulator/.dartplant-sha256"
if [[ ! -x "$emulator_bin" || ! -x "$qemu_aarch64_bin" ]] || \
   [[ ! -f "$emulator_marker" ]] || \
   [[ "$(cat "$emulator_marker" 2>/dev/null || true)" != "$aarch64_emulator_sha256" ]] || \
   ! is_aarch64_elf "$emulator_bin" || ! is_aarch64_elf "$qemu_aarch64_bin"; then
  install_pinned_aarch64_emulator
fi
if ! is_aarch64_elf "$emulator_bin" || ! is_aarch64_elf "$qemu_aarch64_bin"; then
  echo "error: emulator host executables are not ELF64 AArch64" >&2
  exit 4
fi
metrics_lib="$android_home/emulator/lib64/libandroid-emu-metrics.so"
metrics_sha="$(sha256sum "$metrics_lib" 2>/dev/null | awk '{print $1}')"
if [[ "$metrics_sha" != "$aarch64_metrics_sha256" ]]; then
  # A failed prior probe may have left a locally modified emulator tree. The
  # cache identity is the archive SHA, so restore the exact pinned distribution
  # before applying the runtime-only symbol interposition below.
  echo "restoring pinned emulator because metrics provenance changed: $metrics_sha"
  install_pinned_aarch64_emulator
  metrics_sha="$(sha256sum "$metrics_lib" 2>/dev/null | awk '{print $1}')"
fi
if [[ "$metrics_sha" != "$aarch64_metrics_sha256" ]]; then
  echo "error: pinned emulator metrics checksum mismatch: $metrics_sha" >&2
  exit 15
fi
{
  echo "emulator_source_url=$aarch64_emulator_url"
  echo "emulator_sha256=$aarch64_emulator_sha256"
  echo "emulator_revision=$aarch64_emulator_revision"
  echo "metrics_sha256=$aarch64_metrics_sha256"
  echo "emulator_elf=AArch64"
  echo "qemu_system_aarch64_elf=AArch64"
} | tee "$log_dir/emulator-provenance.txt"

yes | sdkmanager --licenses >/dev/null 2>&1 || true
sdkmanager \
  "platforms;android-${api_level}" \
  "$system_image"

ptrace_scope_original="$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo unknown)"
ptrace_scope="$ptrace_scope_original"
ptrace_workaround="none"
echo "kernel.yama.ptrace_scope.original=$ptrace_scope_original"

if [[ "$ptrace_scope_original" =~ ^[1-9][0-9]*$ ]]; then
  # The Linux-AArch64 emulator's crash handler ptraces the QEMU process during
  # startup. GitHub-hosted runners are ephemeral and provide passwordless sudo,
  # so prefer removing YAMA's ptrace restriction for this job over wrapping QEMU
  # in a competing debugger tracer. The latter still leaves crashpad's child
  # ptrace attempt failing with EPERM on GitHub's ARM runner.
  if sudo sysctl -w kernel.yama.ptrace_scope=0; then
    ptrace_scope="$(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null || echo unknown)"
    if [[ "$ptrace_scope" == "0" ]]; then
      ptrace_workaround="sysctl"
    fi
  fi
fi
echo "kernel.yama.ptrace_scope.effective=$ptrace_scope"

host_packages=()
if ! command -v adb >/dev/null 2>&1 || ! adb version >/dev/null 2>&1; then
  echo "Google platform-tools adb is unavailable on this host; using Ubuntu's native AArch64 adb"
  host_packages+=(adb)
fi
if [[ "$ptrace_scope" != "0" ]] && ! command -v gdb >/dev/null 2>&1; then
  # The Linux-AArch64 emulator's crash-handler startup path self-ptraces.
  # YAMA ptrace restrictions make that path crash on Ubuntu hosts; keeping a
  # debugger attached causes the handler to take its already-traced path.
  host_packages+=(gdb)
fi
if ! command -v cc >/dev/null 2>&1; then
  host_packages+=(build-essential)
fi
if (( ${#host_packages[@]} > 0 )); then
  sudo apt-get update
  sudo apt-get install -y "${host_packages[@]}"
fi
adb version
if [[ "$ptrace_scope" != "0" ]]; then
  if ! command -v gdb >/dev/null 2>&1; then
    echo "error: ptrace_scope=$ptrace_scope requires gdb for stable AArch64 emulator startup" >&2
    exit 14
  fi
  gdb --version | head -1
fi

uuid_shim_source="$repo_root/scripts/ci/arm64_emulator_uuid_shim.c"
uuid_shim="$android_home/emulator/lib64/libStubXlib.so"
if [[ ! -f "$uuid_shim_source" ]]; then
  echo "error: ARM64 emulator UUID shim source is missing: $uuid_shim_source" >&2
  exit 16
fi
uuid_shim_tmp="$(mktemp -t dartplant-arm64-uuid-shim-XXXXXX.so)"
"$(command -v cc)" \
  -shared -fPIC -O2 -std=c11 -Wall -Wextra -Werror \
  -Wl,-z,relro,-z,now \
  "$uuid_shim_source" \
  -o "$uuid_shim_tmp"
if ! is_aarch64_elf "$uuid_shim_tmp"; then
  echo "error: host-built UUID shim is not ELF64 AArch64" >&2
  exit 16
fi
if ! readelf -sW "$uuid_shim_tmp" | grep -Eq '[[:space:]]uuid_generate$'; then
  echo "error: host-built UUID shim does not export uuid_generate" >&2
  exit 16
fi
mv -f "$uuid_shim_tmp" "$uuid_shim"
uuid_shim_sha="$(sha256sum "$uuid_shim" | awk '{print $1}')"
{
  echo "uuid_interposition=libStubXlib.so"
  echo "uuid_shim_sha256=$uuid_shim_sha"
  echo "uuid_shim_elf=AArch64"
} | tee -a "$log_dir/emulator-provenance.txt"

# The emulator's SDK-root resolver expects platform-tools to live directly
# under ANDROID_SDK_ROOT. Google does not publish Linux-AArch64 platform-tools
# through the standard SDK repository, so expose Ubuntu's native AArch64
# platform-tools at the canonical SDK location instead of relying on PATH only.
adb_bin="$(readlink -f "$(command -v adb)")"
native_platform_tools="$(dirname "$adb_bin")"
if [[ ! -x "$native_platform_tools/adb" ]]; then
  echo "error: native ARM64 platform-tools directory is invalid: $native_platform_tools" >&2
  exit 12
fi
rm -rf "$android_home/platform-tools"
ln -s "$native_platform_tools" "$android_home/platform-tools"
echo "platform_tools=$android_home/platform-tools -> $native_platform_tools"

echo "emulator_bin=$emulator_bin"
file "$emulator_bin" || true
file "$qemu_aarch64_bin" || true
ldd "$emulator_bin" || true
if ! "$emulator_bin" -version; then
  echo "error: installed AArch64 emulator binary cannot execute on the ARM64 host" >&2
  exit 5
fi

# Mirror the AOSP tools/base Bazel emulator launcher instead of asking
# avdmanager to resolve a hardware profile. The SDK image package does not
# need to contain devices.xml for a valid AVD: write the two AVD ini files
# directly, use the system image's absolute path, and map arm64-v8a to the
# emulator architecture name "arm64" exactly as the emulator sources do.
system_image_dir="$android_home/system-images/android-${api_level}/${system_image_tag}/arm64-v8a"
if [[ ! -f "$system_image_dir/source.properties" ]]; then
  echo "error: ARM64 system image is incomplete: $system_image_dir" >&2
  exit 6
fi
image_api="$(sed -n 's/^AndroidVersion.ApiLevel=//p' "$system_image_dir/source.properties" | head -1)"
image_abi="$(sed -n 's/^SystemImage.Abi=//p' "$system_image_dir/source.properties" | head -1)"
if [[ "$image_api" != "$api_level" || "$image_abi" != "arm64-v8a" ]]; then
  echo "error: system-image provenance mismatch: api=$image_api abi=$image_abi" >&2
  exit 13
fi

android_emulator_home="${ANDROID_EMULATOR_HOME:-$HOME/.android}"
android_avd_home="${ANDROID_AVD_HOME:-$android_emulator_home/avd}"
export ANDROID_EMULATOR_HOME="$android_emulator_home"
export ANDROID_AVD_HOME="$android_avd_home"
mkdir -p "$android_avd_home"

avd_dir="$android_avd_home/${avd_name}.avd"
rm -rf "$avd_dir"
rm -f "$android_avd_home/${avd_name}.ini"
mkdir -p "$avd_dir"

cat >"$android_avd_home/${avd_name}.ini" <<EOF
avd.ini.encoding=UTF-8
path=$avd_dir
path.rel=avd/${avd_name}.avd
target=android-${api_level}
EOF

cat >"$avd_dir/config.ini" <<EOF
AvdId=$avd_name
PlayStore.enabled=false
abi.type=arm64-v8a
avd.ini.displayname=$avd_name
avd.ini.encoding=UTF-8
disk.dataPartition.size=2G
hw.accelerometer=yes
hw.arc=false
hw.audioInput=no
hw.battery=yes
hw.camera.back=none
hw.camera.front=none
hw.cpu.arch=arm64
hw.cpu.ncore=2
hw.dPad=no
hw.device.manufacturer=Google
hw.device.name=pixel
hw.gps=yes
hw.gpu.enabled=yes
hw.gpu.mode=swiftshader_indirect
hw.initialOrientation=Portrait
hw.keyboard=yes
hw.lcd.density=420
hw.lcd.height=1920
hw.lcd.width=1080
hw.mainKeys=no
hw.ramSize=2048
hw.sdCard=no
hw.sensors.orientation=yes
hw.sensors.proximity=yes
hw.trackBall=no
image.sysdir.1=${system_image_dir}/
runtime.network.latency=none
runtime.network.speed=full
showDeviceFrame=no
tag.id=$system_image_tag
vm.heapSize=256
EOF

{
  echo "avd_name=$avd_name"
  echo "avd_home=$android_avd_home"
  echo "system_image_dir=$system_image_dir"
  echo "system_image_api=$image_api"
  echo "abi.type=$image_abi"
  echo "hw.cpu.arch=arm64"
} | tee "$log_dir/avd-provenance.txt"

accel="off"
if [[ -c /dev/kvm && -r /dev/kvm && -w /dev/kvm ]]; then
  accel="on"
fi
echo "emulator_accel=$accel"

emulator_args=(
  -avd "$avd_name"
  -no-window
  -no-audio
  -no-boot-anim
  -no-snapshot
  -no-metrics
  -wipe-data
  -cores 2
  -memory 2048
  -gpu swiftshader_indirect
  -feature -Vulkan
  -accel "$accel"
)

qemu_cpu_mode="host"
qemu_gic_mode="host"
if [[ "$accel" == "off" ]]; then
  # Android Emulator's Linux-AArch64 glue selects -cpu host for an ARM64
  # target even when KVM is unavailable. QEMU also treats gic-version=host
  # as a KVM-only setting, so pure TCG must override both through the
  # emulator's documented -qemu passthrough. This mirrors QEMU's own
  # software-emulation convention: -cpu max with an emulated/max GIC.
  qemu_cpu_mode="max"
  qemu_gic_mode="max"
  emulator_args+=(
    -qemu
    -cpu max
    -machine gic-version=max
  )
fi

emulator_launch_mode="direct"
launch_prefix=()
if [[ "$ptrace_scope" != "0" ]]; then
  # Proven workaround for the Linux-AArch64 emulator crash-handler/YAMA
  # interaction. "handle all" is intentional: QEMU legitimately uses signals
  # such as SIGUSR1/SIGUSR2/SIGCONT, so handling only SIGSEGV can stop and kill
  # an otherwise healthy guest under gdb batch mode.
  gdb_commands="$log_dir/gdb-launch.txt"
  cat >"$gdb_commands" <<'GDBEOF'
set pagination off
set confirm off
handle all nostop noprint pass
run
GDBEOF
  emulator_launch_mode="gdb-yama-workaround"
  ptrace_workaround="gdb"
  launch_prefix=(gdb -q -batch -x "$gdb_commands" --args)
fi

{
  echo "qemu_cpu_mode=$qemu_cpu_mode"
  echo "qemu_gic_mode=$qemu_gic_mode"
  echo "ptrace_scope_original=$ptrace_scope_original"
  echo "ptrace_scope=$ptrace_scope"
  echo "ptrace_workaround=$ptrace_workaround"
  echo "emulator_launch_mode=$emulator_launch_mode"
} | tee -a "$log_dir/avd-provenance.txt"

"${launch_prefix[@]}" "$emulator_bin" "${emulator_args[@]}" >"$log_dir/emulator.log" 2>&1 &
emulator_pid=$!
echo "$emulator_pid" >"$log_dir/emulator.pid"
echo "emulator_pid=$emulator_pid launch_mode=$emulator_launch_mode"

emulator_has_fatal_error() {
  [[ -f "$log_dir/emulator.log" ]] && \
    grep -Eq \
      'PANIC:|gic-version=host requires KVM|CPU type .*KVM|only be used with KVM' \
      "$log_dir/emulator.log"
}

deadline=$((SECONDS + boot_timeout))
serial=""
while (( SECONDS < deadline )); do
  if ! kill -0 "$emulator_pid" 2>/dev/null; then
    echo "error: emulator exited before adb became ready" >&2
    tail -200 "$log_dir/emulator.log" || true
    exit 7
  fi
  if emulator_has_fatal_error; then
    echo "error: emulator/QEMU reported a fatal boot error before adb became ready" >&2
    tail -200 "$log_dir/emulator.log" || true
    exit 7
  fi
  serial="$(adb devices | awk '$2 == "device" && $1 ~ /^emulator-/ {print $1; exit}')"
  if [[ -n "$serial" ]]; then
    boot_completed="$(adb -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
    if [[ "$boot_completed" == "1" ]]; then
      break
    fi
  fi
  sleep 2
done

if [[ -z "$serial" ]]; then
  echo "error: emulator never appeared in adb" >&2
  tail -200 "$log_dir/emulator.log" || true
  exit 8
fi
boot_completed="$(adb -s "$serial" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')"
if [[ "$boot_completed" != "1" ]]; then
  echo "error: ARM64 Android guest did not finish booting within ${boot_timeout}s" >&2
  tail -200 "$log_dir/emulator.log" || true
  exit 9
fi

system_server_pid="$(adb -s "$serial" shell pidof system_server 2>/dev/null | tr -d '\r')"
if [[ -z "$system_server_pid" ]]; then
  echo "error: system_server is unavailable after sys.boot_completed=1" >&2
  exit 9
fi

echo "waiting for Android broadcast queues to become idle"
broadcast_idle_output="$(
  timeout 900s adb -s "$serial" shell am wait-for-broadcast-idle 2>&1
)" || {
  status=$?
  echo "error: am wait-for-broadcast-idle failed with exit code $status" >&2
  printf '%s\n' "$broadcast_idle_output" >&2
  exit 9
}
printf '%s\n' "$broadcast_idle_output"
if [[ "$broadcast_idle_output" != *"All broadcast queues are idle!"* ]]; then
  echo "error: Android broadcast queues did not report an idle state" >&2
  exit 9
fi

package_manager_ready() {
  local output
  output="$(timeout 60s adb -s "$serial" shell pm path android 2>/dev/null | tr -d '\r')" || return 1
  [[ "$output" == package:* ]]
}

if ! package_manager_ready; then
  echo "error: PackageManager is not responsive after broadcast-idle barrier" >&2
  exit 9
fi

# Pure TCG can expose a short interval where BOOT_COMPLETED is done but
# system_server is still destabilized by late startup work. Require the same
# system_server process and a responsive PackageManager across a short soak so
# the runtime gate never starts installing APKs into a dying framework.
readiness_soak_seconds="${DARTPLANT_ANDROID_READINESS_SOAK_SECONDS:-60}"
if ! [[ "$readiness_soak_seconds" =~ ^[0-9]+$ ]]; then
  echo "error: DARTPLANT_ANDROID_READINESS_SOAK_SECONDS must be an integer" >&2
  exit 9
fi
echo "soaking Android framework readiness for ${readiness_soak_seconds}s"
readiness_deadline=$((SECONDS + readiness_soak_seconds))
readiness_checks=0
system_server_pid_after="$system_server_pid"
while (( SECONDS < readiness_deadline )); do
  system_server_pid_after="$(
    timeout 60s adb -s "$serial" shell pidof system_server 2>/dev/null | tr -d '\r'
  )" || system_server_pid_after=""
  if [[ -z "$system_server_pid_after" || "$system_server_pid_after" != "$system_server_pid" ]]; then
    echo "error: system_server restarted during readiness soak: before=$system_server_pid after=$system_server_pid_after" >&2
    exit 9
  fi
  if ! package_manager_ready; then
    echo "error: PackageManager became unresponsive during readiness soak" >&2
    exit 9
  fi
  readiness_checks=$((readiness_checks + 1))
  sleep 5
done

guest_uname="$(adb -s "$serial" shell uname -m | tr -d '\r')"
guest_abi="$(adb -s "$serial" shell getprop ro.product.cpu.abi | tr -d '\r')"
guest_abilist="$(adb -s "$serial" shell getprop ro.product.cpu.abilist | tr -d '\r')"
printf '%s\n' "$serial" >"$log_dir/serial"
{
  echo "serial=$serial"
  echo "guest_uname=$guest_uname"
  echo "guest_abi=$guest_abi"
  echo "guest_abilist=$guest_abilist"
  echo "boot_completed=$boot_completed"
  echo "broadcast_idle=1"
  echo "system_server_pid=$system_server_pid_after"
  echo "package_manager_ready=1"
  echo "readiness_soak_seconds=$readiness_soak_seconds"
  echo "readiness_checks=$readiness_checks"
  echo "emulator_accel=$accel"
  echo "qemu_cpu_mode=$qemu_cpu_mode"
  echo "qemu_gic_mode=$qemu_gic_mode"
  echo "ptrace_scope_original=$ptrace_scope_original"
  echo "ptrace_scope=$ptrace_scope"
  echo "ptrace_workaround=$ptrace_workaround"
  echo "emulator_launch_mode=$emulator_launch_mode"
} | tee "$log_dir/guest.txt"

if [[ "$guest_uname" != "aarch64" || "$guest_abi" != "arm64-v8a" ]]; then
  echo "error: guest provenance is not native ARM64 Android" >&2
  exit 10
fi

echo "ARM64 Android emulator boot probe: PASS"
