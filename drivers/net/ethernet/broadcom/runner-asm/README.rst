.. SPDX-License-Identifier: GPL-2.0-only

BCM6846 Runner assembler
=======================

``runner-as`` builds the reduced BCM6846 SRAM Buffer Pool Manager (SBPM)
program used by the BCMBCA XRDP Ethernet driver. It uses a flex scanner, bison grammar, symbol table
and instruction encoder, following the separation used by aic7xxx's
``aicasm``. It is an independent implementation, without the AIC instruction
set, conditional patching language or Berkeley DB dependency.

This is an encoding assembler for the supplied single-issue BCM6846 image,
not a complete description of the Runner ISA or an ACE-compatible assembler.
The instruction forms below distinguish decoded operands from fields whose
meaning remains unresolved. The notification extension below reuses an observed external write.
Packet notification delivery has been tested on BCM6846; the instruction
is not a general-purpose DMA interface. Do not use the unresolved
forms as documented peripheral operations, or assume compatibility with
the dual-issue Runner in newer SoCs.

Build and test
--------------

Kbuild builds the host assembler when the Ethernet driver is enabled,
assembles ``bcm6846_runner.rnr`` and embeds the result through ``.incbin``
in a read-only ELF data object linked into ``bcmbca-xrdp``. No external
firmware file is required at runtime. Generated files stay in the kernel
output directory. A standalone invocation after building is::

  build-codex/drivers/net/ethernet/broadcom/runner-asm/runner-as input.rnr output.bin

Run the host tests from the source tree with::

  python3 drivers/net/ethernet/broadcom/runner-asm/test.py \
    build-codex/drivers/net/ethernet/broadcom/runner-asm/runner-as

After undoing the three-word notification hook and its extension in padding,
the tests check this reference digest for all 8192 instruction words, including
NOP padding, reconstructed from the original U-Boot register-write list::

  924fa9aa8cd5c463a6e913ea7a441f06caad9b1a16b0efb92359245f320d8d6d

The tests also exercise forward/backward labels, independent instruction
vectors, field bounds, ABI assertions and rejection of malformed programs.
They do not execute the instructions or simulate the packet engine.

Source language
---------------

Each instruction occupies four bytes. Labels and ``.`` denote byte
addresses. ``r0`` through ``r31`` denote register selectors; arithmetic
expressions can also supply selectors. Constants use ``.const name = expr``.
Decimal and ``0x`` hexadecimal numbers, parentheses, addition, subtraction,
left shift, AND and OR are supported. Comments start with ``#``. Statements
end with a newline; labels end with a colon.

Instruction operands may refer to forward labels. Constants and layout
directives may only refer to symbols already defined. Expressions and
fields are range-checked, with no implicit truncation. ``.pad address``
fills forward with NOPs; ``.assert_pc address`` rejects changes to a fixed
ABI entry point. The two passes must produce the same symbols and size.
Output is written atomically only after both passes succeed.

Branch labels are relocated only for the ``branch`` and ``jump`` forms.
Some unresolved fields may also contain addresses or displacements. The
current source keeps their positions fixed; it is not generally relocatable.
The host's recorded context initialization supplies task entry PCs and
SRAM addresses separately. Changing that ABI requires corresponding host
changes, not just edits to this assembly file.

Encoding forms
--------------

Bits 31:26 select the observed instruction class. The table specifies bit
positions, not C bitfields. All numerical control fields are retained
explicitly; the assembler does not fill delay slots or schedule hazards.

``nop``
  The vendor defines ``RNR_NOP_OPCODE`` as ``0xfc000000``.

``movi dst, imm16``
  ``0xb0000004 | dst << 20 | imm16 << 4``. The observed immediate-move
  form, including the low control nibble 4. Other move modes are not
  implemented.

``ld32.abs dst, address`` / ``ld64.abs dst, address``
  Base ``0x32000002`` / ``0x32000003``, register bits 24:20 and byte
  address bits 19:4. The 64-bit form transfers an even register pair.
  ``st32.abs`` / ``st64.abs`` use bases ``0x36000002`` / ``0x36000003``.

``load reg, a, b, mode`` / ``store reg, a, b, mode``
  Class 0x0c / 0x0d. Register selector 24:20, address field A 19:15,
  address field B 14:4. Mode bits 3:0 go to instruction bits 3:0;
  mode bit 4 goes to instruction bit 25. The precise address-generation
  and extension semantics of these modes remain unresolved. In the
  source, ``MEM_EXT32`` and ``MEM_EXT64`` name encodings, not a specified
  memory address space or a guaranteed byte displacement.

``alu.add d, a, b, x, y, m``
  Class 0x20, d=24:20, a=19:15, b=9:5, x=14:10, y=4:0, m=25.
  ``alu.sub`` and ``alu.and`` use classes 0x21 and 0x22. These names are
  inferred from counter increments, the SBPM null-buffer comparison and
  FIFO pointer masking. The remaining controls are not fully decoded.

``alu.shift`` / ``alu.mul``
  Classes 0x25 / 0x27, with the same six encoding fields. The shift name
  is inferred from bit-mask construction in the PON interrupt-coalescing
  routine and the reduced image's port-index scaling. Multiplication is
  inferred from counter indices scaled by eight and PON table-address
  calculations. Shift direction, signedness and other controls remain
  explicit; these names do not specify every possible mode.

``field.2a`` / ``field.2b``
  Same six *encoding fields*, with classes 0x2a / 0x2b. These resemble
  insertion/extraction, but their controls are not sufficiently decoded
  to expose semantic bit-position and width operands.

``branch target, control`` / ``jump target, control``
  Class 0x00 / 0x01. Bits 12:0 contain target / 4; bits 25:13 contain
  control. Branch conditions and delay controls remain explicit. The
  ``jump.reg reg, control`` form preserves the observed indirect task
  return encoding, with a five-bit selector in place of the target.
  Its selector is not a code relocation.

``op02``, ``op30``, ``op31``, ``op33``
  The same six encoding fields as the ALU forms, with the indicated
  class number. These fields do **not** imply ALU or register semantics.
  Class 0x02 appears in control flow; 0x30/0x31 appear in debug/control
  sequences; 0x33 appears in peripheral exchanges. Message destinations,
  completion behavior and memory-ordering guarantees remain unresolved.

``op08 control, value`` / ``op09 control, value``
  Class 0x08 / 0x09. Control occupies bits 25:13 and value bits 12:0.
  These occur at task/context boundaries. They are not yet specified as
  context-switch or barrier instructions.

Evidence and limits
-------------------

The reconstruction uses Broadcom's GPL code supplied with the Genexis
BCM6846 sources. Paths below are relative to the vendor ``rdp`` directory
unless stated otherwise:

* U-Boot ``bcmbca/xrdp/data_path_6846.c`` supplies the working image as
  byte-swapped 32-bit writes following an instruction-RAM NOP fill.
* ``projects/XRDP_CFE2/firmware_bin/BCM6846/runner_fw_0.c`` supplies a
  related image with matching initialization and debug sequences.
* ``projects/XRDP_CFE2/drivers/rdd/BCM6846/rdd_runner_labels.h`` places
  its initialization at 0x18 and debug routine at 0x7c. The first two
  vector encodings target exactly these addresses in instruction units.
* The adjacent ``auto/rdd_runner_defs_auto.h`` places global registers at
  0x60, the register-save buffer at 0x1c0, and the task index at 0x16c.
  The load/store operands agree with those addresses. Comparing the debug
  sequence with the reduced image identifies its buffer at 0x2c0 and
  task index at 0xec. Register-pair transfers advance the buffer by eight
  bytes, while the first two single-register transfers advance by four.
* ``rdd_init.c`` initializes global r1 to 1. The CFE2 counter sequence
  uses that selector in its addition; the reduced image instead loads
  r4 with 1 and uses the corresponding changed operand in the same
  load/add/store sequence. This cross-check supports the move and add
  register fields independently of reassembly.
* ``drivers/rdp_subsystem/xrdp/rdp_drv_rnr.c`` defines the NOP and loads
  instruction words in big-endian order.

The manifests name ``debug.uc``, ``cpu_rx.uc``, ``cpu_tx.uc`` and common
firmware macros, but matching sources and ACE were not present in the
examined dump. The older DSL Runner headers contain assembly macros; they
provide vocabulary, not proof of BCM6846 instruction encodings.

The full Linux PON firmware's host setup supplies 0x82d9708c (UBUS_SLV
QUEUE_ITR) as the receive interrupt destination. That establishes a
destination, not a compatible write sequence for this reduced SBPM image.
In ``projects/PON_6846/firmware_bin/runner_fw_1.c``, the routine labelled
``interrupt_coalescing_1st_wakeup_request`` starts at 0x67e4. At 0x6804 it
loads the configured destination from SRAM 0x3628 into r18/r19. A candidate
notification sequence at 0x6848 stores r8 through r14, followed at 0x684c
by class 0x35, with fields d=14, a=18, b=4, x=8, y=0, m=1. The sequence
recurs at 0x688c/0x6890. This suggests an external write of a four-byte
mask, but the field interpretation and completion rules are not proven.
The assembler names this form ``write.ext``, with the same six encoding
fields as the ALU forms. Its register operands and control fields are copied
unchanged in the notification extension. The name describes the inferred
operation; it does not specify the remaining class 0x35 modes.

RX notification extension
-------------------------

The hook at PC 0x23c transfers to 0x334 with two NOP delay slots, using
control 0x1400 as in the reset vector and PON coalescing loop. The three
displaced counter instructions execute first. The FIFO descriptor has
already been published, and its existing producer update is unchanged.
All other original instructions and task entry addresses remain unchanged.
The extension preserves r8, r14 and r18/r19, copies the vendor notification
sequence, then returns to the original recycling path at 0x248. It never
runs on the FIFO-full branch. The zero-enable branch uses the original
initialization routine's compare/equal-jump encoding.

The SRAM mailbox occupies previously unused space between the debug save
area (0x2c0..0x33f) and the descriptor FIFO (0x800..0xbff):

=========== ============================================================
Offset      Contents
=========== ============================================================
0x380       Host enable word: zero or queue-0 mask (1)
0x384       Runner-owned notification mask, retained until reset
0x388       Queue ITR physical address, low word followed by high word
0x390       Runner count of issued notification instructions
0x3a0..3af  RX task's saved r8, r14, r18 and r19
=========== ============================================================

All words use Runner big-endian SRAM order. The host initializes the
address pair before enabling notifications. The mask is immutable while
notifications are enabled and remains valid after disabling them; the
extension does not need to reuse a DMA source buffer before completion.
Only the RX task uses the save area. TX and debug retain their old ABI.

Packet-generated interrupts, patterned traffic and interface restarts
have been tested on BCM6846. The attempt counter alone proves only
execution past the write instruction, not arrival at the host. These
tests do not establish a general Runner memory barrier, external-write
completion semantics or a full ISA specification.

The driver's shared ``rx-irq`` ethtool private flag defaults on when the
Runner mailbox and all queue interrupts are available. Older device trees
without interrupts retain timer-driven receive. Queue 0 starts an RX NAPI
batch. TX timers cannot start a new RX batch. The host masks
notifications and clears ITR/ISR before draining, then unmasks after NAPI
completion without clearing arrivals during the drain. Full-budget polls
and deferred NAPI repolls retain RX ownership. TX completion polling
remains available only while TX work is outstanding. Turning the flag off
restores 1-ms RX polling.
The synthetic IRQ self-test is refused while IRQ receive is enabled so
that it cannot masquerade as packet notifications.

The shared ethtool ``rx-usecs`` setting controls NAPI software IRQ
coalescing, with a default of zero and a range of 0..1000 microseconds.
The first IRQ schedules NAPI immediately. Subsequent polls may defer IRQ
rearming for the configured interval; one empty poll ends the batch.
Zero disables deferral. Coalescing is inactive in polling mode. Because
the engine has one NAPI instance, configuring any MAC port changes the
setting for all ports, including the DSA conduit.

For a direct test, enable the flag on the MAC netdevice (the DSA conduit
for a switch port)::

  ethtool -C eth0 rx-usecs 50
  ethtool --set-priv-flags eth0 rx-irq on
  ethtool -S eth0

Require increasing ``shared_runner_notifications``,
``shared_queue_irq_events`` and ``shared_rx_irq_polls``, unchanged
``shared_rx_polling_polls`` and ``shared_irq_test_events``, and successful
patterned traffic. Test sparse packets after idle, sustained RX, full
budgets, bidirectional traffic and interface restart recovery. Compare RX
throughput, UDP loss, socket drops and CPU consumption with the flag off,
with IRQ receive and zero deferral, and with IRQ coalescing using the same
image. A timer still services TX: its counter alone does not indicate RX
fallback.
