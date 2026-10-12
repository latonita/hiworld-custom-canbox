#!/usr/bin/env bash
MCP=http://127.0.0.1:8089
ROOT="<repo_folder>"
subsys(){ # $1=name -> subsystem file basename
  local n="$1"
  case "$n" in
    can_dispatch_profile*|can_profile_dispatch*) echo can_rx_decode;;
    can_*|CAN_*) echo can;;
    uart_*|usart_*|USART_*) echo uart;;
    FLASH_*|flash_*) echo flash;;
    gpio_*|afio_*) echo gpio;;
    tim_*|tim2_*|TIM_*) echo tim;;
    rcc_*|RCC_*|pwr_*) echo rcc;;
    adc_*) echo adc;;
    dma_*) echo dma;;
    exti_*|nvic_*) echo core;;
    wwdg_*) echo wwdg;;
    softtimer_*|util_*|memcpy*|i2c_*) echo util;;
    sys_*|main_loop|startup_crt0|call_main|image_entry|app_entry*|param_page*|reinit_*|fatal_*) echo sys;;
    Delay*|Initialize*|Set*|Get*|Receive*|Send*|Enable*|Program*|Erase*|Wait*|Handle*|Reset*) echo boot_core;;
    *) echo misc;;
  esac
}
declare -A SEEN
while IFS=$'\t' read -r addr name; do
  [ -z "$addr" ] && continue
  case "$addr" in 0800[01]*) region=bootloader;; *) region=app;; esac
  base=$(subsys "$name")
  f="$ROOT/$region/src/${base}.c"
  if [ -z "${SEEN[$f]}" ]; then
    echo "// ${region} subsystem: ${base}  —  decompiled from Ghidra (named)." > "$f"
    echo "// Nations N32L406 canbox. Addresses are original flash addresses." >> "$f"
    echo "#include \"n32l406.h\"" >> "$f"; echo >> "$f"
    SEEN[$f]=1
  fi
  echo "// ================= $name @ 0x$addr =================" >> "$f"
  curl -s "$MCP/decompile_function?address=0x$addr" 2>/dev/null >> "$f"
  echo >> "$f"
done < "$ROOT/ghidra/functions.tsv"
echo "export done."
echo "bootloader files:"; ls "$ROOT/bootloader/src/"
echo "app files:"; ls "$ROOT/app/src/"
wc -l "$ROOT"/app/src/*.c "$ROOT"/bootloader/src/*.c | tail -1
