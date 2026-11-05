"""Stage D: build the two images the complete update path needs.

  loader -> linked at 0x08000000, 64 KiB, may select a target and hand off
  app    -> linked at 0x08010000, 960 KiB, trusts storage and claims health

Same source list and flags as the explorer ZG UART target, so the board binding
is byte-for-byte the one already proven on silicon. Only the product switches,
the boot role and the linker script differ.

Kept outside the repo build for the same reason as build_update_variant.py: a
failed experiment must not look like a shipped target.

    python build-temp/build_stage_d.py
"""

import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GCC = Path(r"D:/Project/Can/toolchain/xpack-arm-none-eabi-gcc-15.2.1-1.1/bin/arm-none-eabi-gcc.exe")
DFP = Path(r"D:/Keil/Keil5/ARM/PACK/Keil/STM32F4xx_DFP/2.16.0")
TARGET = "explorer_ZG_uart_READONLY"
OUT = ROOT / "build-temp/stage-d"

# Switches this script owns; everything else comes from the proven target.
OWNED = ("OTA_ENABLE_FLASHING=", "OTA_ENABLE_BOOT_HANDOFF=", "OTA_ENABLE_WATCHDOG=", "OTA_EXPLORER_WATCHDOG_CAPABLE=", "OTA_EXPLORER_NO_I2C_CLEAR=",
         "OTA_EXPLORER_UPDATE_CAPABLE=", "OTA_EXPLORER_STORAGE_TRUSTED=",
         "OTA_EXPLORER_HANDOFF_CAPABLE=", "OTA_EXPLORER_BOOT_ROLE=",
         "OTA_EXPLORER_APP_SICK=")

VARIANTS = {
    "loader": {
        "main": "firmware/bootloader/main.c",
        "ld": "firmware/linker/explorer_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=1", "OTA_ENABLE_BOOT_HANDOFF=1",
                    "OTA_ENABLE_WATCHDOG=0", "OTA_EXPLORER_UPDATE_CAPABLE=1",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=1",
                    "OTA_EXPLORER_BOOT_ROLE=1"],
    },
    "app": {
        "main": "firmware/app/main.c",
        "ld": "firmware/linker/app_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=0", "OTA_ENABLE_BOOT_HANDOFF=0",
                    "OTA_ENABLE_WATCHDOG=0", "OTA_EXPLORER_UPDATE_CAPABLE=0",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=0",
                    "OTA_EXPLORER_BOOT_ROLE=2"],
    },
    # Test-only: the same application, except that it never confirms itself.
    # The trial counter runs out and the loader restores the previous image,
    # which is the rollback path the host model covers but silicon never ran.
    "sick": {
        "main": "firmware/app/main.c",
        "ld": "firmware/linker/app_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=0", "OTA_ENABLE_BOOT_HANDOFF=0",
                    "OTA_ENABLE_WATCHDOG=0", "OTA_EXPLORER_UPDATE_CAPABLE=0",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=0",
                    "OTA_EXPLORER_BOOT_ROLE=2", "OTA_EXPLORER_APP_SICK=1"],
    },
    # Temporary: dumps the EEPROM so the eeprom_crc32 banner field can be
    # judged against real bytes instead of against itself.
    "dump": {
        "main": "build-temp/stage-d/dump_main.c",
        "ld": "firmware/linker/explorer_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=0", "OTA_ENABLE_BOOT_HANDOFF=0",
                    "OTA_ENABLE_WATCHDOG=0", "OTA_EXPLORER_UPDATE_CAPABLE=0",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=0",
                    "OTA_EXPLORER_BOOT_ROLE=0"],
    },
    # Temporary: replaces the loader with the attacker. Rewrites the app and
    # repairs every checksum it can compute, so the next boot can only be
    # stopped by the signature.
    "attack": {
        "main": "build-temp/stage-d/attack_main.c",
        "ld": "firmware/linker/explorer_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=0", "OTA_ENABLE_BOOT_HANDOFF=0",
                    "OTA_ENABLE_WATCHDOG=0", "OTA_EXPLORER_UPDATE_CAPABLE=0",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=0",
                    "OTA_EXPLORER_BOOT_ROLE=0"],
    },
    # Temporary: invalidates both metadata records so the loader stays in
    # diagnostics, which is what a factory-fresh board looks like.
    "wipe": {
        "main": "build-temp/stage-d/wipe_meta_main.c",
        "ld": "firmware/linker/explorer_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=0", "OTA_ENABLE_BOOT_HANDOFF=0",
                    "OTA_ENABLE_WATCHDOG=0", "OTA_EXPLORER_UPDATE_CAPABLE=0",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=0",
                    "OTA_EXPLORER_BOOT_ROLE=0"],
    },
    # Same loader/app, with the hardware watchdog gate finally on. The BSP
    # binding already implements HAL IWDG; these two variants are what the
    # docs call "enable the gate only after measuring the real LSI".
    "loader_wd": {
        "main": "firmware/bootloader/main.c",
        "ld": "firmware/linker/explorer_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=1", "OTA_ENABLE_BOOT_HANDOFF=1",
                    "OTA_ENABLE_WATCHDOG=1", "OTA_EXPLORER_WATCHDOG_CAPABLE=1",
                    "OTA_EXPLORER_UPDATE_CAPABLE=1",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=1",
                    "OTA_EXPLORER_BOOT_ROLE=1"],
    },
    "app_wd": {
        "main": "firmware/app/main.c",
        "ld": "firmware/linker/app_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=0", "OTA_ENABLE_BOOT_HANDOFF=0",
                    "OTA_ENABLE_WATCHDOG=1", "OTA_EXPLORER_WATCHDOG_CAPABLE=1",
                    "OTA_EXPLORER_UPDATE_CAPABLE=0",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=0",
                    "OTA_EXPLORER_BOOT_ROLE=2"],
    },
    # The sick app plus the watchdog: it boots, answers, never confirms and
    # never feeds, so the IWDG -- not an external reset -- has to advance the
    # trial counter until the loader restores the previous image.
    "sick_wd": {
        "main": "firmware/app/main.c",
        "ld": "firmware/linker/app_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=0", "OTA_ENABLE_BOOT_HANDOFF=0",
                    "OTA_ENABLE_WATCHDOG=1", "OTA_EXPLORER_WATCHDOG_CAPABLE=1",
                    "OTA_EXPLORER_UPDATE_CAPABLE=0",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=0",
                    "OTA_EXPLORER_BOOT_ROLE=2", "OTA_EXPLORER_APP_SICK=1"],
    },
    # Temporary: start the watchdog and never reload it, to measure the real
    # LSI period on this chip instead of trusting the datasheet.
    "wd_bite": {
        "main": "build-temp/stage-d/wd_bite_main.c",
        "ld": "firmware/linker/explorer_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=0", "OTA_ENABLE_BOOT_HANDOFF=0",
                    "OTA_ENABLE_WATCHDOG=1", "OTA_EXPLORER_WATCHDOG_CAPABLE=1",
                    "OTA_EXPLORER_UPDATE_CAPABLE=0",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=0",
                    "OTA_EXPLORER_BOOT_ROLE=0"],
    },
    # Red probe for the EEPROM stress test: the product loader with the I2C
    # bus clear removed. A reset inside an EEPROM write must then reproduce
    # "EEPROM read failed" on the next boot -- otherwise the stress test is
    # not hitting the window it claims to hit. Never shipped.
    "loader_noclear": {
        "main": "firmware/bootloader/main.c",
        "ld": "firmware/linker/explorer_boot.ld",
        "defines": ["OTA_ENABLE_FLASHING=1", "OTA_ENABLE_BOOT_HANDOFF=1",
                    "OTA_ENABLE_WATCHDOG=0", "OTA_EXPLORER_UPDATE_CAPABLE=1",
                    "OTA_EXPLORER_STORAGE_TRUSTED=1", "OTA_EXPLORER_HANDOFF_CAPABLE=1",
                    "OTA_EXPLORER_BOOT_ROLE=1", "OTA_EXPLORER_NO_I2C_CLEAR=1"],
    },
}
DEFAULT = ("loader", "app")


def target_sources():
    project = ROOT / "mdk/explorer_readonly.uvprojx"
    target = next(t for t in ET.parse(project).findall("Targets/Target")
                  if t.findtext("TargetName") == TARGET)
    controls = target.find("TargetOption/TargetArmAds/Cads/VariousControls")
    defines = [d for d in controls.findtext("Define").split(",") if d != "__MICROLIB"]
    defines = [d for d in defines if not d.startswith(OWNED)]
    includes = [(project.parent / p.replace("\\", "/")).resolve()
                for p in controls.findtext("IncludePath").split(";")]
    files = [(project.parent / f.text.replace("\\", "/")).resolve()
             for f in target.findall("Groups/Group/Files/File/FilePath")]
    startup = DFP / "Drivers/CMSIS/Device/ST/STM32F4xx/Source/Templates/gcc/startup_stm32f407xx.s"
    sources = [p for p in files if p.suffix == ".c"]
    return defines, includes, sources, startup


def build(name, spec, defines, includes, sources, startup, log):
    work = OUT / name
    work.mkdir(parents=True, exist_ok=True)
    own = [p for p in sources if p.parent.name in ("bootloader", "app")]
    assert len(own) == 1, own
    body = [p for p in sources if p not in own] + [ROOT / spec["main"], startup]
    body.insert(-1, ROOT / "firmware/runtime/newlib_init.c")
    all_defines = defines + spec["defines"]

    common = [str(GCC), "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-Os", "-g3",
              "-ffunction-sections", "-fdata-sections", "-fno-common", "-std=c99"]
    common += ["-D" + d for d in all_defines]
    for path in includes[:4]:
        common += ["-I", str(path)]
    for path in includes[4:]:
        common += ["-isystem", str(path)]

    objects = []
    for index, source in enumerate(body):
        output = work / f"{index:02d}_{source.stem}.o"
        command = common + ["-Wall", "-Wextra"]
        if source.is_relative_to(ROOT):
            command += ["-Werror"]
        command += ["-c", str(source), "-o", str(output)]
        run = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=90)
        log.append("COMMAND: " + " ".join(command) + "\n" + run.stdout + run.stderr)
        if run.returncode:
            print(run.stdout + run.stderr)
            return None
        objects.append(output)

    elf, binary = work / f"{name}.elf", work / f"{name}.bin"
    link = [str(GCC), "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft", "-nostartfiles",
            "--specs=nano.specs", "--specs=nosys.specs",
            "-T", str(ROOT / spec["ld"]),
            "-Wl,--gc-sections", "-Wl,--fatal-warnings", *map(str, objects), "-o", str(elf)]
    run = subprocess.run(link, cwd=ROOT, capture_output=True, text=True, timeout=120)
    log.append("COMMAND: " + " ".join(link) + "\n" + run.stdout + run.stderr)
    if run.returncode:
        print(run.stdout + run.stderr)
        return None
    subprocess.run([str(GCC.with_name("arm-none-eabi-objcopy.exe")), "-O", "binary",
                    str(elf), str(binary)], check=True, timeout=60)
    return binary


def describe(name, binary):
    raw = binary.read_bytes()
    sp = int.from_bytes(raw[0:4], "little")
    reset = int.from_bytes(raw[4:8], "little")
    print(f"{name:7s} bytes={len(raw):6d} sp=0x{sp:08X} reset=0x{reset:08X} "
          f"sha256={__import__('hashlib').sha256(raw).hexdigest()[:16]}")
    return raw


def main():
    only = sys.argv[1:] or list(DEFAULT)
    unknown = [name for name in only if name not in VARIANTS]
    if unknown:
        print("unknown variant(s): %s" % unknown)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)
    defines, includes, sources, startup = target_sources()
    log = []
    for name in only:
        binary = build(name, VARIANTS[name], defines, includes, sources, startup, log)
        if binary is None:
            (OUT / "build-log.txt").write_text("\n".join(log), encoding="utf-8")
            return 1
        describe(name, binary)
    (OUT / "build-log.txt").write_text("\n".join(log), encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
