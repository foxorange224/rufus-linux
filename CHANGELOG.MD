# ChangeLogs Rufus for Linux

o Version 1.0.3 (2026.08.16):
    - About dialog redesigned to match the original Rufus layout:
      * Icon and text block (title, version, official website link,
        copyright, Linux port credits, translators, bug reporting)
      * "Additional Copyrights" scroll box with third-party notices
        (GPLv3, Qt6 LGPLv3, zstd/xxhash, FreeDOS, SYSLINUX, GRUB2,
        UEFI:NTFS, MBR)
      * License button opens the full GPLv3 text from the binary
      * New About strings translated in all 14 shipped languages
    - Welcome dialog: the language selector now applies the chosen
      language immediately, retranslating the dialog on the fly
    - GUI log always scrolls to the bottom (no manual scrolling needed
      during long operations)
    - All log and status lines are now shown in English (no localized
      technical messages in the log)
    - es-ES translation reviewed: 63 corrections (shorter, clearer
      messages) propagated to all other shipped languages; all
      translations remain 100% complete

o Version 1.0.2 (2026.08.15):
    - Boot and format engine reworked:
      * Syslinux redirect stage eliminated (direct MBR/SBR installation)
      * More reliable format sequence and partition table handling
      * MS-DOS / FreeDOS / ReactOS boot handling fixes
      * Progress bar phases reworked for ISO writes (format steps
        compressed when an image extraction follows)
    - GUI improvements:
      * New Preferences dialog (About button opens it) with interface
        theme selection (Fusion default)
      * Language menu now reflects the active language correctly
      * System language detected at startup (session / locale.conf)
      * "?" help next to Target system hides when not applicable
      * START button shows a guidance message when no image is selected
      * About dialog links open in the user's session browser (fixed
        launching as root / Kate fallback)
      * Extraction log lines appear when each file starts, matching the
        status bar ("Extracting: /path (size)")
      * GUI log no longer shows timestamps or internal debug output
      * Device and target system labels retranslated on language change
    - New headless CLI mode:
      * sudo ./rufus --image=<file> --boot=mbr|gpt --filesystem=<fs>
        --device=sdX
      * --list-devices / --devices: list removable drives
      * IMG/VHD disk images ignore --boot/--filesystem with a notice
      * Translated messages (es-ES) and log-style output to stdout
    - Elevation:
      * Running the GUI without root shows a confirmation and opens the
        session's polkit password dialog (pkexec) instead of failing
      * CLI mode still requires sudo; --list-devices works without root
      * Internal --elevated flag prevents re-elevation loops
    - Diagnostics:
      * Logger filters debug messages to --debug mode
      * XDG_RUNTIME_DIR handled when started via sudo/pkexec
      * Theme diagnostic line in the log (style, palette, config dirs)
      * Translations updated (es-ES, pt-BR, ru-RU)
      * Language menu lists every shipped language dynamically (no
        hardcoded filter); all shipped translations are 100% complete

o Version 1.0.1 (2026.08.11):
    Alpha release