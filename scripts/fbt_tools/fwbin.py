import os

import SCons
from SCons.Action import Action
from SCons.Builder import Builder

__OBJCOPY_ARM_BIN = "arm-none-eabi-objcopy"
__NM_ARM_BIN = "arm-none-eabi-nm"


def _copro_load_addr(env):
    """Flash install address of the configured radio stack, or None if unavailable."""
    try:
        stack_bin = os.path.join(
            env.subst("${ROOT_DIR.abspath}"),
            env.subst("${COPRO_STACK_BIN_DIR}"),
            env.subst("${COPRO_STACK_BIN}"),
        )
        if not os.path.isfile(stack_bin):
            return None
        from flipper.assets.coprobin import CoproBinary

        return CoproBinary(stack_bin).get_flash_load_addr()
    except Exception:
        return None


def generate(env):
    env.SetDefault(
        BIN2DFU="${FBT_SCRIPT_DIR}/bin2dfu.py",
        BIN_SIZE_SCRIPT="${FBT_SCRIPT_DIR}/fwsize.py",
        OBJCOPY=__OBJCOPY_ARM_BIN,  # FIXME
        NM=__NM_ARM_BIN,  # FIXME
    )

    copro_base = _copro_load_addr(env)
    if copro_base is not None:
        env["ENV"]["FBT_COPRO_BASE"] = f"{copro_base:#010x}"

    for var in ("FBT_COPRO_BASE", "FBT_ALLOW_COPRO_OVERLAP"):
        if var in os.environ:
            env["ENV"][var] = os.environ[var]

    if not env["VERBOSE"]:
        env.SetDefault(
            HEXCOMSTR="\tHEX\t${TARGET}",
            BINCOMSTR="\tBIN\t${TARGET}",
            DFUCOMSTR="\tDFU\t${TARGET}",
        )

    env.Append(
        BUILDERS={
            "HEXBuilder": Builder(
                action=Action(
                    [["${OBJCOPY}", "-O", "ihex", "${SOURCE}", "${TARGET}"]],
                    "${HEXCOMSTR}",
                ),
                suffix=".hex",
                src_suffix=".elf",
            ),
            "BINBuilder": Builder(
                action=Action(
                    [["${OBJCOPY}", "-O", "binary", "-S", "${SOURCE}", "${TARGET}"]],
                    "${BINCOMSTR}",
                ),
                suffix=".bin",
                src_suffix=".elf",
            ),
            "DFUBuilder": Builder(
                action=Action(
                    [
                        [
                            "${PYTHON3}",
                            "${BIN2DFU}",
                            "-i",
                            "${SOURCE}",
                            "-o",
                            "${TARGET}",
                            "-a",
                            "${IMAGE_BASE_ADDRESS}",
                            "-l",
                            "Flipper Zero F${TARGET_HW}",
                        ]
                    ],
                    "${DFUCOMSTR}",
                ),
                suffix=".dfu",
                src_suffix=".bin",
            ),
        }
    )


def exists(env):
    try:
        return env["OBJCOPY"]
    except KeyError:
        pass

    if objcopy := env.WhereIs(__OBJCOPY_ARM_BIN):
        return objcopy

    raise SCons.Errors.StopError("Could not detect objcopy for arm")
