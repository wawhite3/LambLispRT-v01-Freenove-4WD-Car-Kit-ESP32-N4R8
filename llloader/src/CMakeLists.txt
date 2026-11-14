idf_component_register(
    SRCS "llloader_main.c"
         "ota_writer.c"
         "source_ladder.c"
         "src_aux_storage.c"
         "src_network.c"
         "src_console.c"
         "src_ble_console.c"
         "llloader_hex.c"
         # [owner 2026-10-06] The status LED.  Compiles to empty functions unless LOADER_LED_GPIO is
         # defined, so it is in the build for every env while changing none of them -- same reasoning
         # as the LLIP reader below: whoever first sets the flag should not also be discovering a
         # cross-compile break in the trusted root.
         "llloader_led.c"
         # [P253] The LLIP frame reader/writer.  IN THE BUILD BEFORE IT HAS A CALLER, ON PURPOSE --
         # so that whoever wires up the first caller is not also discovering a cross-compile break
         # in the trusted root while changing its boot path.
         #
         # THE COST WAS MEASURED, NOT ASSUMED, because the factory partition is at 87.9% with only
         # ~139 KB spare and a free-looking addition there is worth checking.  Two builds of
         # llloader-esp32 on 2026-09-30, the CMakeLists line the only difference:
         #   without:  Flash 1037003 / 1179648 (87.9%)   RAM 46392
         #   with:     Flash 1037003 / 1179648 (87.9%)   RAM 46392
         # Byte-identical -- --gc-sections drops an unreferenced translation unit entirely.  If a
         # later addition here is NOT free, that is the number to compare against.
         #
         # AND IT COMPILES ON BOTH ARCHITECTURES, checked by looking at the OBJECT rather than at
         # the exit code (rc=0 does not prove a file was built -- a mis-spelled SRCS entry is not
         # an error to CMake in every layout):
         #   .../llloader-esp32/src/llloader_llip.c.o        ELF 32-bit, Tensilica Xtensa
         #   .../llloader-esp32c5-8m/src/llloader_llip.c.o   ELF 32-bit, UCB RISC-V, RVC, soft-float
         # Zero warnings in either.  That is the reason this line exists before a caller does.
         "llloader_llip.c"
         # [P253] step C: the query answerer's POLICY -- which query is answered, with which fields.
         # Pure (no IDF, no NVS, no UART), so it carries 27 host arms in `w3 test loader`; the IDF
         # side is gather_status() in llloader_main.c, which is the only part needing a board.
         #
         # UNLIKE llloader_llip.c ABOVE, THIS ONE HAS A CALLER, so --gc-sections cannot drop it and
         # the cost is REAL rather than zero.  Measured 2026-10-03, llloader-esp32s3, the step C
         # wiring the only difference:
         #   before:  Flash 1068191 / 1179648 (90.6%)   RAM 40128
         #   after:   Flash 1071259 / 1179648 (90.8%)   RAM 40128
         # +3,068 bytes of flash, no RAM change, ~106 KB spare in the factory slot.  That is the
         # whole of step C -- this file, the scan-loop answerer in src_console.c, and gather_status()
         # in llloader_main.c -- not this file alone.
         #
         # The figure went DOWN 88 bytes when src_console.c was rewired onto this file's pure RX
         # scanner ([B741]): the hand-rolled magic matcher and query buffer it replaced were
         # duplicated logic.  A bug fix and a testability refactor that together cost less than the
         # wiring they replaced is worth recording, because the usual assumption is the reverse.
         #
         # *** DO NOT COMPARE AGAINST THE 1037003 FIGURE RECORDED ABOVE.  IT IS A DIFFERENT
         # ARCHITECTURE. *** Those two builds were llloader-esp32 (classic, xtensa LX6); these are
         # llloader-esp32s3 (LX7).  Both tables happen to give factory the same 1179648 bytes, so
         # the numbers look directly comparable and are not: differencing them yields +34,344 bytes
         # and attributes a whole architecture to this one file.  I nearly recorded that.  When
         # adding to this list, build the SAME env before and after and say which env it was.
         #
         # The link failure on the way here is worth recording rather than skipping: src_console.c
         # COMPILED CLEAN with the call in it and failed only at LINK --
         # `undefined reference to llloader_query_reply` -- which is the exact shape CLAUDE.md's
         # nugget names.  A missing SRCS entry is invisible to the compiler; only the linker asks.
         "llloader_query.c"
         # [P253] step B: the press-and-hold exit's DECISION -- arming, the threshold, what a release
         # means -- lifted out of the FreeRTOS task in llloader_main.c so it can be refuted on a
         # laptop.  15 host arms in `w3 test loader`, including the GPIO0 STRAP case (a pin held low
         # since boot must never read as a hold) and both sides of the threshold.
         #
         # IT HAS A CALLER, so --gc-sections cannot drop it and the cost is real.  Measured on
         # llloader-esp32s3-wifi, this refactor the only difference:
         #   before:  Flash 877771 / 1179648 (74.4%)
         #   after:   Flash 877855 / 1179648 (74.4%)  -- +84 bytes, no RAM change
         # The task now holds no timing arithmetic of its own: ONE copy of the decision, or the host
         # arms would be testing a parallel implementation while the loader did something else.
         #
         # *** THE LINE BELOW IS THE ENTRY.  I WROTE THIS COMMENT BLOCK AND LEFT THE ENTRY OUT, and
         # the build died at LINK with `undefined reference to llloader_btn_init` after 8 minutes --
         # the exact failure the llloader_query.c note above already records in as many words ("a
         # missing SRCS entry is invisible to the compiler; only the linker asks").  Reading a warning
         # is not the same as it firing.  If you add a file here, add the quoted name FIRST and the
         # explanation after. ***
         "llloader_button.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_partition nvs_flash mbedtls esp_system esp_driver_rmt
    PRIV_REQUIRES esp_timer joltwallet__littlefs
                  esp_wifi esp_netif esp_event esp_http_client esp_driver_uart
                  bt)
