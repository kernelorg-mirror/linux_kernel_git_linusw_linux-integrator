.. SPDX-License-Identifier: GPL-2.0-only

Kernel driver bcm_pmb_pvtmon
============================

Supported chips:

  * Broadcom BCM6846 PVTMON

    Access: Processor Monitor Bus (PMB), bus 1, device 6

Author: Linus Walleij <linusw@kernel.org>

Description
-----------

The process, voltage and temperature monitor (PVTMON) measures die
temperature and seven voltage inputs. The driver uses the shared PMB MFD
transport and obtains its clock and reset from the sibling providers.

With CONFIG_THERMAL_OF enabled, the temperature channel also supplies
device-tree thermal zones. The PVTMON node uses #thermal-sensor-cells = <0>;
voltage inputs do not have thermal sensor indices.

Sysfs attributes
----------------

All attributes are read-only. Temperature is reported in millidegrees
Celsius and voltages in millivolts, using the BCM6846 vendor conversion
formulas and input-divider ratios.

================ ================================
Attribute        Description
================ ================================
temp1_input      Die temperature
in0_input        First 0.85 V input
in1_input        Second 0.85 V input
in2_input        VIN input
in3_input        1.0 V input
in4_input        1.8 V input
in5_input        3.3 V input
in6_input        Diagnostic test input
in0_label        V0.85_1
in1_label        V0.85_2
in2_label        VIN
in3_label        V1.0
in4_label        V1.8
in5_label        V3.3
in6_label        Vtest
================ ================================

Labels follow the vendor channel names. Vtest is a diagnostic ADC input,
not a supply rail. The driver does not change the analog test routing.
