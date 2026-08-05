# Repository Guidelines

## Project Structure & Module Organization

This tree is a Rockchip RK3506 Linux SDK composed of multiple Git projects. Major components include `kernel-6.1/` (active Linux kernel), `u-boot/`, `buildroot/`, `rkbin/`, `hal/`, `rtos/`, and `tools/`. Board configuration lives under `device/`; PowerFin device-tree work is primarily in `kernel-6.1/arch/arm/boot/dts/`. Generated images and staging output belong in `output/`, `rockdev/`, or component-specific build directories. Do not commit generated images, captures, or unpacked SDK archives unless they are intentional test fixtures.

## Build, Test, and Development Commands

- `./build.sh powerfin_buildroot_sdmmc_defconfig` selects the PowerFin SD-card configuration.
- `./build.sh kernel` builds the configured kernel and device trees.
- `./build.sh uboot` builds U-Boot; `./build.sh buildroot` builds the root filesystem.
- `./build.sh firmware` packages the configured firmware set.
- `./repack_powerfin_zboot.sh` repacks `zboot.img` without rebuilding the kernel.(use it when only edited dts)
- `./flash_zboot.sh -f` flashes the target device. This is destructive and requires explicit hardware-test intent.
- `./build.sh kernel-make:<target>` runs a kernel make target through the SDK environment.

Use the narrowest relevant build first. For kernel-only changes, avoid rebuilding the full SDK.

## Coding Style & Naming Conventions

Follow the conventions of the component being edited. Kernel code uses Linux style: tabs for indentation, `snake_case` identifiers, lowercase file names, and kernel types such as `u32`. Run `kernel-6.1/scripts/checkpatch.pl --strict <patch>` for kernel patches. Device-tree node and property names should follow existing Rockchip bindings and use lowercase hyphenated names. Shell scripts should use clear error handling and quote expansions.

## Testing Guidelines

There is no single SDK-wide test suite. Build the affected component and run its local tests where available, such as U-Boot tests under `u-boot/test/`. For kernel or DTS changes, verify boot logs, device nodes, sysfs state, and the relevant hardware path. Record the board, image, commands, and observed result in the pull request.

## Pin Setting
refer to: `pin_config.md`

## Dshot Debug
we implement a new kernel driver using rk3506 flexbus to produce dshot signal. If you are doing related debug and development, refer to:`dshot_debug.md`

## Reference
if you want some other opensource project to refer:
PX4: `~/PX4-Autopilot/`
BF: `~/betaflight/`

## Commit & Pull Request Guidelines

Keep commits scoped to one component and use short imperative subjects, commonly prefixed by the subsystem, for example `dts: rk3506-powerfin: add flexbus dshot`. Do not mix generated artifacts with source changes. Pull requests should describe the problem, implementation, configuration impact, build commands, and hardware verification. Link related issues and include logs or waveform captures when behavior depends on hardware timing.
