# Ghidra script — rename all identified functions in the Hiworld JPF2 box firmware.
# Run from the Script Manager (GUI) or:  analyzeHeadless <proj> <name> -process <bin> -postScript rename_hiworld.py
#
# Works whether the program's image base is 0x08002000 (correct) or 0x00000000
# (default binary import): every address below is written in FLASH terms and mapped to
#   gui_addr = image_base + (flash_addr - 0x08002000)
# Functions that don't exist at the mapped address (e.g. in an under-analyzed base-0 db)
# are reported and skipped — re-run auto-analysis (and a Thumb prologue scan) first to
# define all 383 functions, then run this again.
#
# @category Hiworld

FLASH_BASE = 0x08002000

# flash_addr -> name.  Provenance: hiworld-iap-upload.md / _index.md (FLASH/UART/startup),
# hiworld-all-car-profiles.md (CAN profiles), hiworld-gpio-io.md (GPIO/TIM/EXTI),
# hiworld-jpf2-83k-io.md (uart_D2).  DEAD = present but zero callers.
NAMES = {
    # --- startup / runtime ---
    0x08002000: "image_entry",
    0x0800200c: "startup_crt0",
    0x0800220a: "call_main",
    0x080088d8: "main_loop",
    # --- parameter page (flash 0x0801F800) ---
    0x08002d3e: "wwdg_kick",
    0x08002d46: "param_page_save_0801F800",   # ONLY live flash writer (param page only)
    0x08002dbc: "param_page_load_0801F800",
    # --- reset / firmware-update trigger ---
    0x08002dec: "reinit_can_filters_and_state",
    0x08002e26: "wwdg_force_reset_thunk",
    0x08002e2e: "wwdg_force_reset",           # 0xE0 handler target: while(1) WWDG_CR=0xBF
    0x08002e64: "fatal_error_reset",
    # --- FLASH HAL (0x40022000) ---
    0x080046b4: "RCC_EnableHSI_forFlash",
    0x080046fc: "FLASH_Unlock",
    0x08004708: "FLASH_Lock",
    0x08004716: "FLASH_GetBank1Status",
    0x08004762: "FLASH_WaitForLastOperation",
    0x08004788: "FLASH_ClearStatusFlags",
    0x08004794: "FLASH_ErasePage",            # LIVE - param page only
    0x080047e2: "FLASH_MassErase_DEAD",
    0x0800482c: "FLASH_GetReadOutProtStatus",
    0x08004840: "FLASH_OB_precheck",
    0x08004854: "FLASH_OB_RDPprogram_DEAD",
    0x08004a96: "FLASH_ProgramWord",          # LIVE - param page only
    0x08004aec: "FLASH_ProgramOBData_DEAD",
    0x08004b4a: "FLASH_OB_WriteProt_DEAD",
    0x08004bf6: "FLASH_OB_RDPtoggle_DEAD",
    0x08004cbc: "FLASH_OB_routineA_DEAD",
    0x08004d94: "FLASH_OB_routineB_DEAD",
    0x08004e84: "FLASH_OB_routineC_DEAD",
    # --- UART (USART2, 38400 8N1) ---
    0x0800291e: "usart2_init",
    0x08008a54: "uart_init_38400_and_version",
    0x08008a96: "uart_rx_dispatch",           # 0xFC->0xFD handshake; 0xE0->wwdg_force_reset
    0x08008bb2: "uart_rx_parser_AA55",
    0x08008d04: "uart_build_frame_AA55",
    0x08008d9c: "uart_tx_round_robin",
    0x08008e62: "uart_tx_sched",
    0x08007da8: "uart_D2_cluster_text_to_can",
    # --- CAN (bxCAN 0x40006400) ---
    0x080074c0: "can_tx_enqueue",
    0x0800755c: "can_autobaud_state_machine",
    0x08008110: "can_periodic_tx_scheduler",
    0x08007d44: "can_profile_dispatch_gate",  # profile_sel: ==1 -> profile1 else profile0
    0x08007b2c: "can_set_baud_by_profile",    # 0 -> 0x53=83.333k, 1 -> 0x7D=125k
    0x08009010: "can_dispatch_profile0_83k",  # this car (Grand Cherokee WK)
    0x08009680: "can_dispatch_profile1_125k", # 125k FCA variant (0x211 RPM, 0x20E gear, ...)
    # --- GPIO / peripherals (Nations N32 GPIO map: set +0x18, clear +0x28) ---
    0x080084f6: "gpio_write_status_lines",    # PB6=reverse PC15=illum PB5,PB4 from CAN state
    0x08005f48: "rcc_apb2_enable",
    0x080053a2: "gpio_set_bit",
    0x080053aa: "gpio_clear_bit",
    0x08007430: "gpio_can_xcvr_standby_pb12",
    0x08008458: "gpio_static_init_a",
    0x080084be: "gpio_static_init_b",
    0x080027f8: "tim2_init",
    0x08002f72: "tim2_isr_scheduler_tick",
    0x08003222: "update_path_exti_setup",
    0x080032f6: "exti8_bus_wake_setup",
    # --- util ---
    0x08008598: "memcpy_bytes",
}

fm = currentProgram.getFunctionManager()
base = currentProgram.getImageBase().getOffset()
delta = base - FLASH_BASE
sp = currentProgram.getAddressFactory().getDefaultAddressSpace()
from ghidra.program.model.symbol import SourceType

renamed = 0
missing = []
for flash_addr, name in sorted(NAMES.items()):
    a = sp.getAddress(flash_addr + delta)
    fn = fm.getFunctionAt(a) or fm.getFunctionContaining(a)
    if fn is None:
        missing.append((flash_addr, name))
        continue
    try:
        fn.setName(name, SourceType.USER_DEFINED)
        renamed += 1
    except Exception as e:
        missing.append((flash_addr, "%s (ERR %s)" % (name, e)))

print("=== rename_hiworld: image_base=0x%08x delta=0x%x ===" % (base, delta & 0xffffffff))
print("renamed %d / %d" % (renamed, len(NAMES)))
if missing:
    print("NOT FOUND (define these functions first, then re-run):")
    for fa, nm in missing:
        print("  flash 0x%08x -> gui 0x%08x : %s" % (fa, fa + delta, nm))
