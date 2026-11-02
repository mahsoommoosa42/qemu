.. _Xtensa-System-emulator:

Xtensa System emulator
----------------------

Two executables cover simulation of both Xtensa endian options,
``qemu-system-xtensa`` and ``qemu-system-xtensaeb``. Two different
machine types are emulated:

-  Xtensa emulator pseudo board \"sim\"

-  Avnet LX60/LX110/LX200 board

The sim pseudo board emulation provides an environment similar to one
provided by the proprietary Tensilica ISS. It supports:

-  A range of Xtensa CPUs, default is the DC232B

-  Console and filesystem access via semihosting calls

The Avnet LX60/LX110/LX200 emulation supports:

-  A range of Xtensa CPUs, default is the DC232B

-  16550 UART

-  OpenCores 10/100 Mbps Ethernet MAC

ESP32-S3 deep sleep
~~~~~~~~~~~~~~~~~~

The ``esp32s3`` machine models deep-sleep entry when
``RTC_CNTL_DIG_PWC.DG_WRAP_PD_EN`` and ``RTC_CNTL_STATE0.SLEEP_EN`` are set.
Both CPUs stop executing. An enabled RTC main timer alarm wakes the chip
when selected in ``RTC_CNTL_WAKEUP_STATE``: the digital system resets with
reset cause 5, while the RTC counter, scratch registers and RTC memories
remain available. With no implemented wake source selected, the CPUs
remain asleep until an external machine reset.

The alarm uses QEMU virtual time and the selected RTC slow-clock frequency.
This models firmware-visible sleep entry and timer wakeup, not power
consumption or oscillator startup delays. Light sleep, GPIO/EXT0/EXT1,
touch, ULP and brownout wakeup, sleep rejection and RTC memory power-down
configuration are not implemented.
